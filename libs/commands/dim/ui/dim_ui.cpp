// Dimension style panel, read-only property sections and menu entries (Dear ImGui). Built only in
// application builds.
#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/dim_internal.h"
#include "tbcad/dim/dim.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::dimcmd {

namespace {

constexpr ImGuiInputTextFlags kEnter = ImGuiInputTextFlags_EnterReturnsTrue;

struct PanelState {
    DimStyleId selected{};
    char newName[64] = "";
    char renameTo[64] = "";
};

PanelState& state() {
    static PanelState s;
    return s;
}

std::string number(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

bool hasSpace(const std::string& s) {
    return s.empty() || s.find_first_of(" \t") != std::string::npos;
}

std::string colorText(const Color& c) {
    switch (c.kind) {
        case ColorKind::ByBlock:
            return "ByBlock";
        case ColorKind::ByLayer:
            return "ByLayer";
        case ColorKind::Index:
            return std::to_string(c.indexValue());
        case ColorKind::Rgb:
            return std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b);
        default:
            return "ByBlock";
    }
}

// One command line that changes one property of one style.
std::string editLine(const std::string& style, const char* key, const std::string& value) {
    return "DIMSTYLE Edit " + style + " " + key + " " + value + " Done Done";
}

// A small sample dimension drawn with the style, so changes can be seen as they are made.
void drawPreview(ui::PanelContext& pc, const DimStyle& style) {
    DimensionData d;
    d.kind = DimKind::Linear;
    d.defPts = {Vec2{0, 0}, Vec2{40, 0}, Vec2{20, 8}, Vec2{}};
    d.style = style.id;
    dim::Layout L = dim::layoutDimension(*pc.doc, d);

    BBox2 box;
    for (const auto& l : L.lines)
        for (const Vec2& p : l.pts)
            box.expand(p);
    for (const auto& a : L.arrows)
        for (const Vec2& p : a.ring)
            box.expand(p);
    if (L.text.valid()) {
        box.expand(L.text.center - Vec2{L.text.width / 2, L.text.height});
        box.expand(L.text.center + Vec2{L.text.width / 2, L.text.height});
    }
    ImVec2 size(300, 110);
    ImVec2 min = ImGui::GetCursorScreenPos();
    ImVec2 max(min.x + size.x, min.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg));
    dl->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
    if (!box.empty() && box.width() > 0 && box.height() >= 0) {
        const float pad = 14;
        double k = std::min((size.x - 2 * pad) / box.width(),
                            (size.y - 2 * pad) / std::max(box.height(), 1e-3));
        Vec2 c = (box.min + box.max) / 2;
        auto map = [&](Vec2 p) {
            return ImVec2(static_cast<float>(min.x + size.x / 2 + (p.x - c.x) * k),
                          static_cast<float>(min.y + size.y / 2 - (p.y - c.y) * k));
        };
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        for (const auto& l : L.lines)
            for (std::size_t i = 1; i < l.pts.size(); ++i)
                dl->AddLine(map(l.pts[i - 1]), map(l.pts[i]), col, 1.0f);
        for (const auto& a : L.arrows) {
            std::vector<ImVec2> pts;
            for (const Vec2& p : a.ring)
                pts.push_back(map(p));
            if (pts.size() >= 3) {
                // ImDrawList wants clockwise points for a convex fill; the order of ours depends
                // on the arrow's direction, so fill both ways round.
                dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), col);
                std::reverse(pts.begin(), pts.end());
                dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), col);
            }
        }
        if (L.text.valid()) {
            ImVec2 sz = ImGui::CalcTextSize(L.text.utf8.c_str());
            ImVec2 p = map(L.text.center);
            dl->AddText(ImVec2(p.x - sz.x / 2, p.y - sz.y / 2), col, L.text.utf8.c_str());
        }
    }
    ImGui::Dummy(size);
}

// Input boxes take part of the panel's width, so the labels beside them stay readable when the
// panel is docked narrow (a fixed 120 px box pushed the label out of view).
float fieldWidth() {
    return std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, 70.0f, 160.0f);
}

