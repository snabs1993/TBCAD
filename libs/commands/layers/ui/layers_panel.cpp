#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#include "panel_util.h"
#include "panels.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/layer_states.h"
#include "tbcad/layout/layout.h"

namespace tbcad::layers {

namespace {

using namespace ui_detail;

struct Row {
    LayerId id{};
    int depth = 0;
    bool hasKids = false;
    bool open = true;
    std::string group{};      // header row of an external reference's layers (no layer id)
    std::string shortName{};  // layer name without the "xref|" prefix, for rows under a header
    std::size_t members = 0;  // layers under a header row
};

// External-reference layers are named "<xref>|<layer>" at the top level.
std::string xrefPrefix(const Layer& l) {
    if (l.parent)
        return {};
    const auto bar = l.name.find('|');
    return bar == std::string::npos || bar == 0 ? std::string() : l.name.substr(0, bar);
}

struct PanelState {
    std::string filter{};
    std::string lastCommandFilter{};  // last layers.filter.serial we applied
    std::string stateName{};
    bool invert = false;        // show the layers the filter does NOT match
    std::string newName{};      // text field of the "Save filter" / "New group" popups
    std::string stateRename{};  // new name typed for a layer state
    bool restoreHide = false;   // "turn off layers not in the state" on Restore
    std::set<std::uint32_t> collapsed{};
    std::set<std::string> collapsedXrefs{};  // external references whose layers are folded away
    LayerId selected{};
    std::set<std::uint32_t> multi{};     // Ctrl / Shift click selection; always holds `selected`
    LayerId anchor{};                    // where a Shift range starts
    std::vector<std::uint32_t> order{};  // visible rows, top to bottom (for Shift ranges)
    LayerId renaming{};
    bool focusRename = false;
    std::string renameBuf{};
    std::string pendingRenameName{};  // select + rename the layer with this top-level path next
    LayerId deleteAsk{};              // layer whose delete needs a Branch / MoveUp choice
    LayerId colorFor{};
    LayerId reveal{};  // scroll to this row (and keep it selected) once, e.g. after a rename
};

PanelState& state() {
    static PanelState s;
    return s;
}

// The selected layers as a command argument ("#3,#7"); the clicked row alone when none is chosen.
std::string selectedRefs(const PanelState& st) {
    std::string out;
    for (std::uint32_t v : st.multi)
        out += (out.empty() ? "" : ",") + idRef(LayerId{v});
    return out.empty() ? idRef(st.selected) : out;
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
        bool hit;
        if (expression)
            hit = layerMatchesFilter(doc, id, st.filter);
        else
            hit = lowered(layerPath(doc, id)).find(f) != std::string::npos;
        return st.invert ? !hit : hit;
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
    std::map<std::string, std::vector<LayerId>> xrefs;  // external reference -> its layers
    auto visit = [&](auto&& self, LayerId parent, int depth) -> void {
        for (LayerId id : childrenOf(doc, parent)) {
            if (const Layer* l = doc.layer(id); l && depth == 0) {
                if (std::string x = xrefPrefix(*l); !x.empty()) {
                    if (f.empty() || subtreeMatches(subtreeMatches, id))
                        xrefs[x].push_back(id);
                    continue;
                }
            }
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
    // The layers of each external reference sit under one folded header, as in AutoCAD.
    for (const auto& [name, ids] : xrefs) {
        Row header;
        header.group = name;
        header.hasKids = true;
        header.open = !f.empty() || !st.collapsedXrefs.count(name);
        header.members = ids.size();
        rows.push_back(header);
        if (!header.open)
            continue;
        for (LayerId id : ids) {
            Row r;
            r.id = id;
            r.depth = 1;
            r.shortName = doc.layer(id)->name.substr(name.size() + 1);
            rows.push_back(std::move(r));
        }
    }
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

// The layout viewport that VP Freeze acts on: the one selected on the sheet, else the only one.
struct VpTarget {
    std::string layout{};
    int id = 0;
    std::vector<LayerId> frozen{};
    explicit operator bool() const { return id != 0; }
};

VpTarget viewportTarget(const tbcad::ui::PanelContext& ctx) {
    VpTarget t;
    if (!ctx.host || !ctx.doc || ctx.host->drawingOwner() == BlockId{})
        return t;
    std::string name = ctx.host->setting("layout.current").value_or("");
    auto lay = name.empty() ? std::nullopt : layout::findLayout(*ctx.doc, name);
    if (!lay) {
        auto all = layout::layouts(*ctx.doc);
        if (all.empty())
            return t;
        lay = all.front();
    }
    const layout::Viewport* vp = nullptr;
    const std::string sel = ctx.host->setting("tmp.layout.viewport").value_or("");
    if (const auto colon = sel.find(':');
        colon != std::string::npos && sel.substr(0, colon) == lay->name)
        vp = lay->findViewport(std::atoi(sel.c_str() + colon + 1));
    if (!vp && lay->viewports.size() == 1)
        vp = &lay->viewports.front();
    if (!vp)
        return t;
    t.layout = lay->name;
    t.id = vp->id;
    t.frozen = vp->frozen;
    return t;
}

// A name wider than its cell keeps its end ("...OUT$0$A-WALL"): the start is what repeats.
std::string fitTail(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width)
        return text;
    std::size_t cut = 0;
    while (cut < text.size()) {
        ++cut;
        while (cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            ++cut;
        const std::string t = "..." + text.substr(cut);
        if (ImGui::CalcTextSize(t.c_str()).x <= width)
            return t;
    }
    return "...";
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
    if (st.reveal == row.id && st.renaming != row.id) {
        ImGui::SetScrollHereY(0.5f);
        st.reveal = {};
    }

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
            // The list re-sorts under the new name: keep the layer selected and in view so the
            // next click lands where the user expects.
            st.selected = row.id;
            st.multi = {row.id.value};
            st.reveal = row.id;
        } else if (ImGui::IsItemDeactivated()) {
            st.renaming = {};  // Esc or click elsewhere cancels
        }
    } else {
        // The UI font has no filled-dot glyph (it showed as "?"), so mark the current layer in
        // ASCII.
        const std::string full = row.shortName.empty() ? l->name : row.shortName;
        const std::string mark = current ? "> " : "   ";
        const std::string label = mark + fitTail(full, ImGui::GetContentRegionAvail().x -
                                                           ImGui::CalcTextSize(mark.c_str()).x - 4);
        if (dimmed)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        bool sel = st.selected == row.id || st.multi.count(row.id.value) > 0;
        if (ImGui::Selectable(
                label.c_str(), sel,
                ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap)) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl) {
                if (!st.multi.erase(row.id.value))
                    st.multi.insert(row.id.value);
                st.anchor = row.id;
            } else if (io.KeyShift && st.anchor) {
                auto a = std::find(st.order.begin(), st.order.end(), st.anchor.value);
                auto b = std::find(st.order.begin(), st.order.end(), row.id.value);
                st.multi.clear();
                if (a != st.order.end() && b != st.order.end()) {
                    if (a > b)
                        std::swap(a, b);
                    st.multi.insert(a, b + 1);
                } else {
                    st.multi.insert(row.id.value);
                }
            } else {
                st.multi = {row.id.value};
                st.anchor = row.id;
            }
            st.selected = row.id;
            if (ImGui::IsMouseDoubleClicked(0))
                exec(ctx, {"-LAYER Set " + idRef(row.id)});
        }
        if (dimmed)
            ImGui::PopStyleColor();
        // The name cell is narrow in a docked panel: the tooltip always shows the whole path.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !ImGui::IsMouseDown(0))
            ImGui::SetTooltip("%s", layerPath(doc, row.id).c_str());
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
            if (!st.multi.count(row.id.value))
                st.multi = {row.id.value};
            st.selected = row.id;
            const std::size_t many = st.multi.size();
            const std::string group = many > 1 ? " (" + std::to_string(many) + " layers)" : "";
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
            if (ImGui::MenuItem(("Isolate" + group).c_str()))
                exec(ctx, {"-LAYER Isolate " + selectedRefs(st)});
            if (many > 1) {
                if (ImGui::MenuItem(("Turn Off" + group).c_str()))
                    exec(ctx, {"-LAYER Off " + selectedRefs(st)});
                if (ImGui::MenuItem(("Turn On" + group).c_str()))
                    exec(ctx, {"-LAYER On " + selectedRefs(st)});
            }
            if (ImGui::MenuItem("Show All"))
                exec(ctx, {"-LAYER ShowAll"});
            if (ImGui::MenuItem("Select Objects"))
                selectObjectsOn(ctx, row.id);
            ImGui::Separator();
            if (ImGui::BeginMenu(("Add to Group" + group).c_str())) {
                for (const std::string& g : layerGroupNames(*ctx.doc))
                    if (ImGui::MenuItem(g.c_str()))
                        exec(ctx, {"-LAYERFILTER AddTo " + g, selectedRefs(st)});
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(("Merge" + group + " Into").c_str())) {
                for (LayerId target : layerTreeOrder(*ctx.doc)) {
                    if (st.multi.count(target.value))
                        continue;
                    std::string label(static_cast<std::size_t>(layerDepth(*ctx.doc, target)) * 2,
                                      ' ');
                    label += ctx.doc->layer(target)->name;
                    if (ImGui::MenuItem(label.c_str()))
                        exec(ctx, {"-LAYER Merge " + selectedRefs(st), idRef(target)});
                }
                ImGui::EndMenu();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Move everything on the selected layers to the chosen layer,\n"
                    "then delete the selected layers");
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

void drawTransparencyCell(tbcad::ui::PanelContext& ctx, const Layer& l) {
    ImGui::SetNextItemWidth(-1);
    const std::string shown = std::to_string(l.transparency) + "%";
    if (ImGui::BeginCombo("##tr", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int v = 0; v <= 90; v += 10)
            if (ImGui::Selectable((std::to_string(v) + "%").c_str(), l.transparency == v))
                exec(ctx, {"-LAYER Transparency " + std::to_string(v), idRef(l.id)});
        ImGui::EndCombo();
    }
}

void drawDescriptionCell(tbcad::ui::PanelContext& ctx, const Layer& l) {
    static LayerId editing{};
    static std::string buf;
    ImGui::SetNextItemWidth(-1);
    if (editing != l.id) {
        std::string shown = l.description;
        ImGui::InputText("##desc", &shown);
        if (ImGui::IsItemActivated()) {
            editing = l.id;
            buf = l.description;
        }
        return;
    }
    ImGui::InputText("##desc", &buf);
    if (ImGui::IsItemDeactivated()) {
        if (buf != l.description)
            exec(ctx, {"-LAYER Description", buf.empty() ? std::string("-") : buf, idRef(l.id)});
        editing = LayerId{};
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
            st.reveal = id.value();
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

    // Toolbar. Buttons wrap onto the next row when the panel is narrow, so none is clipped.
    const auto nextButton = [](const char* label) {
        const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetContentRegionAvail().x > w + ImGui::GetStyle().ItemSpacing.x)
            ImGui::SameLine();
    };
    if (ImGui::Button("New"))
        newLayer(ctx, {});
    nextButton("New Sub-layer");
    const bool canSub = st.selected && st.selected != kLayerZero && doc.layer(st.selected);
    ImGui::BeginDisabled(!canSub);
    if (ImGui::Button("New Sub-layer"))
        newLayer(ctx, st.selected);
    ImGui::EndDisabled();
    nextButton("Delete");
    const bool canDelete = st.selected && st.selected != kLayerZero && doc.layer(st.selected);
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::Button("Delete")) {
        if (!childrenOf(doc, st.selected).empty())
            st.deleteAsk = st.selected;
        else
            exec(ctx, {"-LAYER Delete " + idRef(st.selected)});
    }
    ImGui::EndDisabled();
    nextButton("Isolate");
    ImGui::BeginDisabled(!st.selected || !doc.layer(st.selected));
    if (ImGui::Button("Isolate"))
        exec(ctx, {"-LAYER Isolate " + selectedRefs(st)});
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Turn off every layer except the selected ones\n"
            "(Ctrl+click / Shift+click to select several)");
    ImGui::EndDisabled();
    nextButton("Show All");
    if (ImGui::Button("Show All"))
        exec(ctx, {"-LAYER ShowAll"});
    // Filter drop-down: ready-made filters, the drawing's saved filters and group filters.
    ImGui::SetNextItemWidth(std::min(180.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##filters", "Filters...")) {
        auto pick = [&](const char* label, const std::string& expr) {
            if (ImGui::Selectable(label))
                st.filter = expr;
        };
        pick("All layers", "");
        pick("All used layers", "used");
        pick("Unused layers", "empty");
        pick("Layers that are off", "off");
        pick("Frozen layers", "frozen");
        pick("Locked layers", "locked");
        if (const auto names = layerFilterNames(doc); !names.empty()) {
            ImGui::SeparatorText("Saved filters");
            for (const std::string& n : names)
                pick(n.c_str(), "filter:" + n);
        }
        if (const auto groups = layerGroupNames(doc); !groups.empty()) {
            ImGui::SeparatorText("Groups");
            for (const std::string& n : groups)
                pick(n.c_str(), "group:" + n);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Invert", &st.invert);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show the layers the filter does not match");
    ImGui::SameLine();
    const bool haveFilter = !st.filter.empty();
    ImGui::BeginDisabled(!haveFilter);
    if (ImGui::Button("Save filter..."))
        ImGui::OpenPopup("##savefilter");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!st.selected || !doc.layer(st.selected));
    if (ImGui::Button("New group..."))
        ImGui::OpenPopup("##newgroup");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Make a group filter from the selected layers");
    ImGui::EndDisabled();
    for (const char* popup : {"##savefilter", "##newgroup"}) {
        if (!ImGui::BeginPopup(popup))
            continue;
        const bool isFilter = std::string(popup) == "##savefilter";
        ImGui::TextUnformatted(isFilter ? "Save the current filter as"
                                        : "Group of the selected layers");
        ImGui::SetNextItemWidth(180);
        const bool enter =
            ImGui::InputText("##name", &st.newName, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Save") || enter) && !st.newName.empty()) {
            if (isFilter)
                exec(ctx, {"-LAYERFILTER Save " + st.newName, st.filter});
            else
                exec(ctx, {"-LAYERFILTER Group " + st.newName, selectedRefs(st)});
            st.newName.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter layers (text, or name:S-* frozen ...)",
                             &st.filter);

    // Named layer states: pick one to restore it, or save the current state under a new name.
    ImGui::SetNextItemWidth(std::min(220.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##states", "Layer states...")) {
        ImGui::Checkbox("Turn off layers not in the state", &st.restoreHide);
        for (const std::string& n : layerStateNames(doc)) {
            ImGui::PushID(n.c_str());
            if (ImGui::Selectable(n.c_str()))
                exec(ctx, {"-LAYERSTATE", "Restore", n, st.restoreHide ? "HideOthers" : "All"});
            if (ImGui::BeginPopupContextItem("##stctx")) {
                for (const char* part : {"Visibility", "Lock", "Plot", "Look"})
                    if (ImGui::MenuItem((std::string("Restore ") + part + " only").c_str()))
                        exec(ctx, {"-LAYERSTATE", "Restore", n, part});
                if (ImGui::MenuItem("Update from current layers"))
                    exec(ctx, {"-LAYERSTATE", "Save", n});
                ImGui::SetNextItemWidth(140);
                if (ImGui::InputTextWithHint("##rn", "Rename to", &st.stateRename,
                                             ImGuiInputTextFlags_EnterReturnsTrue) &&
                    !st.stateRename.empty()) {
                    exec(ctx, {"-LAYERSTATE", "Rename", n, st.stateRename});
                    st.stateRename.clear();
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Delete")) {
                    exec(ctx, {"-LAYERSTATE", "Delete", n});
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (!layerStateNames(doc).empty())
            ImGui::TextDisabled("Right-click a state for more.");
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
    st.order.clear();
    for (const Row& r : rows)
        st.order.push_back(r.id.value);

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Hideable;
    const float footer = ImGui::GetTextLineHeightWithSpacing() + 6;
    const float availWidth = ImGui::GetContentRegionAvail().x;
    if (ImGui::BeginTable("##layers2", 11, kFlags, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Fixed width: the table scrolls sideways in a narrow panel instead of squeezing the names
        // away.
        // Name takes whatever the other columns leave, so a wide panel shows whole layer names.
        // In a narrow panel Linetype and Weight start hidden (right-click the header to show them).
        const bool narrow = availWidth < 560;
        const VpTarget vp = viewportTarget(ctx);
        const float others = 30 * 3 + 40 + 34 + (narrow ? 0 : 72 + 56) + (vp ? 40 : 0) +
                             (narrow ? 6 : 8) * ImGui::GetStyle().CellPadding.x * 2;
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed,
                                std::max(150.0f, availWidth - others));
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Frz", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("VP Frz", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("Lck", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Colour", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn(
            "Linetype",
            ImGuiTableColumnFlags_WidthFixed | (narrow ? ImGuiTableColumnFlags_DefaultHide : 0),
            72);
        ImGui::TableSetupColumn(
            "Weight",
            ImGuiTableColumnFlags_WidthFixed | (narrow ? ImGuiTableColumnFlags_DefaultHide : 0),
            56);
        ImGui::TableSetupColumn("Plot", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn(
            "Transp.", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 56);
        ImGui::TableSetupColumn(
            "Description", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultHide);
        // VP Frz only exists while a layout viewport is the target.
        static bool vpShown = false;
        if (vpShown != static_cast<bool>(vp) || ImGui::GetFrameCount() < 3) {
            ImGui::TableSetColumnEnabled(3, static_cast<bool>(vp));
            vpShown = static_cast<bool>(vp);
        }
        const float headerTop = ImGui::GetCursorScreenPos().y;
        ImGui::TableHeadersRow();
        {
            const int col = ImGui::TableGetHoveredColumn();
            const float my = ImGui::GetMousePos().y;
            const char* tip = col == 3   ? "Freeze this layer in the selected layout viewport only"
                              : col == 2 ? "Freeze in the whole drawing"
                              : col == 1 ? "Show or hide the layer"
                              : col == 4 ? "Lock: visible but cannot be edited"
                                         : nullptr;
            if (tip && my >= headerTop && my <= headerTop + ImGui::GetFrameHeight())
                ImGui::SetTooltip("%s", tip);
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        if (state().reveal)
            for (std::size_t ri = 0; ri < rows.size(); ++ri)
                if (rows[ri].id == state().reveal)
                    clipper.IncludeItemByIndex(static_cast<int>(ri));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Row& row = rows[static_cast<std::size_t>(i)];
                if (!row.group.empty()) {
                    ImGui::PushID(row.group.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::ArrowButton("##open", row.open ? ImGuiDir_Down : ImGuiDir_Right)) {
                        if (!st.collapsedXrefs.erase(row.group))
                            st.collapsedXrefs.insert(row.group);
                    }
                    ImGui::SameLine(0, 2);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("%s  (xref, %zu)", row.group.c_str(), row.members);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("External reference %s: %zu layers", row.group.c_str(),
                                          row.members);
                    ImGui::PopID();
                    continue;
                }
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
                if (vp) {
                    ImGui::TableSetColumnIndex(3);
                    const bool fz =
                        std::find(vp.frozen.begin(), vp.frozen.end(), row.id) != vp.frozen.end();
                    bool v = fz;
                    if (ImGui::Checkbox("##vpfrz", &v)) {
                        ctx.host->setSetting("tmp.layout.viewport",
                                             vp.layout + ":" + std::to_string(vp.id));
                        exec(ctx, {std::string("VPLAYER ") + (v ? "Freeze" : "Thaw"), l->name});
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Freeze in viewport %d only (%s)", vp.id,
                                          fz ? "frozen" : "visible");
                }
                flag(4, "##lck", l->locked, "Lock", "Unlock",
                     "Locked layers show but cannot be edited");
                ImGui::TableSetColumnIndex(5);
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
                ImGui::TableSetColumnIndex(6);
                drawLinetypeCell(ctx, *l);
                ImGui::TableSetColumnIndex(7);
                drawLineweightCell(ctx, *l);
                flag(8, "##plot", l->plot, "Plot", "NoPlot", "Plot this layer");
                ImGui::TableSetColumnIndex(9);
                drawTransparencyCell(ctx, *l);
                ImGui::TableSetColumnIndex(10);
                drawDescriptionCell(ctx, *l);
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
