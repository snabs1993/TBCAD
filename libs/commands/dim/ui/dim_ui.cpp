// Dimension style panel, read-only property sections and menu entries (Dear ImGui). Built only in
// application builds.
#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "../src/dim_internal.h"
#include "tbcad/dim/dim.h"
#include "tbcad/text/text.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::dimcmd {

namespace {

constexpr ImGuiInputTextFlags kEnter =
    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
// InputDouble / InputInt do not take EnterReturnsTrue: the edit counts when the field is left.
constexpr ImGuiInputTextFlags kNumber = ImGuiInputTextFlags_AutoSelectAll;

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
            // A stacked tolerance is markup: the preview shows it on one line.
            const std::string shown = L.text.stacked ? text::plainText(L.text.utf8) : L.text.utf8;
            ImVec2 sz = ImGui::CalcTextSize(shown.c_str());
            ImVec2 p = map(L.text.center);
            dl->AddText(ImVec2(p.x - sz.x / 2, p.y - sz.y / 2), col, shown.c_str());
        }
    }
    ImGui::Dummy(size);
}

// Label on the left, box filling the rest of the row (like the Properties panel). Returns the
// hidden widget id to give the input.
std::string leftLabel(const char* label) {
    const float labelW = std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, 90.0f, 150.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label, std::strstr(label, "##"));  // without a "##id" part
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(-1);
    return std::string("##") + label;
}

// An input box that leaves room for a button of the given label on its row.
float boxBeside(const char* buttonLabel) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float button = ImGui::CalcTextSize(buttonLabel).x + st.FramePadding.x * 2;
    return std::max(60.0f, ImGui::GetContentRegionAvail().x - button - st.ItemSpacing.x);
}

