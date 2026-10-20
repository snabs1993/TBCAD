#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

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
    bool invert = false;    // show the layers the filter does NOT match
    std::string newName{};  // text field of the "Save filter" / "New group" popups
    // The property filter editor: one choice per switch (0 any, 1 yes, 2 no) and text fields.
    struct PropFilter {
        std::string name, pattern, colour, linetype, lineweight;
        int on = 0, frozen = 0, locked = 0, plot = 0, used = 0;
    } prop{};
    std::string stateRename{};  // new name typed for a layer state
    bool restoreHide = false;   // "turn off layers not in the state" on Restore
    // Layer States Manager dialog.
    std::string mgrSel{};   // the state picked in the list
    std::string mgrName{};  // text field for the new name / the rename
    std::string lasPath{};  // file for Import / Export
    bool mgrVisibility = true, mgrLock = true, mgrPlot = true, mgrLook = true, mgrCurrent = true;
    // Layer Translator dialog.
    std::string trFilter{};
    std::map<std::uint32_t, std::string> trNew{};  // typed replacement name per layer
    std::set<std::uint32_t> collapsed{};
    // Column the rows are sorted by (-1 or 0: by name, A-Z); sub-layers sort among their
    // siblings. Set from the table header, which remembers it with the panel's layout.
    int sortColumn = -1;
    bool sortDescending = false;
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
    // A filter made only of state words ("off", "frozen locked", "not:used") is an expression too;
    // that is what the Filters... drop-down sets.
    auto onlyStateWords = [&] {
        static const char* const kWords[] = {"on",     "off",      "frozen", "thawed",
                                             "locked", "unlocked", "plot",   "noplot",
                                             "used",   "empty",    "current"};
        std::istringstream in(f);
        std::string w;
        bool any = false;
        while (in >> w) {
            if (w.compare(0, 4, "not:") == 0)
                w = w.substr(4);
            if (std::find_if(std::begin(kWords), std::end(kWords),
                             [&](const char* k) { return w == k; }) == std::end(kWords))
                return false;
            any = true;
        }
        return any;
    };
    const bool expression = f.find(':') != std::string::npos || onlyStateWords();
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
    auto sorted = [&](std::vector<LayerId> ids) {
        if (st.sortColumn < 0 || (st.sortColumn == 0 && !st.sortDescending))
            return ids;  // childrenOf already lists them by name
        auto key = [&](const Layer& l) -> std::string {
            char buf[16];
            switch (st.sortColumn) {
                case 1:
                    return l.on ? "1" : "0";
                case 2:
                    return l.frozen ? "1" : "0";
                case 4:
                    return l.locked ? "1" : "0";
                case 5:
                    if (l.color.kind == ColorKind::Index) {
                        std::snprintf(buf, sizeof buf, "0%03d", l.color.indexValue());
                        return buf;
                    }
                    std::snprintf(buf, sizeof buf, "1%02x%02x%02x", l.color.r, l.color.g,
                                  l.color.b);
                    return buf;
                case 6:
                    return lowered(linetypeName(doc, l.linetype));
                case 7:
                    std::snprintf(buf, sizeof buf, "%05d", l.lineweight.v + 10);
                    return buf;
                case 8:
                    return l.plot ? "1" : "0";
                case 9:
                    std::snprintf(buf, sizeof buf, "%03d", l.transparency);
                    return buf;
                case 10:
                    return lowered(l.description);
                default:
                    return {};
            }
        };
        std::stable_sort(ids.begin(), ids.end(), [&](LayerId a, LayerId b) {
            const Layer* la = doc.layer(a);
            const Layer* lb = doc.layer(b);
            if (!la || !lb)
                return false;
            std::string ka = key(*la), kb = key(*lb);
            if (ka == kb) {  // ties (and the Name column) go by name
                ka = lowered(la->name);
                kb = lowered(lb->name);
                if (ka == kb)
                    return false;
            }
            return st.sortDescending ? kb < ka : ka < kb;
        });
        return ids;
    };
    auto visit = [&](auto&& self, LayerId parent, int depth) -> void {
        for (LayerId id : sorted(childrenOf(doc, parent))) {
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
    if (!ctx.host || !ctx.doc)
        return t;
    // In model space: the viewport being edited (entered by a double click), if any.
    const bool model = ctx.host->drawingOwner() == BlockId{};
    if (model && !ctx.host->setting("tmp.layout.editing"))
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
    const std::string sel =
        ctx.host->setting(model ? "tmp.layout.editing" : "tmp.layout.viewport").value_or("");
    if (const auto colon = sel.find(':');
        colon != std::string::npos && sel.substr(0, colon) == lay->name)
        vp = lay->findViewport(std::atoi(sel.c_str() + colon + 1));
    if (!vp && !model && lay->viewports.size() == 1)
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

// The cell helpers below take the layer by value: a pick runs -LAYER, which replaces the row, and
// the rest of the combo list still reads the layer.
void drawLinetypeCell(tbcad::ui::PanelContext& ctx, const Layer l) {
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

void drawLineweightCell(tbcad::ui::PanelContext& ctx, const Layer l) {
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

void drawTransparencyCell(tbcad::ui::PanelContext& ctx, const Layer l) {
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

// The Layer States Manager: the saved states in a list, what a restore should apply as check
// boxes, and the actions (restore, update, rename, delete, new from the current layers).
void layerStatesManager(ui::PanelContext& ctx, const Document& doc, PanelState& st) {
    ImGui::SetNextWindowSize(ImVec2(440, 470), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal("Layer States Manager", &open, ImGuiWindowFlags_NoSavedSettings))
        return;
    const std::vector<std::string> names = layerStateNames(doc);
    if (!st.mgrSel.empty() && std::find(names.begin(), names.end(), st.mgrSel) == names.end())
        st.mgrSel.clear();
    ImGui::TextUnformatted("Saved layer states");
    if (ImGui::BeginChild("##statelist", ImVec2(0, 130), true)) {
        for (const std::string& n : names)
            if (ImGui::Selectable(n.c_str(), n == st.mgrSel))
                st.mgrSel = n;
        if (names.empty())
            ImGui::TextDisabled("None yet. Type a name below and press New.");
    }
    ImGui::EndChild();

    const bool picked = !st.mgrSel.empty();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##mgrname", "Name", &st.mgrName);
    ImGui::SameLine();
    ImGui::BeginDisabled(st.mgrName.empty());
    if (ImGui::Button("New")) {
        exec(ctx, {"-LAYERSTATE", "Save", st.mgrName});
        st.mgrSel = st.mgrName;
        st.mgrName.clear();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!picked);
    if (ImGui::Button("Rename") && picked) {
        exec(ctx, {"-LAYERSTATE", "Rename", st.mgrSel, st.mgrName});
        st.mgrSel = st.mgrName;
        st.mgrName.clear();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!picked);
    if (ImGui::Button("Update from current layers"))
        exec(ctx, {"-LAYERSTATE", "Save", st.mgrSel});
    ImGui::SameLine();
    if (ImGui::Button("Delete") && picked) {
        exec(ctx, {"-LAYERSTATE", "Delete", st.mgrSel});
        st.mgrSel.clear();
    }
    ImGui::EndDisabled();

    // Layer state files: Import reads TBCAD's own and AutoCAD's .las files and names the state
    // after the file; Export writes the picked state.
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##lasfile", "Layer state file (.las)", &st.lasPath);
    ImGui::SameLine();
    ImGui::BeginDisabled(st.lasPath.empty());
    if (ImGui::Button("Import")) {
        const std::filesystem::path p(std::u8string(st.lasPath.begin(), st.lasPath.end()));
        const std::u8string stem = p.stem().u8string();
        std::string name(stem.begin(), stem.end());
        exec(ctx,
             {"-LAYERSTATE", "Import", name.empty() ? std::string("Imported") : name, st.lasPath});
        st.mgrSel = lowered(name);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!picked);
    if (ImGui::Button("Export") && picked)
        exec(ctx, {"-LAYERSTATE", "Export", st.mgrSel, st.lasPath});
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Restore these properties");
    ImGui::Checkbox("On / frozen", &st.mgrVisibility);
    ImGui::SameLine();
    ImGui::Checkbox("Locked", &st.mgrLock);
    ImGui::SameLine();
    ImGui::Checkbox("Plot", &st.mgrPlot);
    ImGui::Checkbox("Colour, linetype, lineweight", &st.mgrLook);
    ImGui::Checkbox("Current layer", &st.mgrCurrent);
    ImGui::Checkbox("Turn off layers not in the state", &st.restoreHide);
    ImGui::BeginDisabled(!picked);
    if (ImGui::Button("Restore") && picked) {
        std::string parts;
        for (auto [on, word] :
             {std::pair{st.mgrVisibility, "Visibility"}, std::pair{st.mgrLock, "Lock"},
              std::pair{st.mgrPlot, "Plot"}, std::pair{st.mgrLook, "Look"},
              std::pair{st.mgrCurrent, "Current"}, std::pair{st.restoreHide, "HideOthers"}})
            if (on)
                parts += (parts.empty() ? "" : " ") + std::string(word);
        if (parts.empty())
            ImGui::CloseCurrentPopup();
        else
            exec(ctx, {"-LAYERSTATE", "Restore", st.mgrSel, "Custom", parts});
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close") || !open)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// The words of the -LAYERFILTER language for the property filter editor's choices.
std::string propertyFilterExpression(const PanelState::PropFilter& p) {
    std::string e;
    auto add = [&](const std::string& w) { e += (e.empty() ? "" : " ") + w; };
    auto trimmed = [](std::string t) {
        while (!t.empty() && t.back() == ' ')
            t.pop_back();
        while (!t.empty() && t.front() == ' ')
            t.erase(0, 1);
        return t;
    };
    if (!trimmed(p.pattern).empty())
        add("name:" + trimmed(p.pattern));
    const std::pair<int, std::pair<const char*, const char*>> switches[] = {
        {p.on, {"on", "off"}},
        {p.frozen, {"frozen", "thawed"}},
        {p.locked, {"locked", "unlocked"}},
        {p.plot, {"plot", "noplot"}},
        {p.used, {"used", "empty"}}};
    for (const auto& [v, words] : switches)
        if (v == 1 || v == 2)
            add(v == 1 ? words.first : words.second);
    if (!trimmed(p.colour).empty())
        add("color:" + trimmed(p.colour));
    if (!trimmed(p.linetype).empty())
        add("linetype:" + trimmed(p.linetype));
    if (!trimmed(p.lineweight).empty())
        add("lineweight:" + trimmed(p.lineweight));
    return e;
}

// AutoCAD's Layer Filter Properties: name a filter, set what it matches, see which layers
// match, and save it among the drawing's filters (-LAYERFILTER Save).
void propertyFilterEditor(ui::PanelContext& ctx, const Document& doc, PanelState& st) {
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal(
            "Layer Filter Properties", &open,
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
        return;
    PanelState::PropFilter& p = st.prop;
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("Filter name", "e.g. Walls-frozen", &p.name);
    ImGui::Separator();
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("Layer name", "* and ? allowed, e.g. A-WALL*", &p.pattern);
    auto tri = [](const char* label, int& v, const char* yes, const char* no) {
        const char* items[] = {"Any", yes, no};
        ImGui::SetNextItemWidth(200);
        ImGui::Combo(label, &v, items, 3);
    };
    tri("On", p.on, "On", "Off");
    tri("Freeze", p.frozen, "Frozen", "Thawed");
    tri("Lock", p.locked, "Locked", "Unlocked");
    tri("Plot", p.plot, "Plot", "No plot");
    tri("In use", p.used, "Used", "Unused");
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("Colour", "red, 3 or 255,128,0", &p.colour);
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("Linetype", "e.g. Dashed", &p.linetype);
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("Lineweight", "e.g. 0.35", &p.lineweight);

    const std::string expr = propertyFilterExpression(p);
    std::string error;
    std::vector<std::string> hits;
    if (!expr.empty())
        for (LayerId id : layerTreeOrder(doc)) {
            std::string e;
            if (layerMatchesFilter(doc, id, expr, &e))
                hits.push_back(layerPath(doc, id));
            if (!e.empty()) {
                error = e;
                break;
            }
        }
    ImGui::Separator();
    if (expr.empty())
        ImGui::TextDisabled("Choose at least one property.");
    else if (!error.empty())
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "%s", error.c_str());
    else {
        ImGui::TextDisabled("%zu layer(s) match: %s", hits.size(), expr.c_str());
        if (ImGui::BeginChild("##hits", ImVec2(0, 90), true)) {
            for (const std::string& h : hits)
                ImGui::TextUnformatted(h.c_str());
        }
        ImGui::EndChild();
    }
    const bool canSave = !expr.empty() && error.empty() && !p.name.empty();
    ImGui::BeginDisabled(!canSave);
    if (ImGui::Button("Save and apply") && canSave) {
        exec(ctx, {"-LAYERFILTER Save " + p.name, expr});
        st.filter = "filter:" + p.name;
        st.invert = false;
        p = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || !open)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// The Layer Translator: every layer with a box for the name it should get. Layers given a name
// that exists already are merged into it. Apply runs LAYTRANS Map as one undo step.
void layerTranslator(ui::PanelContext& ctx, const Document& doc, PanelState& st) {
    ImGui::SetNextWindowSize(ImVec2(480, 460), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal("Layer Translator", &open, ImGuiWindowFlags_NoSavedSettings))
        return;
    ImGui::TextWrapped(
        "Type a new name beside a layer. A name that exists already merges the "
        "layer into it.");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##trfilter", "Filter", &st.trFilter);
    const std::string f = lowered(st.trFilter);
    std::string pairs;
    std::size_t changes = 0;
    if (ImGui::BeginChild("##trlist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), true)) {
        if (ImGui::BeginTable("##trtable", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Layer");
            ImGui::TableSetupColumn("Translate to");
            ImGui::TableHeadersRow();
            for (LayerId id : layerTreeOrder(doc)) {
                const Layer* l = doc.layer(id);
                if (!l || id == kLayerZero)
                    continue;
                const std::string path = layerPath(doc, id);
                std::string& to = st.trNew[id.value];
                const bool changed = !to.empty() && to != path;
                if (changed) {
                    pairs += (pairs.empty() ? "" : ";") + path + ">" + to;
                    ++changes;
                }
                if (!f.empty() && lowered(path).find(f) == std::string::npos)
                    continue;
                ImGui::PushID(static_cast<int>(id.value));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(path.c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##to", path.c_str(), &to);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::BeginDisabled(changes == 0);
    if (ImGui::Button("Translate")) {
        // No trailing Enter (exec adds one): on an idle command line it would repeat LAYTRANS.
        if (ctx.runCommand) {
            if (ctx.cancelCommand)
                ctx.cancelCommand();
            ctx.runCommand("LAYTRANS");
            ctx.runCommand("Map");
            ctx.runCommand(pairs);
        }
        st.trNew.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu to change", changes);
    ImGui::SameLine();
    if (ImGui::Button("Load CSV...")) {
        ImGui::CloseCurrentPopup();
        if (ctx.runCommand) {  // leaves the file name prompt open for the user
            if (ctx.cancelCommand)
                ctx.cancelCommand();
            ctx.runCommand("LAYTRANS");
            ctx.runCommand("Load");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Export CSV...")) {
        ImGui::CloseCurrentPopup();
        if (ctx.runCommand) {
            if (ctx.cancelCommand)
                ctx.cancelCommand();
            ctx.runCommand("LAYTRANS");
            ctx.runCommand("Export");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close") || !open)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

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
        // Room left on the row of the previous item (the cursor is already on the next row).
        const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (right - ImGui::GetItemRectMax().x > w + ImGui::GetStyle().ItemSpacing.x)
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
    nextButton("New property filter...");
    if (ImGui::Button("New property filter..."))
        ImGui::OpenPopup("Layer Filter Properties");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A saved filter by name, switches, colour, linetype and lineweight");
    nextButton("New group...");
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
    propertyFilterEditor(ctx, doc, st);
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

    ImGui::SameLine();
    if (ImGui::Button("Manager...")) {
        st.mgrSel.clear();
        ImGui::OpenPopup("Layer States Manager");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Save, restore, rename and delete layer states");
    layerStatesManager(ctx, doc, st);
    ImGui::SameLine();
    if (ImGui::Button("Translate...")) {
        st.trNew.clear();
        ImGui::OpenPopup("Layer Translator");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rename or merge layers to an office standard");
    layerTranslator(ctx, doc, st);

    std::vector<Row> rows = buildRows(doc, st);
    st.order.clear();
    for (const Row& r : rows)
        st.order.push_back(r.id.value);

    // Widths, order, shown columns and the sort are remembered with the panel layout (imgui.ini).
    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable;
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
        ImGui::TableSetupColumn(
            "Name", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort,
            std::max(150.0f, availWidth - others));
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Frz", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn(
            "VP Frz", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 44);
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
        // Clicking a header sorts by it, again reverses (by name A-Z to start with).
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty) {
            const int col = specs->SpecsCount > 0 ? specs->Specs[0].ColumnIndex : -1;
            const bool desc = specs->SpecsCount > 0 &&
                              specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            specs->SpecsDirty = false;
            if (col != st.sortColumn || desc != st.sortDescending) {
                st.sortColumn = col;
                st.sortDescending = desc;
                rows = buildRows(doc, st);
                st.order.clear();
                for (const Row& r : rows)
                    st.order.push_back(r.id.value);
            }
        }
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
                // The name cell's menu can merge or delete this very layer.
                l = doc.layer(row.id);
                if (!l) {
                    ImGui::PopID();
                    continue;
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
                // A cell that runs a command can change the layer table and move the layer, so it
                // is looked up again after each one (a stale pointer read freed memory).
                const auto again = [&] { return (l = doc.layer(row.id)) != nullptr; };
                do {
                    flag(1, "##on", l->on, "On", "Off",
                         l->on ? "Visible; click to turn off" : "Off; click to turn on");
                    if (!again())
                        break;
                    flag(2, "##frz", l->frozen, "Freeze", "Thaw",
                         "Frozen layers are hidden and skipped when regenerating");
                    if (!again())
                        break;
                    if (vp) {
                        ImGui::TableSetColumnIndex(3);
                        const bool fz = std::find(vp.frozen.begin(), vp.frozen.end(), row.id) !=
                                        vp.frozen.end();
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
                    if (!again())
                        break;
                    flag(4, "##lck", l->locked, "Lock", "Unlock",
                         "Locked layers show but cannot be edited");
                    if (!again())
                        break;
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
                    if (!again())
                        break;
                    ImGui::TableSetColumnIndex(6);
                    drawLinetypeCell(ctx, *l);
                    if (!again())
                        break;
                    ImGui::TableSetColumnIndex(7);
                    drawLineweightCell(ctx, *l);
                    if (!again())
                        break;
                    flag(8, "##plot", l->plot, "Plot", "NoPlot", "Plot this layer");
                    if (!again())
                        break;
                    ImGui::TableSetColumnIndex(9);
                    drawTransparencyCell(ctx, *l);
                    if (!again())
                        break;
                    ImGui::TableSetColumnIndex(10);
                    drawDescriptionCell(ctx, *l);
                } while (false);
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
