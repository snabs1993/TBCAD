#include "wheel_menu.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "icons.h"
#include "tbcad/ui/radial_wheel.h"

namespace tbcad::app {

namespace {

ImU32 colour(Rgba c) {
    return IM_COL32(c.r, c.g, c.b, c.a);
}

// Tools that have a tool-strip icon; the rest show their name only.
const std::map<std::string, Icon>& iconFor() {
    static const std::map<std::string, Icon> m = {{"LINE", Icon::Line},
                                                  {"POLYLINE", Icon::Polyline},
                                                  {"CIRCLE", Icon::Circle},
                                                  {"ARC", Icon::Arc},
                                                  {"RECTANGLE", Icon::Rectangle},
                                                  {"HATCH", Icon::Hatch},
                                                  {"TEXT", Icon::Text},
                                                  {"MOVE", Icon::Move},
                                                  {"COPY", Icon::Copy},
                                                  {"ROTATE", Icon::Rotate},
                                                  {"SCALE", Icon::Scale},
                                                  {"MIRROR", Icon::Mirror},
                                                  {"STRETCH", Icon::Stretch},
                                                  {"TRIM", Icon::Trim},
                                                  {"OFFSET", Icon::Offset},
                                                  {"FILLET", Icon::Fillet},
                                                  {"ERASE", Icon::Erase},
                                                  {"UNDO", Icon::Undo},
                                                  {"REDO", Icon::Redo},
                                                  {"SAVE", Icon::Save},
                                                  {"QSAVE", Icon::Save},
                                                  {"OPEN", Icon::Open},
                                                  {"NEW", Icon::New},
                                                  {"ZOOM", Icon::ZoomExtents}};
    return m;
}

}  // namespace

void WheelMenu::open(ImVec2 centre, std::vector<std::string> tools) {
    open_ = true;
    centre_ = centre;
    tools_ = std::move(tools);
    tools_.resize(ui::kWheelSlots);
    hover_ = -1;
}

std::string WheelMenu::hovered() const {
    return open_ && hover_ >= 0 && hover_ < static_cast<int>(tools_.size())
               ? tools_[static_cast<std::size_t>(hover_)]
               : std::string{};
}

void WheelMenu::draw(ImVec2 mouse, float scale, const ui::Theme& theme) {
    if (!open_)
        return;
    const float inner = 34.0f * scale;
    const float outer = 124.0f * scale;
    const int n = ui::kWheelSlots;
    constexpr float kPi = 3.14159265358979f;
    const float half = kPi / n;
    hover_ = ui::wheelSliceAt(mouse.x - centre_.x, mouse.y - centre_.y, inner, n);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 fill = colour(ui::withAlpha(theme.bgPanel, 0.92));
    const ImU32 hot = colour(ui::withAlpha(theme.accent, 0.85));
    const ImU32 line = colour(theme.border);
    const ImU32 text = colour(theme.text);
    const ImU32 muted = colour(theme.textMuted);

    for (int i = 0; i < n; ++i) {
        // ImGui angles run clockwise from +x; slice 0 is centred on up (-pi/2).
        const float mid = static_cast<float>(ui::wheelSliceCentre(i, n)) - kPi / 2;
        const float a0 = mid - half + 0.012f;
        const float a1 = mid + half - 0.012f;
        dl->PathClear();
        dl->PathArcTo(centre_, outer, a0, a1, 24);
        dl->PathArcTo(centre_, inner, a1, a0, 12);
        dl->PathFillConvex(i == hover_ ? hot : fill);
        dl->PathClear();
        dl->PathArcTo(centre_, outer, a0, a1, 24);
        dl->PathArcTo(centre_, inner, a1, a0, 12);
        dl->PathStroke(line, ImDrawFlags_Closed, 1.0f);

        const std::string& tool = tools_[static_cast<std::size_t>(i)];
        const float r = (inner + outer) * 0.5f;
        const ImVec2 c(centre_.x + std::cos(mid) * r, centre_.y + std::sin(mid) * r);
        const ImU32 fg = tool.empty() ? muted : text;
        if (tool.empty()) {
            const ImVec2 ts = ImGui::CalcTextSize("-");
            dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), fg, "-");
            continue;
        }
        const auto it = iconFor().find(tool);
        const ImVec2 ts = ImGui::CalcTextSize(tool.c_str());
        if (it != iconFor().end()) {
            const float is = 24.0f * scale;
            drawIcon(dl, it->second, ImVec2(c.x - is / 2, c.y - is / 2 - ts.y * 0.45f), is, fg);
            dl->AddText(ImVec2(c.x - ts.x / 2, c.y + is / 2 - ts.y * 0.35f), fg, tool.c_str());
        } else {
            dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), fg, tool.c_str());
        }
    }
    // Centre: shows the hovered command's name; the dead zone cancels on release.
    dl->AddCircleFilled(centre_, inner - 4.0f * scale, fill, 24);
    dl->AddCircle(centre_, inner - 4.0f * scale, line, 24, 1.0f);
    const char* mark = hover_ < 0 ? "x" : nullptr;
    if (mark) {
        const ImVec2 ts = ImGui::CalcTextSize(mark);
        dl->AddText(ImVec2(centre_.x - ts.x / 2, centre_.y - ts.y / 2), muted, mark);
    }
}

}  // namespace tbcad::app