// An input box that leaves room for a button of the given label on its row.
float boxBeside(const char* buttonLabel) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float button = ImGui::CalcTextSize(buttonLabel).x + st.FramePadding.x * 2;
    return std::max(60.0f, ImGui::GetContentRegionAvail().x - button - st.ItemSpacing.x);
}

void enumCombo(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
               int current, const std::vector<std::pair<const char*, const char*>>& options) {
    ImGui::SetNextItemWidth(fieldWidth());
    if (ImGui::BeginCombo(label, options[static_cast<std::size_t>(current)].first)) {
        for (std::size_t i = 0; i < options.size(); ++i)
            if (ImGui::Selectable(options[i].first, static_cast<int>(i) == current))
                pc.runCommand(editLine(s.name, key, options[i].second));
        ImGui::EndCombo();
    }
}

void realField(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
               double value) {
    double v = value;
    ImGui::SetNextItemWidth(fieldWidth());
    if (ImGui::InputDouble(label, &v, 0, 0, "%.6g", kEnter) && v != value)
        pc.runCommand(editLine(s.name, key, number(v)));
}

void textField(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
               const std::string& value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    ImGui::SetNextItemWidth(fieldWidth());
    if (ImGui::InputText(label, buf, sizeof buf, kEnter)) {
        std::string v = buf;
        std::replace(v.begin(), v.end(), ' ', '_');
        pc.runCommand(editLine(s.name, key, v.empty() ? "none" : v));
    }
}

void colorField(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
                const Color& value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s", colorText(value).c_str());
    ImGui::SetNextItemWidth(fieldWidth());
    if (ImGui::InputText(label, buf, sizeof buf, kEnter))
        pc.runCommand(editLine(s.name, key, buf));
}

void drawEditor(ui::PanelContext& pc, const DimStyle& s) {
    drawPreview(pc, s);
    if (hasSpace(s.name)) {
        ImGui::TextWrapped("Names with spaces cannot be edited here; use the DIMSTYLE command.");
        return;
    }
    ImGui::SeparatorText("Text");
    realField(pc, s, "Text height", "TextHeight", s.textHeight);
    realField(pc, s, "Gap to line", "TextGap", s.textGap);
    enumCombo(pc, s, "Placement", "Placement", static_cast<int>(s.textPlacement),
              {{"Above the line", "Above"}, {"Centered on the line", "Centered"}});
    enumCombo(pc, s, "Alignment", "Alignment", static_cast<int>(s.textAlign),
              {{"With the line", "WithLine"}, {"Horizontal", "Horizontal"}});
    colorField(pc, s, "Text colour", "TextColor", s.textColor);

    ImGui::SeparatorText("Lines and arrows");
    enumCombo(pc, s, "Arrow", "ArrowType", static_cast<int>(s.arrowType),
              {{"Closed filled", "Filled"},
               {"Open", "Open"},
               {"Tick", "Tick"},
               {"Dot", "Dot"},
               {"None", "None"}});
    realField(pc, s, "Arrow size", "ArrowSize", s.arrowSize);
    realField(pc, s, "Extension offset", "ExtOffset", s.extLineOffset);
    realField(pc, s, "Extension beyond", "ExtExtension", s.extLineExtension);
    realField(pc, s, "Line beyond", "LineExtension", s.dimLineExtension);
    colorField(pc, s, "Line colour", "LineColor", s.dimLineColor);
    colorField(pc, s, "Extension colour", "ExtColor", s.extLineColor);

    ImGui::SeparatorText("Numbers");
    enumCombo(pc, s, "Units", "Units", static_cast<int>(s.units),
              {{"mm", "mm"}, {"cm", "cm"}, {"m", "m"}, {"in", "in"}, {"ft", "ft"}});
    int prec = s.precision;
    ImGui::SetNextItemWidth(fieldWidth());
    if (ImGui::InputInt("Decimals", &prec, 1, 1, kEnter) && prec != s.precision)
        pc.runCommand(editLine(s.name, "Precision", std::to_string(std::clamp(prec, 0, 8))));
    realField(pc, s, "Round off", "RoundOff", s.roundOff);
    textField(pc, s, "Prefix", "Prefix", s.prefix);
    textField(pc, s, "Suffix", "Suffix", s.suffix);
    bool lead = s.suppressLeadingZeros, trail = s.suppressTrailingZeros;
    if (ImGui::Checkbox("Suppress leading zeros", &lead))
        pc.runCommand(editLine(s.name, "LeadingZeros", lead ? "Suppress" : "Show"));
    if (ImGui::Checkbox("Suppress trailing zeros", &trail))
        pc.runCommand(editLine(s.name, "TrailingZeros", trail ? "Suppress" : "Show"));

    ImGui::SeparatorText("Scale");
    realField(pc, s, "Overall scale", "Scale", s.overallScale);
}

