// The selection dialogs: Quick Select (QSELECT), Object Selection Filters (FILTER) and Select
// Similar Settings. They only change what is selected, so they work on the selection directly;
// saving or deleting a named filter changes the drawing and goes through _FILTERSAVE (one undo
// step).
#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "commands.h"
#include "tbcad/select/select.h"

#ifdef TBCAD_BLOCK_PANEL_IMGUI
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#endif

namespace tbcad::block_cmds {

#ifdef TBCAD_BLOCK_PANEL_IMGUI

namespace {

using select::Compare;
using select::Property;
using select::Query;

constexpr const char* kPropLabels[] = {
    "Type",    "Layer",  "Color",  "Linetype", "Lineweight", "Level", "Transparency",
    "Pattern", "Height", "Radius", "Length",   "Name",       "Style", "Contents"};
constexpr int kPropCount = static_cast<int>(sizeof kPropLabels / sizeof kPropLabels[0]);

bool numeric(Property p) {
    return p == Property::Transparency || p == Property::Height || p == Property::Radius ||
           p == Property::Length;
}
bool textual(Property p) {
    return !numeric(p) && p != Property::Color && p != Property::Lineweight;
}

// Operators shown for a property; "Select All" (index 5) takes every object of the type.
enum Op { OpEquals, OpNot, OpGreater, OpLess, OpWildcard, OpAll };
constexpr const char* kOpLabels[] = {"= Equals",    "<> Not Equal",     "> Greater than",
                                     "< Less than", "* Wildcard Match", "Select All"};

bool opAllowed(Property p, int op) {
    if (op == OpGreater || op == OpLess)
        return numeric(p);
    if (op == OpWildcard)
        return textual(p);
    return true;
}

Query makeQuery(Property p, int op, const std::string& value) {
    Query q;
    q.property = p;
    q.value = value;
    q.negate = op == OpNot;
    q.compare = op == OpGreater    ? Compare::Greater
                : op == OpLess     ? Compare::Less
                : op == OpWildcard ? Compare::Wildcard
                                   : Compare::Equals;
    return q;
}

std::vector<EntityId> everything(const Document& doc) {
    Query all;
    all.property = Property::Type;
    all.value = "*";
    all.compare = Compare::Wildcard;
    return select::queryEntities(doc, all).value_or(std::vector<EntityId>{});
}

// Names to pick from for the value of a property (empty = type it).
std::vector<std::string> valueChoices(const Document& doc, Property p) {
    std::vector<std::string> out;
    switch (p) {
        case Property::Layer:
            for (const auto& [id, l] : doc.layers())
                out.push_back(l.name);
            break;
        case Property::Linetype:
            out = {"ByLayer", "ByBlock"};
            for (const auto& [id, l] : doc.linetypes())
                out.push_back(l.name);
            break;
        case Property::Color:
            out = {"ByLayer", "ByBlock", "1", "2", "3", "4", "5", "6", "7"};
            break;
        case Property::Lineweight:
            out = {"ByLayer", "Default", "0.00", "0.13", "0.18", "0.25", "0.35", "0.50", "0.70"};
            break;
        case Property::Name:
            for (const auto& [id, b] : doc.blocks())
                if (!b.anonymous)
                    out.push_back(b.name);
            break;
        case Property::Style:
            for (const auto& [id, s] : doc.textStyles())
                out.push_back(s.name);
            for (const auto& [id, s] : doc.dimStyles())
                out.push_back(s.name);
            break;
        default:
            break;
    }
    std::sort(out.begin() + (p == Property::Linetype ? 2 : 0), out.end());
    return out;
}

// A value field: a combo of the known names when there are some, always free to type.
void valueField(const char* label, const Document& doc, Property p, std::string& value) {
    ImGui::SetNextItemWidth(220);
    ImGui::InputText(label, &value);
    // The names are only listed while the drop-down is open: a drawing with thousands of layers
    // would otherwise build the list every frame.
    bool any = p == Property::Layer || p == Property::Linetype || p == Property::Color ||
               p == Property::Lineweight || p == Property::Style;
    if (p == Property::Name)
        for (const auto& [bid, b] : doc.blocks())
            if (!b.anonymous) {
                any = true;
                break;
            }
    if (!any)
        return;
    ImGui::SameLine();
    const std::string id = std::string("##pick") + label;
    if (ImGui::BeginCombo(id.c_str(), "", ImGuiComboFlags_NoPreview)) {
        for (const auto& c : valueChoices(doc, p))
            if (ImGui::Selectable(c.c_str(), c == value))
                value = c;
        ImGui::EndCombo();
    }
}

bool opCombo(const char* label, Property p, int& op, bool allowAll) {
    if (!opAllowed(p, op) || (!allowAll && op == OpAll))
        op = OpEquals;
    bool changed = false;
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo(label, kOpLabels[op])) {
        for (int i = 0; i <= (allowAll ? OpAll : OpWildcard); ++i)
            if (opAllowed(p, i) && ImGui::Selectable(kOpLabels[i], i == op)) {
                op = i;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

bool propCombo(const char* label, int& prop, bool withType) {
    bool changed = false;
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo(label, kPropLabels[prop])) {
        for (int i = withType ? 0 : 1; i < kPropCount; ++i)
            if (ImGui::Selectable(kPropLabels[i], i == prop)) {
                prop = i;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

// ---- Quick Select ----

struct QuickState {
    bool withinSelection = false;
    std::string type = "Multiple";
    int prop = 1;  // Layer
    int op = OpEquals;
    std::string value{};
    bool exclude = false;
    bool append = false;
    std::string result{};
    // Cache of the object types offered, and what it was worked out for.
    std::set<std::string> types{};
    std::optional<std::tuple<std::uint64_t, std::uint64_t, bool, std::size_t, EntityId, EntityId>>
        typesKey{};
};

void drawQuickSelect(ui::PanelContext& pc) {
    if (!pc.doc || !pc.host)
        return;
    static QuickState st;
    const Document& doc = *pc.doc;
    const std::vector<EntityId>& now = pc.host->selection();

    ImGui::TextUnformatted("Apply to:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Entire drawing", !st.withinSelection))
        st.withinSelection = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(now.empty());
    if (ImGui::RadioButton("Current selection", st.withinSelection))
        st.withinSelection = true;
    ImGui::EndDisabled();
    if (now.empty())
        st.withinSelection = false;

    // The object types found where the search runs: worked out again only when the drawing, the
    // selection or the scope changed, as a walk over a big drawing takes far longer than a frame.
    const std::tuple key{doc.uid(),
                         doc.revision(),
                         st.withinSelection,
                         now.size(),
                         now.empty() ? EntityId{} : now.front(),
                         now.empty() ? EntityId{} : now.back()};
    if (!st.typesKey || *st.typesKey != key) {
        st.types.clear();
        const std::vector<EntityId> pool = st.withinSelection ? now : everything(doc);
        for (EntityId id : pool)
            if (const Entity* e = doc.find(id))
                st.types.insert(select::objectTypeName(*e));
        st.typesKey = key;
    }
    const std::set<std::string>& types = st.types;
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Object type", st.type.c_str())) {
        if (ImGui::Selectable("Multiple", st.type == "Multiple"))
            st.type = "Multiple";
        for (const auto& t : types)
            if (ImGui::Selectable(t.c_str(), st.type == t))
                st.type = t;
        ImGui::EndCombo();
    }
    propCombo("Properties", st.prop, false);
    const auto prop = static_cast<Property>(st.prop);
    opCombo("Operator", prop, st.op, true);
    ImGui::BeginDisabled(st.op == OpAll);
    valueField("Value", doc, prop, st.value);
    ImGui::EndDisabled();

    // One choice a row, so both fit a slim side panel.
    ImGui::TextUnformatted("How to apply:");
    if (ImGui::RadioButton("Include in new selection set", !st.exclude))
        st.exclude = false;
    if (ImGui::RadioButton("Exclude from new selection set", st.exclude))
        st.exclude = true;
    ImGui::Checkbox("Append to current selection set", &st.append);

    if (ImGui::Button("Select Matching")) {
        std::unordered_set<EntityId> ofType;
        bool typeOk = true;
        if (st.type != "Multiple") {
            Query tq;
            tq.property = Property::Type;
            tq.value = st.type;
            auto t = select::queryEntities(doc, tq);
            typeOk = t.has_value();
            if (t)
                ofType.insert(t->begin(), t->end());
        }
        std::unordered_set<EntityId> hit;
        bool valueOk = true;
        if (st.op != OpAll) {
            auto m = select::queryEntities(doc, makeQuery(prop, st.op, st.value));
            valueOk = m.has_value();
            if (m)
                hit.insert(m->begin(), m->end());
        }
        if (!typeOk || !valueOk) {
            st.result = "\"" + st.value + "\" is not a valid " + kPropLabels[st.prop] + " value.";
        } else {
            std::vector<EntityId> out = st.append ? now : std::vector<EntityId>{};
            std::unordered_set<EntityId> have(out.begin(), out.end());
            std::size_t found = 0;
            const std::vector<EntityId> pool = st.withinSelection ? now : everything(doc);
            for (EntityId id : pool) {
                if (st.type != "Multiple" && !ofType.count(id))
                    continue;
                const bool matches = st.op == OpAll || hit.count(id);
                if (matches == st.exclude)
                    continue;
                ++found;
                if (have.insert(id).second)
                    out.push_back(id);
            }
            if (pc.cancelCommand)
                pc.cancelCommand();
            pc.host->setSelection(std::move(out));
            st.result = std::to_string(found) + " object(s) found.";
        }
    }
    if (!st.result.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(st.result.c_str());
    }
}

// ---- Object Selection Filters ----

struct FilterState {
    std::vector<Query> conds{};
    int selected = -1;
    int prop = 0;
    int op = OpEquals;
    bool orGroup = false;
    std::string value{};
    std::string name{};
    std::string loaded{};
    bool within = false;
    std::string result{};
};

std::string describe(const Query& q) {
    return std::string(kPropLabels[static_cast<int>(q.property)]) +
           (q.negate                         ? " <> "
            : q.compare == Compare::Greater  ? " > "
            : q.compare == Compare::Less     ? " < "
            : q.compare == Compare::Wildcard ? " like "
                                             : " = ") +
           q.value;
}

void saveFilter(ui::PanelContext& pc, const std::string& name, const std::string& encoded) {
    if (!pc.runCommand)
        return;
    if (pc.cancelCommand)
        pc.cancelCommand();
    pc.host->setSetting("tmp.filter.save", name + "\n" + encoded);
    pc.runCommand("_FILTERSAVE");
}

void drawFilter(ui::PanelContext& pc) {
    if (!pc.doc || !pc.host)
        return;
    static FilterState st;
    const Document& doc = *pc.doc;

    ImGui::TextDisabled("Conditions (each OR starts a group; within a group all must hold)");
    if (ImGui::BeginListBox("##conds", ImVec2(-1, 120))) {
        for (int i = 0; i < static_cast<int>(st.conds.size()); ++i) {
            const Query& q = st.conds[static_cast<std::size_t>(i)];
            const std::string line = std::string(i == 0         ? ""
                                                 : q.orPrevious ? "OR  "
                                                                : "AND ") +
                                     describe(q) + "##" + std::to_string(i);
            if (ImGui::Selectable(line.c_str(), st.selected == i))
                st.selected = i;
        }
        ImGui::EndListBox();
    }
    ImGui::BeginDisabled(st.selected < 0 || st.selected >= static_cast<int>(st.conds.size()));
    if (ImGui::Button("Delete")) {
        st.conds.erase(st.conds.begin() + st.selected);
        st.selected = -1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear List")) {
        st.conds.clear();
        st.selected = -1;
    }

    ImGui::SeparatorText("Add a condition");
    ImGui::BeginDisabled(st.conds.empty());
    ImGui::Checkbox("OR (start a new group)", &st.orGroup);
    ImGui::EndDisabled();
    propCombo("Property##f", st.prop, true);
    const auto prop = static_cast<Property>(st.prop);
    opCombo("Operator##f", prop, st.op, false);
    valueField("Value##f", doc, prop, st.value);
    if (ImGui::Button("Add to List")) {
        Query q = makeQuery(prop, st.op, st.value);
        if (!select::queryEntities(doc, q)) {
            st.result = "\"" + st.value + "\" is not a valid " + kPropLabels[st.prop] + " value.";
        } else {
            q.orPrevious = st.orGroup && !st.conds.empty();
            st.conds.push_back(std::move(q));
            st.result.clear();
        }
    }

    ImGui::SeparatorText("Named filters");
    const auto names = select::savedFilters(doc);
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Current", st.loaded.empty() ? "*unnamed" : st.loaded.c_str())) {
        for (const auto& n : names)
            if (ImGui::Selectable(n.c_str(), n == st.loaded)) {
                auto it = doc.header().custom.find("filter." + n);
                if (it != doc.header().custom.end()) {
                    st.conds = select::decodeFilter(it->second);
                    st.loaded = n;
                    st.name = n;
                    st.selected = -1;
                }
            }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##fname", "Name", &st.name);
    ImGui::SameLine();
    ImGui::BeginDisabled(pc.commandActive || st.name.empty() || st.conds.empty());
    if (ImGui::Button("Save As")) {
        saveFilter(pc, st.name, select::encodeFilter(st.conds));
        st.loaded = st.name;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(pc.commandActive || st.loaded.empty());
    if (ImGui::Button("Delete Current Filter List")) {
        saveFilter(pc, st.loaded, "");
        st.loaded.clear();
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::BeginDisabled(pc.host->selection().empty());
    ImGui::Checkbox("Only within the current selection", &st.within);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(st.conds.empty());
    if (ImGui::Button("Apply Filter")) {
        auto ids = select::filterEntities(doc, st.conds);
        if (!ids) {
            st.result = "A condition has a value that cannot be read.";
        } else {
            if (st.within && !pc.host->selection().empty()) {
                const auto& now = pc.host->selection();
                const std::unordered_set<EntityId> in(now.begin(), now.end());
                std::erase_if(*ids, [&](EntityId id) { return !in.count(id); });
            }
            st.result = std::to_string(ids->size()) + " object(s) found.";
            if (pc.cancelCommand)
                pc.cancelCommand();
            pc.host->setSelection(std::move(*ids));
        }
    }
    ImGui::EndDisabled();
    if (!st.result.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(st.result.c_str());
    }
}

// ---- Select Similar Settings ----

void drawSimilarSettings(ui::PanelContext& pc) {
    if (!pc.host)
        return;
    using select::Match;
    static const std::pair<const char*, Match> kBits[] = {
        {"Object type", Match::Type},
        {"Layer", Match::Layer},
        {"Color", Match::Color},
        {"Linetype", Match::Linetype},
        {"Lineweight", Match::Lineweight},
        {"Level", Match::Level},
        {"Transparency", Match::Transparency},
        {"Name / style (text, dimension, hatch pattern, block)", Match::Style},
        {"Size (text height, radius)", Match::Size}};
    unsigned m = select::similarMatch(pc.host->setting(select::kSimilarMatchKey));
    ImGui::TextUnformatted("SELECTSIMILAR selects the objects that share:");
    for (const auto& [label, bit] : kBits) {
        bool on = (m & static_cast<unsigned>(bit)) != 0;
        if (ImGui::Checkbox(label, &on)) {
            m ^= static_cast<unsigned>(bit);
            pc.host->setSetting(select::kSimilarMatchKey, std::to_string(m));
        }
    }
}

}  // namespace

#endif

void registerSelectPanels(ui::UiRegistry& ui) {
#ifdef TBCAD_BLOCK_PANEL_IMGUI
    ui.addPanel("qselect", "Quick Select", drawQuickSelect, false);
    ui.addPanel("filter", "Object Selection Filters", drawFilter, false);
    ui.addPanel("selectsimilar", "Select Similar Settings", drawSimilarSettings, false);
#endif
    ui.addMenuItem("Tools/Quick Select...", "QSELECT", "tools.qselect");
    ui.addMenuItem("Tools/Object Selection Filters...", "FILTER", "tools.filter");
}

}  // namespace tbcad::block_cmds
