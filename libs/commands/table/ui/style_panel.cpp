// The Table Style dialog (TABLESTYLE): the styles, Set current / New / Delete, a preview, and
// the sizes and cell styles (title, header, data) of the selected one. Every change runs
// -TABLESTYLE Modify with the whole style, so it is one undo step and shows on the command line.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "tbcad/core/color.h"
#include "tbcad/core/document.h"
#include "tbcad/table/table.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::tablecmd {

namespace {

struct TableStylePanel {
    std::string selected = "Standard";
    std::string newName{};
};

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

void send(ui::PanelContext& pc, const std::vector<std::string>& lines) {
    if (!pc.runCommand)
        return;
    if (pc.cancelCommand)
        pc.cancelCommand();
    for (const std::string& l : lines)
        pc.runCommand(l);
}

const char* alignWord(int a) {
    return a == 0 ? "Left" : a == 1 ? "Center" : a == 2 ? "Right" : "Default";
}

// The answers of -TABLESTYLE New/Modify that give `name` exactly the values of `s`.
std::vector<std::string> defineLines(const char* verb, const std::string& name,
                                     const table::TableStyle& s) {
    std::vector<std::string> l = {"-TABLESTYLE",
                                  verb,
                                  name,
                                  num(s.colWidth),
                                  num(s.rowHeight),
                                  num(s.textHeight),
                                  num(s.margin),
                                  s.textStyle.empty() ? "-" : s.textStyle};
    const std::pair<const char*, const TableCellStyle*> roles[] = {
        {"Title", &s.title}, {"Header", &s.header}, {"Data", &s.data}};
    for (const auto& [role, cs] : roles) {
        l.push_back(role);
        l.push_back(num(cs->textHeight));
        l.push_back(std::to_string(cs->color));
        l.push_back(std::to_string(cs->fill));
        l.push_back(alignWord(cs->align));
    }
    l.push_back("HeaderRow");
    l.push_back(s.headerRow ? "Yes" : "No");
    l.push_back("Done");
    return l;
}

ImU32 imColor(int index, ImU32 fallback) {
    if (index < 1 || index > 255)
        return fallback;
    const Rgba c = paletteColor(static_cast<std::uint8_t>(index));
    return IM_COL32(c.r, c.g, c.b, 255);
}

// A small drawing of a table in the style: title row, header row when on, three data rows.
void drawPreview(const table::TableStyle& s) {
    const ImVec2 size(220, 110);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(255, 255, 255, 255));
    const ImU32 line = IM_COL32(40, 40, 40, 255);
    const int dataRows = 3;
    const int rows = 1 + (s.headerRow ? 1 : 0) + dataRows;
    const float rh = (size.y - 10) / static_cast<float>(rows);
    const float x0 = p.x + 10, x1 = p.x + size.x - 10;
    const int cols = 3;
    const float cw = (x1 - x0) / cols;
    float y = p.y + 5;
    auto rowCells = [&](const TableCellStyle& cs, bool merged, const char* text, int defAlign) {
        const ImU32 fill = imColor(cs.fill, 0);
        if (fill)
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rh), fill);
        const ImU32 ink = imColor(cs.color, line);
        const int align = cs.align < 0 ? defAlign : cs.align;
        for (int c = 0; c < (merged ? 1 : cols); ++c) {
            const float cx0 = merged ? x0 : x0 + c * cw;
            const float cx1 = merged ? x1 : cx0 + cw;
            const float tw = ImGui::CalcTextSize(text).x * 0.8f;
            const float tx = align == 0   ? cx0 + 3
                             : align == 2 ? cx1 - 3 - tw
                                          : (cx0 + cx1 - tw) / 2;
            dl->AddText(nullptr, ImGui::GetFontSize() * 0.8f,
                        ImVec2(tx, y + (rh - ImGui::GetFontSize() * 0.8f) / 2), ink, text);
            dl->AddRect(ImVec2(cx0, y), ImVec2(cx1, y + rh), line);
        }
        y += rh;
    };
    rowCells(s.title, true, "Title", 1);
    if (s.headerRow)
        rowCells(s.header, false, "Header", 1);
    for (int r = 0; r < dataRows; ++r)
        rowCells(s.data, false, "Data", 0);
    ImGui::Dummy(size);
}

