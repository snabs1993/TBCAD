// Dimension style panel, read-only property sections and menu entries (Dear ImGui). Built only in
// application builds.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

void enumCombo(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
               int current, const std::vector<std::pair<const char*, const char*>>& options) {
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
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputDouble(label, &v, 0, 0, "%.6g", kEnter) && v != value)
        pc.runCommand(editLine(s.name, key, number(v)));
}

void textField(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
               const std::string& value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    ImGui::SetNextItemWidth(120);
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
    ImGui::SetNextItemWidth(120);
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
    ImGui::SetNextItemWidth(120);
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

    ImGui::BeginChild("dimstyle-list", ImVec2(170, 0), ImGuiChildFlags_Borders);
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
    }
    ImGui::SetNextItemWidth(160);
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

void drawDimensionSection(ui::PanelContext& pc, EntityId id) {
    if (!pc.doc)
        return;
    const Entity* e = pc.doc->find(id);
    const DimensionData* d = e ? entityAs<DimensionData>(*e) : nullptr;
    if (!d)
        return;
    const DimStyle& s = dim::styleOf(*pc.doc, d->style);
    ImGui::SeparatorText("Dimension");
    ImGui::Text("Style: %s", s.name.c_str());
    ImGui::Text("Text: %s", dim::dimensionText(s, *d).c_str());
    if (!d->textOverride.empty())
        ImGui::TextDisabled("Text is overridden");
}

void drawLeaderSection(ui::PanelContext& pc, EntityId id) {
    if (!pc.doc)
        return;
    const Entity* e = pc.doc->find(id);
    const LeaderData* l = e ? entityAs<LeaderData>(*e) : nullptr;
    if (!l)
        return;
    ImGui::SeparatorText("Leader");
    ImGui::Text("Style: %s", dim::styleOf(*pc.doc, l->style).name.c_str());
    ImGui::Text("%zu vertices", l->vertices.size());
}

}  // namespace

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("Dimension/Linear", "DIMLINEAR", "dim.linear");
    ui.addMenuItem("Dimension/Aligned", "DIMALIGNED", "dim.aligned");
    ui.addMenuItem("Dimension/Radius", "DIMRADIUS", "dim.radius");
    ui.addMenuItem("Dimension/Diameter", "DIMDIAMETER", "dim.diameter");
    ui.addMenuItem("Dimension/Angular", "DIMANGULAR", "dim.angular");
    ui.addMenuItem("Dimension/Leader", "LEADER", "dim.leader");
    ui.addPanel("dimstyles", "Dimension Styles", drawPanel, false);
    ui.addPropertySection(EntityKind::Dimension, drawDimensionSection);
    ui.addPropertySection(EntityKind::Leader, drawLeaderSection);
}

}  // namespace tbcad::dimcmd
