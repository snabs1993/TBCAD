#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "panel_util.h"
#include "panels.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/layer_states.h"

namespace tbcad::layers {

namespace {

using namespace ui_detail;

struct Row {
    LayerId id{};
    int depth = 0;
    bool hasKids = false;
    bool open = true;
};

struct PanelState {
    std::string filter{};
    std::string lastCommandFilter{};  // last layers.filter.serial we applied
    std::string stateName{};
    std::set<std::uint32_t> collapsed{};
    LayerId selected{};
    LayerId renaming{};
    bool focusRename = false;
    std::string renameBuf{};
    std::string pendingRenameName{};  // select + rename the layer with this top-level path next
    LayerId deleteAsk{};              // layer whose delete needs a Branch / MoveUp choice
    LayerId colorFor{};
};

PanelState& state() {
    static PanelState s;
    return s;
}

std::string lowered(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Visible rows: collapsed branches are skipped; with a filter, matches and their ancestors show
// and everything is expanded.
std::vector<Row> buildRows(const Document& doc, const PanelState& st) {
    std::vector<Row> rows;
    std::string f = lowered(st.filter);
    // "name:S-* frozen", "branch:Walls" ... use the filter language of -LAYERFILTER; plain text is
    // a substring of the layer path.
    const bool expression = f.find(':') != std::string::npos;
    auto matches = [&](LayerId id) {
        if (f.empty())
            return true;
        if (expression)
            return layerMatchesFilter(doc, id, st.filter);
        return lowered(layerPath(doc, id)).find(f) != std::string::npos;
    };
    // A layer shows when it or any descendant matches.
    auto subtreeMatches = [&](auto&& self, LayerId id) -> bool {
        if (matches(id))
            return true;
        for (LayerId c : childrenOf(doc, id))
            if (self(self, c))
                return true;
        return false;
    };
    auto visit = [&](auto&& self, LayerId parent, int depth) -> void {
        for (LayerId id : childrenOf(doc, parent)) {
            if (!f.empty() && !subtreeMatches(subtreeMatches, id))
                continue;
            Row r;
            r.id = id;
            r.depth = depth;
            r.hasKids = !childrenOf(doc, id).empty();
            r.open = !f.empty() || !st.collapsed.count(id.value);
            rows.push_back(r);
            if (r.hasKids && r.open)
                self(self, id, depth + 1);
        }
    };
    visit(visit, LayerId{}, 0);
    return rows;
}

std::string uniqueName(const Document& doc, LayerId parent, const std::string& base) {
    for (int n = 1;; ++n) {
        std::string name = base + std::to_string(n);
        if (!doc.layerByName(parent, name))
            return name;
    }
}

void selectObjectsOn(tbcad::ui::PanelContext& ctx, LayerId id) {
    if (!ctx.host || !ctx.doc)
        return;
    std::vector<EntityId> ids;
    ctx.doc->forEachEntity([&](const Entity& e) {
        if (e.common.layer == id && ctx.doc->ownerBlock(e.common.id) == BlockId{})
            ids.push_back(e.common.id);
    });
    ctx.host->setSelection(std::move(ids));
}

void newLayer(tbcad::ui::PanelContext& ctx, LayerId parent) {
    PanelState& st = state();
    std::string name = uniqueName(*ctx.doc, parent, parent ? "SubLayer" : "Layer");
    if (parent)
        exec(ctx, {"-LAYER NewSub " + idRef(parent), name});
    else
        exec(ctx, {"-LAYER New", name});
    st.pendingRenameName = parent ? layerPath(*ctx.doc, parent) + kPathSeparator + name : name;
}

void drawNameCell(tbcad::ui::PanelContext& ctx, const Row& row, bool dimmed, bool& openToggled) {
    PanelState& st = state();
    const Document& doc = *ctx.doc;
    const Layer* l = doc.layer(row.id);
    ImGui::PushID(static_cast<int>(row.id.value));
    ImGui::Indent(row.depth * 14.0f);
    if (row.hasKids) {
        if (ImGui::ArrowButton("##open", row.open ? ImGuiDir_Down : ImGuiDir_Right))
            openToggled = true;
    } else {
        ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
    }
    ImGui::SameLine(0, 2);
    const bool current = doc.header().currentLayer == row.id;

    if (st.renaming == row.id) {
        if (st.focusRename) {
            ImGui::SetKeyboardFocusHere();
            st.focusRename = false;
        }
        ImGui::SetNextItemWidth(-1);
        bool done = ImGui::InputText(
            "##rename", &st.renameBuf,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (done) {
            if (!st.renameBuf.empty() && st.renameBuf != l->name)
                exec(ctx, {"-LAYER Rename " + idRef(row.id), st.renameBuf});
            st.renaming = {};
        } else if (ImGui::IsItemDeactivated()) {
            st.renaming = {};  // Esc or click elsewhere cancels
        }
    } else {
        // The UI font has no filled-dot glyph (it showed as "?"), so mark the current layer in
        // ASCII.
        std::string label = std::string(current ? "> " : "   ") + l->name;
        if (dimmed)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        bool sel = st.selected == row.id;
        if (ImGui::Selectable(
                label.c_str(), sel,
                ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap)) {
            st.selected = row.id;
            if (ImGui::IsMouseDoubleClicked(0))
                exec(ctx, {"-LAYER Set " + idRef(row.id)});
        }
        if (dimmed)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
            st.renaming = row.id;
            st.renameBuf = l->name;
            st.focusRename = true;
        }

        // Drag to re-parent.
        if (row.id != kLayerZero &&
            ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            std::uint32_t v = row.id.value;
            ImGui::SetDragDropPayload("TBCAD_LAYER", &v, sizeof v);
            ImGui::TextUnformatted(layerPath(doc, row.id).c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* peek = ImGui::AcceptDragDropPayload(
                "TBCAD_LAYER",
                ImGuiDragDropFlags_AcceptPeekOnly | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            if (peek && peek->DataSize == sizeof(std::uint32_t)) {
                LayerId src{*static_cast<const std::uint32_t*>(peek->Data)};
                std::string why = whyCannotReparent(doc, src, row.id);
                if (why.empty()) {
                    // Drop line / highlight on the target row.
                    ImGui::GetWindowDrawList()->AddRect(
                        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                        ImGui::GetColorU32(ImGuiCol_DragDropTarget), 0.0f, 0, 2.0f);
                    if (ImGui::IsMouseReleased(0)) {
                        exec(ctx, {"-LAYER Parent " + idRef(src), idRef(row.id)});
                    }
                } else {
                    ImGui::GetWindowDrawList()->AddRect(
                        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                        ImGui::GetColorU32(ImVec4(0.9f, 0.25f, 0.25f, 1.0f)), 0.0f, 0, 2.0f);
                    ImGui::SetTooltip("%s", why.c_str());
                    if (ImGui::IsMouseReleased(0) && ctx.host)
                        ctx.host->print("Cannot move layer: " + why);
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextItem("##ctx")) {
            st.selected = row.id;
            if (ImGui::MenuItem("New"))
                newLayer(ctx, {});
            if (row.id != kLayerZero && ImGui::MenuItem("New Sub-layer"))
                newLayer(ctx, row.id);
            if (row.id != kLayerZero && ImGui::MenuItem("Rename", "F2")) {
                st.renaming = row.id;
                st.renameBuf = l->name;
                st.focusRename = true;
            }
            if (ImGui::MenuItem("Set Current"))
                exec(ctx, {"-LAYER Set " + idRef(row.id)});
            if (ImGui::MenuItem("Isolate"))
                exec(ctx, {"-LAYER Isolate " + idRef(row.id)});
            if (ImGui::MenuItem("Show All"))
                exec(ctx, {"-LAYER ShowAll"});
            if (ImGui::MenuItem("Select Objects"))
                selectObjectsOn(ctx, row.id);
            ImGui::Separator();
            if (row.id != kLayerZero && ImGui::MenuItem("Delete")) {
                if (row.hasKids)
                    st.deleteAsk = row.id;
                else
                    exec(ctx, {"-LAYER Delete " + idRef(row.id)});
            }
            ImGui::EndPopup();
        }
    }
    ImGui::Unindent(row.depth * 14.0f);
    ImGui::PopID();
}

void drawLinetypeCell(tbcad::ui::PanelContext& ctx, const Layer& l) {
    const Document& doc = *ctx.doc;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##lt", linetypeName(doc, l.linetype).c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        if (l.parent && ImGui::Selectable("ByParent", l.linetype == kLinetypeByParent))
            exec(ctx, {"-LAYER Linetype ByParent", idRef(l.id)});
        for (const auto& [id, lt] : doc.linetypes()) {
            if (ImGui::Selectable(lt.name.c_str(), l.linetype == id))
                exec(ctx, {"-LAYER Linetype #" + std::to_string(id.value), idRef(l.id)});
        }
        ImGui::Separator();
        if (ImGui::Selectable("Load more linetypes..."))
            exec(ctx, {"-LINETYPE Load", "*"});
        ImGui::EndCombo();
    }
}

void drawLineweightCell(tbcad::ui::PanelContext& ctx, const Layer& l) {
    static const char* const kWeights[] = {"0.00", "0.05", "0.09", "0.13", "0.15", "0.18",
                                           "0.20", "0.25", "0.30", "0.35", "0.40", "0.50",
                                           "0.60", "0.70", "0.80", "1.00", "1.40", "2.00"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##lw", lineweightName(l.lineweight).c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        if (l.parent && ImGui::Selectable("ByParent", l.lineweight.isByParent()))
            exec(ctx, {"-LAYER Lineweight ByParent", idRef(l.id)});
        if (ImGui::Selectable("Default", l.lineweight.isDefault()))
            exec(ctx, {"-LAYER Lineweight Default", idRef(l.id)});
        for (const char* w : kWeights) {
            bool sel =
                l.lineweight.isValue() && std::abs(l.lineweight.toMm() - std::atof(w)) < 1e-6;
            if (ImGui::Selectable((std::string(w) + " mm").c_str(), sel))
                exec(ctx, {std::string("-LAYER Lineweight ") + w, idRef(l.id)});
        }
        ImGui::EndCombo();
    }
}

}  // namespace

void drawLayersPanel(tbcad::ui::PanelContext& ctx) {
    if (!ctx.doc)
        return;
    PanelState& st = state();
    const Document& doc = *ctx.doc;

    // A layer created from this panel starts in rename mode.
    if (!st.pendingRenameName.empty()) {
        if (auto id = resolveLayer(doc, st.pendingRenameName)) {
            st.selected = id.value();
            st.renaming = id.value();
            st.renameBuf = doc.layer(id.value())->name;
            st.focusRename = true;
            st.pendingRenameName.clear();
        } else if (ImGui::GetFrameCount() % 120 == 0) {
            st.pendingRenameName.clear();  // the creation was refused; give up
        }
    }

    // -LAYERFILTER sets the host setting layers.filter; the panel follows it.
    if (ctx.host) {
        const std::string serial = ctx.host->setting("layers.filter.serial").value_or("");
        if (serial != st.lastCommandFilter) {
            st.lastCommandFilter = serial;
            st.filter = ctx.host->setting("layers.filter").value_or("");
        }
    }

    // Toolbar.
    if (ImGui::Button("New"))
        newLayer(ctx, {});
    ImGui::SameLine();
    const bool canSub = st.selected && st.selected != kLayerZero && doc.layer(st.selected);
    ImGui::BeginDisabled(!canSub);
    if (ImGui::Button("New Sub-layer"))
        newLayer(ctx, st.selected);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool canDelete = st.selected && st.selected != kLayerZero && doc.layer(st.selected);
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::Button("Delete")) {
        if (!childrenOf(doc, st.selected).empty())
            st.deleteAsk = st.selected;
        else
            exec(ctx, {"-LAYER Delete " + idRef(st.selected)});
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter layers (text, or name:S-* frozen ...)",
                             &st.filter);

    // Named layer states: pick one to restore it, or save the current state under a new name.
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##states", "Layer states...")) {
        for (const std::string& n : layerStateNames(doc))
            if (ImGui::Selectable(n.c_str()))
                exec(ctx, {"-LAYERSTATE", "Restore", n, "All"});
        ImGui::Separator();
        ImGui::SetNextItemWidth(120);
        ImGui::InputTextWithHint("##newstate", "Save as", &st.stateName);
        ImGui::SameLine();
        if (ImGui::Button("Save") && !st.stateName.empty()) {
            exec(ctx, {"-LAYERSTATE", "Save", st.stateName});
            st.stateName.clear();
        }
        ImGui::EndCombo();
    }

    std::vector<Row> rows = buildRows(doc, st);

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Hideable;
    const float footer = ImGui::GetTextLineHeightWithSpacing() + 6;
    if (ImGui::BeginTable("##layers", 8, kFlags, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Frz", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("Lck", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("Colour", ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("Linetype", ImGuiTableColumnFlags_WidthFixed, 96);
        ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthFixed, 84);
        ImGui::TableSetupColumn("Plot", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Row& row = rows[static_cast<std::size_t>(i)];
                const Layer* l = doc.layer(row.id);
                if (!l)
                    continue;
                EffectiveLayer eff = doc.effective(row.id);
                // Greyed when an ancestor (not the layer itself) hides or freezes it.
                bool dimmed = (!eff.visible() && l->on && !l->frozen) || eff.inactive;
                ImGui::PushID(static_cast<int>(row.id.value));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool toggled = false;
                drawNameCell(ctx, row, dimmed, toggled);
                if (toggled) {
                    if (st.collapsed.count(row.id.value))
                        st.collapsed.erase(row.id.value);
                    else
                        st.collapsed.insert(row.id.value);
                }

                if (dimmed)
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                auto flag = [&](int col, const char* id, bool value, const char* onCmd,
                                const char* offCmd, const char* tip) {
                    ImGui::TableSetColumnIndex(col);
                    bool v = value;
                    if (ImGui::Checkbox(id, &v))
                        exec(ctx,
                             {std::string("-LAYER ") + (v ? onCmd : offCmd) + " " + idRef(row.id)});
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tip);
                };
                flag(1, "##on", l->on, "On", "Off",
                     l->on ? "Visible; click to turn off" : "Off; click to turn on");
                flag(2, "##frz", l->frozen, "Freeze", "Thaw",
                     "Frozen layers are hidden and skipped when regenerating");
                flag(3, "##lck", l->locked, "Lock", "Unlock",
                     "Locked layers show but cannot be edited");
                ImGui::TableSetColumnIndex(4);
                {
                    ImVec4 col = swatch(eff.color);
                    if (ImGui::ColorButton(
                            "##col", col,
                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
                            ImVec2(22, ImGui::GetFrameHeight() - 4))) {
                        st.colorFor = row.id;
                        ImGui::OpenPopup("##colour");
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "%s%s", colorName(l->color).c_str(),
                            l->color.kind == ColorKind::ByParent ? " (from parent)" : "");
                    if (st.colorFor == row.id) {
                        ColorChoices allowed;
                        allowed.byParent = l->parent != LayerId{};
                        std::string chosen;
                        if (colorPopup("##colour", l->color, allowed, chosen)) {
                            exec(ctx, {"-LAYER Color " + chosen, idRef(row.id)});
                            st.colorFor = {};
                        }
                    }
                }
                ImGui::TableSetColumnIndex(5);
                drawLinetypeCell(ctx, *l);
                ImGui::TableSetColumnIndex(6);
                drawLineweightCell(ctx, *l);
                flag(7, "##plot", l->plot, "Plot", "NoPlot", "Plot this layer");
                if (dimmed)
                    ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    {
        bool ao = doc.header().activeOnly;
        // The command finishes after its one answer: no closing empty line (it would repeat it).
        if (ImGui::Checkbox("Active layer only", &ao) && ctx.runCommand)
            ctx.runCommand(ao ? "ACTIVEONLY On" : "ACTIVEONLY Off");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Only the current layer and its sub-layers can be drawn on,\n"
                "picked or edited. Other layers stay visible but are locked.");
    }

    // Dropping on the empty space under the list moves a layer to the top level.
    ImGui::TextDisabled("%zu layers", doc.layers().size());
    ImGui::SameLine();
    ImGui::TextDisabled("  drag a layer onto another to make it a sub-layer");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("TBCAD_LAYER")) {
            LayerId src{*static_cast<const std::uint32_t*>(p->Data)};
            std::string why = whyCannotReparent(doc, src, {});
            if (why.empty())
                exec(ctx, {"-LAYER Parent " + idRef(src), "None"});
            else if (ctx.host)
                ctx.host->print("Cannot move layer: " + why);
        }
        ImGui::EndDragDropTarget();
    }

    // Delete a layer that has sub-layers: ask what to do with them.
    if (st.deleteAsk) {
        ImGui::OpenPopup("Delete layer");
    }
    if (ImGui::BeginPopupModal("Delete layer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const Layer* l = doc.layer(st.deleteAsk);
        if (!l) {
            st.deleteAsk = {};
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("\"%s\" has sub-layers.", l->name.c_str());
            ImGui::TextWrapped(
                "Delete the whole branch (every layer in it must be empty), "
                "or delete only this layer and move its sub-layers up one level.");
            if (ImGui::Button("Delete branch")) {
                exec(ctx, {"-LAYER Delete " + idRef(st.deleteAsk), "Branch"});
                st.deleteAsk = {};
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Move sub-layers up")) {
                exec(ctx, {"-LAYER Delete " + idRef(st.deleteAsk), "MoveUp"});
                st.deleteAsk = {};
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                st.deleteAsk = {};
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

}  // namespace tbcad::layers