// One role's cell style; true when it changed.
bool cellStyleFields(const char* id, TableCellStyle& cs) {
    bool changed = false;
    ImGui::PushID(id);
    double h = cs.textHeight;
    ImGui::SetNextItemWidth(110);
    ImGui::InputDouble("Text height (0 = table's)", &h, 0, 0, "%.6g");
    if (ImGui::IsItemDeactivatedAfterEdit() && h >= 0 && h != cs.textHeight) {
        cs.textHeight = h;
        changed = true;
    }
    int col = cs.color;
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("Text colour (0 = cell's)", &col, 0);
    if (ImGui::IsItemDeactivatedAfterEdit() && col >= 0 && col <= 255 && col != cs.color) {
        cs.color = col;
        changed = true;
    }
    int fill = cs.fill;
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("Fill colour (0 = none)", &fill, 0);
    if (ImGui::IsItemDeactivatedAfterEdit() && fill >= 0 && fill <= 255 && fill != cs.fill) {
        cs.fill = fill;
        changed = true;
    }
    ImGui::SetNextItemWidth(110);
    if (ImGui::BeginCombo("Alignment", alignWord(cs.align))) {
        for (int a = -1; a <= 2; ++a)
            if (ImGui::Selectable(alignWord(a), a == cs.align) && a != cs.align) {
                cs.align = a;
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

void drawTableStyles(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand)
        return;
    static TableStylePanel st;
    const Document& doc = *pc.doc;
    const std::vector<table::TableStyle> styles = table::tableStyles(doc.header());
    const std::string current = table::currentTableStyleName(doc.header());
    auto found = std::find_if(styles.begin(), styles.end(),
                              [&](const table::TableStyle& s) { return s.name == st.selected; });
    if (found == styles.end()) {
        st.selected = current;
        found = std::find_if(styles.begin(), styles.end(),
                             [&](const table::TableStyle& s) { return s.name == current; });
    }

    ImGui::BeginChild("ts-list", ImVec2(160, 0), ImGuiChildFlags_Borders);
    for (const table::TableStyle& s : styles) {
        const std::string label = s.name + (s.name == current ? "  (current)" : "");
        if (ImGui::Selectable(label.c_str(), s.name == st.selected))
            st.selected = s.name;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    const table::TableStyle sel =
        found != styles.end() ? *found : table::TableStyle{.name = "Standard"};
    ImGui::BeginDisabled(pc.commandActive);
    ImGui::BeginDisabled(sel.name == current);
    if (ImGui::Button("Set current"))
        send(pc, {"-TABLESTYLE", "Set", sel.name});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(sel.name == "Standard");
    if (ImGui::Button("Delete"))
        send(pc, {"-TABLESTYLE", "Delete", sel.name});
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Apply to tables..."))
        send(pc, {"-TABLESTYLE", "Apply", sel.name});
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##tsnew", "Name of the new style", &st.newName);
    ImGui::SameLine();
    ImGui::BeginDisabled(st.newName.empty());
    if (ImGui::Button("New, starting with this one")) {
        send(pc, defineLines("New", st.newName, sel));
        st.selected = st.newName;
        st.newName.clear();
    }
    ImGui::EndDisabled();

    drawPreview(sel);

    table::TableStyle edit = sel;
    bool changed = false;
    ImGui::SeparatorText("General");
    auto size = [&](const char* label, double& v, bool allowZero) {
        double x = v;
        ImGui::SetNextItemWidth(110);
        ImGui::InputDouble(label, &x, 0, 0, "%.6g");
        if (ImGui::IsItemDeactivatedAfterEdit() && (allowZero ? x >= 0 : x > 0) && x != v) {
            v = x;
            changed = true;
        }
    };
    size("Column width", edit.colWidth, false);
    size("Row height", edit.rowHeight, false);
    size("Text height", edit.textHeight, false);
    size("Cell margin", edit.margin, true);
    ImGui::SetNextItemWidth(110);
    if (ImGui::BeginCombo("Text style",
                          edit.textStyle.empty() ? "(current)" : edit.textStyle.c_str())) {
        if (ImGui::Selectable("(current)", edit.textStyle.empty()) && !edit.textStyle.empty()) {
            edit.textStyle.clear();
            changed = true;
        }
        for (const auto& [id, t] : doc.textStyles())
            if (ImGui::Selectable(t.name.c_str(), t.name == edit.textStyle) &&
                t.name != edit.textStyle) {
                edit.textStyle = t.name;
                changed = true;
            }
        ImGui::EndCombo();
    }
    if (ImGui::Checkbox("Header row", &edit.headerRow))
        changed = true;
    ImGui::SeparatorText("Cell styles");
    if (ImGui::BeginTabBar("ts-cells")) {
        const std::pair<const char*, TableCellStyle*> roles[] = {
            {"Title", &edit.title}, {"Header", &edit.header}, {"Data", &edit.data}};
        for (const auto& [role, cs] : roles)
            if (ImGui::BeginTabItem(role)) {
                changed |= cellStyleFields(role, *cs);
                ImGui::EndTabItem();
            }
        ImGui::EndTabBar();
    }
    if (changed && found != styles.end())
        send(pc, defineLines("Modify", sel.name, edit));
    ImGui::EndDisabled();
    ImGui::EndGroup();
}

}  // namespace

void registerTableStylePanel(ui::UiRegistry& ui) {
    ui.addPanel("tablestyles", "Table Style", drawTableStyles, false);
}

}  // namespace tbcad::tablecmd
