// The Point Style dialog (DDPTYPE): a grid of the twenty point marks and the size, applied by
// running PTYPE so that a change is one undo step and shows on the command line.
#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "tbcad/ui/ui_registry.h"

namespace tbcad::draw2 {

namespace {

struct PointStyleState {
    long long mode = -1;  // -1: not read from the drawing yet
    double size = 0;
    bool relative = true;
};

long long headerInt(const Document& doc, const char* key, long long fallback) {
    auto it = doc.header().custom.find(key);
    if (it == doc.header().custom.end())
        return fallback;
    return std::strtoll(it->second.c_str(), nullptr, 10);
}

double headerDouble(const Document& doc, const char* key) {
    auto it = doc.header().custom.find(key);
    return it == doc.header().custom.end() ? 0.0 : std::strtod(it->second.c_str(), nullptr);
}

// Draws the mark for the base style (0 dot, 1 none, 2 plus, 3 cross, 4 tick) plus the enclosing
// circle (32) and square (64).
void drawMark(ImDrawList* dl, ImVec2 c, float r, long long mode, ImU32 col) {
    const long long base = mode % 32;
    if (base == 0)
        dl->AddCircleFilled(c, 1.5f, col);
    else if (base == 2) {
        dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col);
        dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col);
    } else if (base == 3) {
        dl->AddLine({c.x - r, c.y - r}, {c.x + r, c.y + r}, col);
        dl->AddLine({c.x - r, c.y + r}, {c.x + r, c.y - r}, col);
    } else if (base == 4)
        dl->AddLine({c.x, c.y - r}, {c.x, c.y}, col);
    if (mode & 32)
        dl->AddCircle(c, r, col);
    if (mode & 64)
        dl->AddRect({c.x - r, c.y - r}, {c.x + r, c.y + r}, col);
}

void drawPointStyle(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand)
        return;
    static PointStyleState st;
    if (st.mode < 0 || (!pc.commandActive && ImGui::IsWindowAppearing())) {
        st.mode = headerInt(*pc.doc, "pdmode", 0);
        st.size = headerDouble(*pc.doc, "pdsize");
        st.relative = st.size == 0;
    }

    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    for (long long ring : {0, 32, 64, 96}) {
        for (long long b = 0; b <= 4; ++b) {
            const long long m = b + ring;
            ImGui::PushID(static_cast<int>(m));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##pt", st.mode == m, 0, ImVec2(40, 40)))
                st.mode = m;
            drawMark(ImGui::GetWindowDrawList(), {p.x + 20, p.y + 20}, 10, m, col);
            ImGui::PopID();
            if (b != 4)
                ImGui::SameLine();
        }
    }
    ImGui::Checkbox("Set size relative to screen", &st.relative);
    ImGui::BeginDisabled(st.relative);
    ImGui::SetNextItemWidth(160);
    ImGui::InputDouble("Point size (mm)", &st.size, 0, 0, "%.6g");
    ImGui::EndDisabled();
    if (st.size < 0)
        st.size = 0;

    ImGui::BeginDisabled(pc.commandActive);
    if (ImGui::Button("OK")) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.10g", st.relative ? 0.0 : st.size);
        if (pc.cancelCommand)
            pc.cancelCommand();
        pc.runCommand("PTYPE");
        pc.runCommand(std::to_string(st.mode));
        pc.runCommand(buf);
    }
    ImGui::EndDisabled();
}

}  // namespace

void registerPointStylePanel(ui::UiRegistry& ui) {
    ui.addPanel("pointstyle", "Point Style", drawPointStyle, false);
    ui.addMenuItem("Format/Point Style...", "DDPTYPE", "format.pointstyle");
}

}  // namespace tbcad::draw2