void enumCombo(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
               int current, const std::vector<std::pair<const char*, const char*>>& options,
               const char* tip = nullptr) {
    if (ImGui::BeginCombo(leftLabel(label).c_str(),
                          options[static_cast<std::size_t>(current)].first)) {
        for (std::size_t i = 0; i < options.size(); ++i)
            if (ImGui::Selectable(options[i].first, static_cast<int>(i) == current))
                pc.runCommand(editLine(s.name, key, options[i].second));
        ImGui::EndCombo();
    }
    if (tip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
}

void realField(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
               double value, const char* tip = nullptr) {
    double v = value;
    ImGui::InputDouble(leftLabel(label).c_str(), &v, 0, 0, "%.6g", kNumber);
    if (tip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    if (ImGui::IsItemDeactivatedAfterEdit() && v != value)
        pc.runCommand(editLine(s.name, key, number(v)));
}

void textField(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
               const std::string& value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    if (ImGui::InputText(leftLabel(label).c_str(), buf, sizeof buf, kEnter)) {
        std::string v = buf;
        std::replace(v.begin(), v.end(), ' ', '_');
        pc.runCommand(editLine(s.name, key, v.empty() ? "none" : v));
    }
}

void colorField(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
                const Color& value) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s", colorText(value).c_str());
    if (ImGui::InputText(leftLabel(label).c_str(), buf, sizeof buf, kEnter))
        pc.runCommand(editLine(s.name, key, buf));
}

void flagField(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
               bool value, const char* on, const char* off) {
    bool v = value;
    if (ImGui::Checkbox(label, &v))
        pc.runCommand(editLine(s.name, key, v ? on : off));
}

void precisionField(ui::PanelContext& pc, const DimStyle& s, const char* label, const char* key,
                    int value) {
    int v = value;
    ImGui::InputInt(leftLabel(label).c_str(), &v, 1, 1, kNumber);
    if (ImGui::IsItemDeactivatedAfterEdit() && v != value)
        pc.runCommand(editLine(s.name, key, std::to_string(std::clamp(v, 0, 8))));
}

void textStyleCombo(ui::PanelContext& pc, const DimStyle& s) {
    const TextStyle* ts = s.textStyle.value ? pc.doc->textStyle(s.textStyle) : nullptr;
    if (ImGui::BeginCombo(leftLabel("Text style").c_str(), ts ? ts->name.c_str() : "(current)")) {
        if (ImGui::Selectable("(current)", ts == nullptr))
            pc.runCommand(editLine(s.name, "TextStyle", "Current"));
        for (const auto& [id, t] : pc.doc->textStyles())
            if (!hasSpace(t.name) && ImGui::Selectable(t.name.c_str(), ts && ts->id == id))
                pc.runCommand(editLine(s.name, "TextStyle", t.name));
        ImGui::EndCombo();
    }
}

// The properties in AutoCAD's tabs: Lines, Symbols and Arrows, Text, Fit, Primary Units,
// Alternate Units, Tolerances.
void drawEditor(ui::PanelContext& pc, const DimStyle s) {  // by value: fields run DIMSTYLE
    drawPreview(pc, s);
    if (hasSpace(s.name)) {
        ImGui::TextWrapped("Names with spaces cannot be edited here; use the DIMSTYLE command.");
        return;
    }
    if (!ImGui::BeginTabBar("##dimstyletabs"))
        return;
    if (ImGui::BeginTabItem("Lines")) {
        ImGui::SeparatorText("Dimension lines");
        colorField(pc, s, "Colour", "LineColor", s.dimLineColor);
        realField(pc, s, "Extend beyond ticks", "LineExtension", s.dimLineExtension);
        double base = s.baselineSpacing;
        ImGui::InputDouble(leftLabel("Baseline spacing").c_str(), &base, 0, 0, "%.6g", kNumber);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("DIMDLI: 0 = twice the text height");
        if (ImGui::IsItemDeactivatedAfterEdit() && base != s.baselineSpacing)
            pc.runCommand(editLine(s.name, "BaselineSpacing", base > 0 ? number(base) : "Auto"));
        flagField(pc, s, "Suppress dim line 1", "DimLine1", s.suppressDim1, "Hide", "Show");
        ImGui::SameLine();
        flagField(pc, s, "Suppress dim line 2", "DimLine2", s.suppressDim2, "Hide", "Show");
        ImGui::SeparatorText("Extension lines");
        colorField(pc, s, "Colour##ext", "ExtColor", s.extLineColor);
        realField(pc, s, "Extend beyond dim lines", "ExtExtension", s.extLineExtension);
        realField(pc, s, "Offset from origin", "ExtOffset", s.extLineOffset);
        flagField(pc, s, "Suppress ext line 1", "ExtLine1", s.suppressExt1, "Hide", "Show");
        ImGui::SameLine();
        flagField(pc, s, "Suppress ext line 2", "ExtLine2", s.suppressExt2, "Hide", "Show");
        flagField(pc, s, "Fixed length extension lines", "FixedExt", s.fixedExtLines, "On", "Off");
        ImGui::BeginDisabled(!s.fixedExtLines);
        realField(pc, s, "Length", "FixedExtLength", s.fixedExtLength);
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Symbols and Arrows")) {
        enumCombo(pc, s, "Arrowheads", "ArrowType", static_cast<int>(s.arrowType),
                  {{"Closed filled", "Filled"},
                   {"Open", "Open"},
                   {"Architectural tick", "Tick"},
                   {"Dot", "Dot"},
                   {"None", "None"}});
        realField(pc, s, "Arrow size", "ArrowSize", s.arrowSize);
        realField(pc, s, "Center mark", "CenterMark", s.centerMark,
                  "DIMCEN: size of the centre mark of radius and diameter dimensions; negative "
                  "draws centre lines, 0 none.");
        ImGui::SeparatorText("Arc length symbol");
        enumCombo(pc, s, "Arc symbol", "ArcSymbol", std::clamp(s.arcSymbol, 0, 2),
                  {{"Preceding dimension text", "Before"},
                   {"Above dimension text", "Above"},
                   {"None", "None"}},
                  "DIMARCSYM: where an arc length dimension shows the arc symbol.");
        ImGui::SeparatorText("Radius jog dimension");
        realField(pc, s, "Jog angle", "JogAngle", s.jogAngle * 180 / kPi,
                  "DIMJOGANG: the angle of the jog's middle segment against the dimension line, "
                  "in degrees (5 to 90).");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Text")) {
        ImGui::SeparatorText("Text appearance");
        textStyleCombo(pc, s);
        colorField(pc, s, "Text colour", "TextColor", s.textColor);
        enumCombo(pc, s, "Fill", "TextFill", s.textFill,
                  {{"None", "None"}, {"Background", "Background"}, {"Colour", "Color"}});
        ImGui::BeginDisabled(s.textFill != 2);
        int fillColor = s.textFillColor;
        ImGui::InputInt(leftLabel("Fill colour").c_str(), &fillColor, 1, 1, kNumber);
        if (ImGui::IsItemDeactivatedAfterEdit() && fillColor != s.textFillColor)
            pc.runCommand(
                editLine(s.name, "TextFillColor", std::to_string(std::clamp(fillColor, 0, 256))));
        ImGui::EndDisabled();
        realField(pc, s, "Text height", "TextHeight", s.textHeight);
        realField(pc, s, "Fraction height scale", "TolHeight", s.tolHeightFactor);
        flagField(pc, s, "Draw frame around text", "TextFrame", s.textFrame, "Yes", "No");
        ImGui::SeparatorText("Text placement");
        enumCombo(pc, s, "Vertical", "Placement", static_cast<int>(s.textPlacement),
                  {{"Above", "Above"}, {"Centered", "Centered"}, {"Below", "Below"}});
        realField(
            pc, s, "Vertical position", "TextVertPos", s.textVertPos,
            "DIMTVP: with Centered text, moves it this many text heights up (negative down).");
        enumCombo(pc, s, "Horizontal", "Justify", s.textJustify,
                  {{"Centered", "Centered"},
                   {"At extension line 1", "Ext1"},
                   {"At extension line 2", "Ext2"}});
        realField(pc, s, "Offset from dim line", "TextGap", s.textGap);
        ImGui::SeparatorText("Text alignment");
        enumCombo(pc, s, "Inside", "Alignment", static_cast<int>(s.textAlign),
                  {{"Aligned with dimension line", "WithLine"}, {"Horizontal", "Horizontal"}});
        enumCombo(pc, s, "Outside", "AlignmentOutside", static_cast<int>(s.textAlignOutside),
                  {{"Aligned with dimension line", "WithLine"}, {"Horizontal", "Horizontal"}});
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Fit")) {
        enumCombo(pc, s, "If not enough room", "Fit", static_cast<int>(s.fit),
                  {{"Either text or arrows (best fit)", "Best"},
                   {"Arrows", "ArrowsFirst"},
                   {"Text", "TextFirst"},
                   {"Both text and arrows", "BothOutside"}},
                  "DIMATFIT: what moves outside the extension lines when the text and arrows do "
                  "not both fit.");
        flagField(pc, s, "Always keep text between ext lines", "TextInside", s.textInsideForced,
                  "Forced", "Auto");
        flagField(pc, s, "Suppress arrows if they don't fit inside", "ArrowsOutside",
                  s.suppressArrowsOutside, "Suppress", "Show");
        enumCombo(pc, s, "Text not in default place", "TextMove", static_cast<int>(s.textMove),
                  {{"Beside the dimension line", "WithLine"},
                   {"Over the line, with leader", "Leader"},
                   {"Over the line, without leader", "Free"}});
        ImGui::SeparatorText("Scale for dimension features");
        realField(pc, s, "Use overall scale of", "Scale", s.overallScale);
        ImGui::SeparatorText("Fine tuning");
        flagField(pc, s, "Draw dim line between ext lines", "LineInside", s.forceLineInside,
                  "Force", "Auto");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Primary Units")) {
        ImGui::SeparatorText("Linear dimensions");
        enumCombo(pc, s, "Unit format", "LengthFormat", static_cast<int>(s.linearFormat),
                  {{"Decimal", "Decimal"},
                   {"Architectural", "Architectural"},
                   {"Fractional", "Fractional"}});
        enumCombo(pc, s, "Units", "Units", static_cast<int>(s.units),
                  {{"mm", "mm"}, {"cm", "cm"}, {"m", "m"}, {"in", "in"}, {"ft", "ft"}});
        precisionField(pc, s, "Precision", "Precision", s.precision);
        enumCombo(pc, s, "Decimal separator", "DecimalMark", s.commaDecimal ? 1 : 0,
                  {{"'.' (Period)", "Point"}, {"',' (Comma)", "Comma"}});
        realField(pc, s, "Round off", "RoundOff", s.roundOff);
        textField(pc, s, "Prefix", "Prefix", s.prefix);
        textField(pc, s, "Suffix", "Suffix", s.suffix);
        realField(pc, s, "Scale factor", "LinearFactor", s.linearFactor,
                  "DIMLFAC: multiplies every length measurement before it is shown.");
        flagField(pc, s, "Suppress leading zeros", "LeadingZeros", s.suppressLeadingZeros,
                  "Suppress", "Show");
        ImGui::SameLine();
        flagField(pc, s, "Suppress trailing zeros", "TrailingZeros", s.suppressTrailingZeros,
                  "Suppress", "Show");
        ImGui::SeparatorText("Angular dimensions");
        enumCombo(pc, s, "Units format", "AngleFormat", static_cast<int>(s.angleFormat),
                  {{"Decimal degrees", "Degrees"},
                   {"Degrees Minutes Seconds", "Dms"},
                   {"Gradians", "Gradians"},
                   {"Radians", "Radians"}});
        int ap = s.angularPrecision;
        ImGui::InputInt(leftLabel("Angle precision").c_str(), &ap, 1, 1, kNumber);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("-1 = the same as the linear precision");
        if (ImGui::IsItemDeactivatedAfterEdit() && ap != s.angularPrecision)
            pc.runCommand(editLine(s.name, "AngularPrecision",
                                   ap < 0 ? "Same" : std::to_string(std::min(ap, 8))));
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Alternate Units")) {
        flagField(pc, s, "Display alternate units", "AltUnits", s.altUnits, "On", "Off");
        ImGui::BeginDisabled(!s.altUnits);
        enumCombo(pc, s, "Units", "AltUnit", static_cast<int>(s.altUnit),
                  {{"mm", "mm"}, {"cm", "cm"}, {"m", "m"}, {"in", "in"}, {"ft", "ft"}});
        realField(pc, s, "Multiplier", "AltScale", s.altScale);
        precisionField(pc, s, "Precision", "AltPrecision", s.altPrecision);
        textField(pc, s, "Prefix", "AltPrefix", s.altPrefix);
        textField(pc, s, "Suffix", "AltSuffix", s.altSuffix);
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Tolerances")) {
        enumCombo(pc, s, "Method", "Tolerance", static_cast<int>(s.tolerance),
                  {{"None", "None"},
                   {"Symmetrical", "Symmetrical"},
                   {"Deviation", "Deviation"},
                   {"Limits", "Limits"}});
        ImGui::BeginDisabled(s.tolerance == DimTolerance::None);
        precisionField(pc, s, "Precision", "TolPrecision", s.tolPrecision);
        realField(pc, s, "Upper value", "TolUpper", s.tolUpper);
        realField(pc, s, "Lower value", "TolLower", s.tolLower);
        realField(pc, s, "Scaling for height", "TolHeight", s.tolHeightFactor);
        ImGui::EndDisabled();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// What a child or override style is for, from the part after '$'.
std::string childLabel(const std::string& name) {
    const auto at = name.rfind('$');
    const std::string code = name.substr(at + 1);
    if (code == "0")
        return "Linear";
    if (code == "2")
        return "Angular";
    if (code == "3")
        return "Diameter";
    if (code == "4")
        return "Radius";
    if (code == "6")
        return "Ordinate";
    if (code == "7")
        return "Leaders";
    return "Override " + code;
}

void drawCompare(ui::PanelContext& pc, const DimStyle& a) {
    static DimStyleId other{};
    const DimStyle* b = pc.doc->dimStyle(other);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##comparewith", b ? b->name.c_str() : "Compare with...")) {
        for (const auto& [id, st] : pc.doc->dimStyles())
            if (id != a.id && ImGui::Selectable(st.name.c_str(), id == other))
                other = id;
        ImGui::EndCombo();
    }
    if (!b || b->id == a.id)
        return;
    const auto da = describeStyle(*pc.doc, a), db = describeStyle(*pc.doc, *b);
    int differences = 0;
    if (ImGui::BeginTable(
            "##compare", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
            ImVec2(0, 160))) {
        ImGui::TableSetupColumn("Property");
        ImGui::TableSetupColumn(a.name.c_str());
        ImGui::TableSetupColumn(b->name.c_str());
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < da.size() && i < db.size(); ++i) {
            if (da[i].second == db[i].second)
                continue;
            ++differences;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(da[i].first.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(da[i].second.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(db[i].second.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Text(differences == 0 ? "The styles are the same." : "%d difference(s).", differences);
}

void drawPanel(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand)
        return;
    PanelState& st = state();
    const DimStyleId current = pc.doc->header().currentDimStyle;
    if (!pc.doc->dimStyle(st.selected))
        st.selected = current;

    // Parents with their child and override styles under them.
    ImGui::BeginChild("dimstyle-list",
                      ImVec2(std::clamp(ImGui::GetContentRegionAvail().x * 0.3f, 90.0f, 190.0f), 0),
                      ImGuiChildFlags_Borders);
    for (const auto& [id, s] : pc.doc->dimStyles()) {
        if (s.name.find('$') != std::string::npos &&
            pc.doc->dimStyleByName(s.name.substr(0, s.name.rfind('$'))))
            continue;  // listed under its parent
        const std::string label = s.name + (id == current ? "  (current)" : "");
        if (ImGui::Selectable(label.c_str(), id == st.selected))
            st.selected = id;
        for (const auto& [cid, c] : pc.doc->dimStyles()) {
            if (c.name.size() <= s.name.size() + 1 || c.name.rfind(s.name + "$", 0) != 0 ||
                c.name.find('$', s.name.size() + 1) != std::string::npos)
                continue;
            const std::string child = "    " + childLabel(c.name) + "##" + c.name;
            if (ImGui::Selectable(child.c_str(), cid == st.selected))
                st.selected = cid;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginGroup();
    // A copy: Delete, Rename and the editor's fields run DIMSTYLE, which replaces or removes the
    // row while this frame still draws it.
    std::optional<DimStyle> selRow;
    if (const DimStyle* found = pc.doc->dimStyle(st.selected))
        selRow = *found;
    const DimStyle* sel = selRow ? &*selRow : nullptr;
    const bool isChild = sel && sel->name.find('$') != std::string::npos;
    if (sel) {
        ImGui::BeginDisabled(st.selected == current || hasSpace(sel->name) || isChild);
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
        ImGui::InputTextWithHint("##renamedimstyle", "New name", st.renameTo, sizeof st.renameTo);
        ImGui::SameLine();
        ImGui::BeginDisabled(st.selected == kDimStyleStandard || hasSpace(sel->name) || isChild ||
                             hasSpace(st.renameTo) || st.renameTo[0] == '\0' ||
                             pc.doc->dimStyleByName(st.renameTo) != nullptr);
        if (ImGui::Button("Rename")) {
            pc.runCommand("DIMSTYLE Rename " + sel->name + " " + st.renameTo + " Done");
            st.renameTo[0] = '\0';
        }
        ImGui::EndDisabled();
    }
    // New: a style of its own (copied from the current one), or a child of the selected style
    // used only for one kind of dimension.
    static int useFor = 0;
    static const std::pair<const char*, const char*> kUseFor[] = {
        {"All dimensions", ""}, {"Linear", "0"},   {"Angular", "2"}, {"Radius", "4"},
        {"Diameter", "3"},      {"Ordinate", "6"}, {"Leaders", "7"}};
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("Use for", kUseFor[useFor].first)) {
        for (int i = 0; i < 7; ++i)
            if (ImGui::Selectable(kUseFor[i].first, i == useFor))
                useFor = i;
        ImGui::EndCombo();
    }
    if (useFor == 0) {
        ImGui::SetNextItemWidth(boxBeside("New from current"));
        ImGui::InputTextWithHint("##newdimstyle", "Name for new style", st.newName,
                                 sizeof st.newName);
        ImGui::SameLine();
        ImGui::BeginDisabled(hasSpace(st.newName) || pc.doc->dimStyleByName(st.newName) != nullptr);
        if (ImGui::Button("New from current")) {
            pc.runCommand(std::string("DIMSTYLE New ") + st.newName + " Done");
            st.newName[0] = '\0';
            st.selected = {};
        }
        ImGui::EndDisabled();
    } else {
        const std::string childName =
            sel && !isChild ? sel->name + "$" + kUseFor[useFor].second : std::string();
        ImGui::BeginDisabled(childName.empty() || hasSpace(childName) ||
                             pc.doc->dimStyleByName(childName) != nullptr);
        const std::string label = std::string("New ") + kUseFor[useFor].first + " child of " +
                                  (sel && !isChild ? sel->name : std::string("...")) + "##newchild";
        if (ImGui::Button(label.c_str())) {
            pc.runCommand("DIMSTYLE New " + childName + " Done");
            if (const DimStyle* made = pc.doc->dimStyleByName(childName))
                st.selected = made->id;
        }
        ImGui::EndDisabled();
    }
    if (sel) {
        if (ImGui::CollapsingHeader("Compare"))
            drawCompare(pc, *sel);
        drawEditor(pc, *sel);
    }
    ImGui::EndGroup();
}

bool isDimOrLeader(const Document& doc, EntityId id) {
    const Entity* e = doc.find(id);
    return e && (kind(*e) == EntityKind::Dimension || kind(*e) == EntityKind::Leader ||
                 kind(*e) == EntityKind::MLeader);
}

DimStyleId styleIdOf(const Entity& e, const Document& doc) {
    DimStyleId id{};
    if (const auto* d = entityAs<DimensionData>(e))
        id = d->style;
    else if (const auto* l = entityAs<LeaderData>(e))
        id = l->style;
    else if (const auto* m = entityAs<MLeaderData>(e))
        id = m->style;
    return id.value ? id : doc.header().currentDimStyle;
}

// Style and overall scale of the selected dimensions and leaders; with several selected the field
// shows *VARIES* when they differ, and an edit applies to all of them.
void drawDimensionSection(ui::PanelContext& pc, EntityId id) {
    if (!pc.doc)
        return;
    std::vector<EntityId> single;
    const std::vector<EntityId>* all = &single;
    if (pc.multiSelection && pc.selection) {
        all = &ui::selectedWhere(pc, "dimension", isDimOrLeader);
        if (all->empty() || all->front() != id)
            return;  // one section for the whole selection
    } else if (isDimOrLeader(*pc.doc, id)) {
        single.push_back(id);
    }
    const std::vector<EntityId>& ids = *all;
    if (ids.empty())
        return;
    const Entity* first = pc.doc->find(ids.front());
    const bool leader = kind(*first) == EntityKind::Leader || kind(*first) == EntityKind::MLeader;
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
    if (ImGui::BeginCombo(leftLabel("Style").c_str(), sameStyle ? s0.name.c_str() : "*VARIES*")) {
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
    if (ImGui::InputText(
            leftLabel("Overall scale").c_str(), buf, sizeof buf,
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
        if (const auto* m = entityAs<MLeaderData>(*first))
            ImGui::Text("%zu leader lines", m->lines.size());
        else
            ImGui::Text("%zu vertices", entityAs<LeaderData>(*first)->vertices.size());
    }
}

// ---- Contextual tab: Dimension ----------------------------------------------------------------

void drawContextTab(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand || !pc.selection || pc.selection->empty())
        return;
    const Entity* first = pc.doc->find(pc.selection->front());
    if (!first)
        return;
    const DimStyleId style0 = styleIdOf(*first, *pc.doc);
    bool sameStyle = true;
    for (EntityId i : *pc.selection)
        if (const Entity* e = pc.doc->find(i))
            sameStyle = sameStyle && styleIdOf(*e, *pc.doc) == style0;
    ImGui::SetNextItemWidth(140);
    if (ImGui::BeginCombo("##ctxdimstyle",
                          sameStyle ? dim::styleOf(*pc.doc, style0).name.c_str() : "*VARIES*")) {
        for (const auto& [sid, st] : pc.doc->dimStyles())
            if (!hasSpace(st.name) &&
                ImGui::Selectable(st.name.c_str(), sameStyle && sid == style0))
                pc.runCommand("DIMSETUP Style " + st.name);
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Dimension style of the selected dimensions");
    const std::pair<const char*, const char*> tools[] = {
        {"Text position", "DIMTEDIT"}, {"Oblique / text", "DIMEDIT"},
        {"Break", "DIMBREAK"},         {"Space", "DIMSPACE"},
        {"Jog", "DIMJOGLINE"},         {"Inspect", "DIMINSPECT"},
        {"Override", "DIMOVERRIDE"},   {"Update", "DIMUPDATE"},
        {"Reassociate", "DIMREASSOC"}};
    for (const auto& [label, command] : tools) {
        ImGui::SameLine();
        if (ImGui::SmallButton(label))
            pc.runCommand(command);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", command);
    }
}

}  // namespace

void registerMLeaderStylePanel(ui::UiRegistry& ui);  // mleader_ui.cpp

void registerUi(ui::UiRegistry& ui) {
    registerMLeaderStylePanel(ui);
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
    ui.addPropertySection(EntityKind::MLeader, drawDimensionSection);
    ui.addContextualTab("dimension", "Dimension", {EntityKind::Dimension}, drawContextTab);
}

}  // namespace tbcad::dimcmd