void drawPanel(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand)
        return;
    PanelState& st = state();
    const DimStyleId current = pc.doc->header().currentDimStyle;
    if (!pc.doc->dimStyle(st.selected))
        st.selected = current;

    ImGui::BeginChild("dimstyle-list",
                      ImVec2(std::clamp(ImGui::GetContentRegionAvail().x * 0.3f, 90.0f, 170.0f), 0),
                      ImGuiChildFlags_Borders);
    for (const auto& [id, s] : pc.doc->dimStyles()) {
        std::string label = s.name + (id == current ? "  (current)" : "");
        if (ImGui::Selectable(label.c_str(), id == st.selected))
            st.selected = id;
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginGroup();
    const DimStyle* sel = pc.doc->dimStyle(st.selected);
    if (sel) {
        ImGui::BeginDisabled(st.selected == current || hasSpace(sel->name));
        if (ImGui::Button("Set current"))
            pc.runCommand("DIMSTYLE Current " + sel->name + " Done");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(st.selected == current || st.selected == kDimStyleStandard ||
                             hasSpace(sel->name));
        if (ImGui::Button("Delete"))
            pc.runCommand("DIMSTYLE Delete " + sel->name + " Done");
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(boxBeside("Rename"));
        ImGui::InputText("##renamedimstyle", st.renameTo, sizeof st.renameTo);
        ImGui::SameLine();
        ImGui::BeginDisabled(st.selected == kDimStyleStandard || hasSpace(sel->name) ||
                             hasSpace(st.renameTo) || st.renameTo[0] == '\0' ||
                             pc.doc->dimStyleByName(st.renameTo) != nullptr);
        if (ImGui::Button("Rename")) {
            pc.runCommand("DIMSTYLE Rename " + sel->name + " " + st.renameTo + " Done");
            st.renameTo[0] = '\0';
        }
        ImGui::EndDisabled();
    }
    ImGui::SetNextItemWidth(boxBeside("New from current"));
    ImGui::InputText("##newdimstyle", st.newName, sizeof st.newName);
    ImGui::SameLine();
    ImGui::BeginDisabled(hasSpace(st.newName) || pc.doc->dimStyleByName(st.newName) != nullptr);
    if (ImGui::Button("New from current")) {
        pc.runCommand(std::string("DIMSTYLE New ") + st.newName + " Done");
        st.newName[0] = '\0';
        st.selected = {};
    }
    ImGui::EndDisabled();
    if (sel)
        drawEditor(pc, *sel);
    ImGui::EndGroup();
}

bool isDimOrLeader(const Document& doc, EntityId id) {
    const Entity* e = doc.find(id);
    return e && (kind(*e) == EntityKind::Dimension || kind(*e) == EntityKind::Leader);
}

DimStyleId styleIdOf(const Entity& e, const Document& doc) {
    DimStyleId id{};
    if (const auto* d = entityAs<DimensionData>(e))
        id = d->style;
    else if (const auto* l = entityAs<LeaderData>(e))
        id = l->style;
    return id.value ? id : doc.header().currentDimStyle;
}

// Style and overall scale of the selected dimensions and leaders; with several selected the field
// shows *VARIES* when they differ, and an edit applies to all of them.
void drawDimensionSection(ui::PanelContext& pc, EntityId id) {
    if (!pc.doc)
        return;
    std::vector<EntityId> ids;
    if (pc.multiSelection && pc.selection) {
        for (EntityId s : *pc.selection)
            if (isDimOrLeader(*pc.doc, s))
                ids.push_back(s);
        if (ids.empty() || ids.front() != id)
            return;  // one section for the whole selection
    } else if (isDimOrLeader(*pc.doc, id)) {
        ids.push_back(id);
    }
    if (ids.empty())
        return;
    const Entity* first = pc.doc->find(ids.front());
    const bool leader = kind(*first) == EntityKind::Leader;
    ImGui::SeparatorText(ids.size() > 1 ? "Dimensions and leaders"
                         : leader       ? "Leader"
                                        : "Dimension");
    if (ids.size() > 1)
        ImGui::TextDisabled("%zu objects", ids.size());

    bool sameStyle = true, sameScale = true;
    const DimStyleId style0 = styleIdOf(*first, *pc.doc);
    const double scale0 = dim::styleOf(*pc.doc, style0).overallScale;
    for (EntityId i : ids) {
        const DimStyleId si = styleIdOf(*pc.doc->find(i), *pc.doc);
        sameStyle = sameStyle && si == style0;
        sameScale = sameScale && dim::styleOf(*pc.doc, si).overallScale == scale0;
    }
    const DimStyle& s0 = dim::styleOf(*pc.doc, style0);

    ImGui::BeginDisabled(pc.commandActive);
    ImGui::SetNextItemWidth(fieldWidth());
    if (ImGui::BeginCombo("Style", sameStyle ? s0.name.c_str() : "*VARIES*")) {
        for (const auto& [sid, st] : pc.doc->dimStyles())
            if (!hasSpace(st.name) &&
                ImGui::Selectable(st.name.c_str(), sameStyle && sid == style0))
                pc.runCommand("DIMSETUP Style " + st.name);
        ImGui::EndCombo();
    }
    char buf[40];
    if (sameScale)
        std::snprintf(buf, sizeof buf, "%g", scale0);
    else
        std::snprintf(buf, sizeof buf, "*VARIES*");
    ImGui::SetNextItemWidth(fieldWidth());
    if (ImGui::InputText(
            "Overall scale", buf, sizeof buf,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
        double v = 0;
        auto [ptr, ec] = std::from_chars(buf, buf + std::strlen(buf), v);
        if (ec == std::errc() && ptr == buf + std::strlen(buf) && v > 0)
            pc.runCommand("DIMSETUP Scale " + std::string(buf));
    }
    ImGui::EndDisabled();
    if (ids.size() == 1 && !leader) {
        const DimensionData* d = entityAs<DimensionData>(*first);
        ImGui::Text("Text: %s", dim::dimensionText(s0, *d).c_str());
        if (!d->textOverride.empty())
            ImGui::TextDisabled("Text is overridden");
    } else if (ids.size() == 1) {
        ImGui::Text("%zu vertices", entityAs<LeaderData>(*first)->vertices.size());
    }
}

}  // namespace

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Dimension/Linear", "DIMLINEAR", "dim.linear");
    ui.addMenuItem("Dimension/Aligned", "DIMALIGNED", "dim.aligned");
    ui.addMenuItem("Dimension/Radius", "DIMRADIUS", "dim.radius");
    ui.addMenuItem("Dimension/Diameter", "DIMDIAMETER", "dim.diameter");
    ui.addMenuItem("Dimension/Angular", "DIMANGULAR", "dim.angular");
    ui.addMenuItem("Dimension/Leader", "LEADER", "dim.leader");
    ui.addMenuItem("Format/Dimension Style Manager", "DIMSTYLES", "format.dimstyles");
    ui.addPanel("dimstyles", "Dimension Styles", drawPanel, false);
    ui.addPropertySection(EntityKind::Dimension, drawDimensionSection);
    ui.addPropertySection(EntityKind::Leader, drawDimensionSection);
}

}  // namespace tbcad::dimcmd
