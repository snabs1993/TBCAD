#include "icons.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace tbcad::app {

namespace {

constexpr float kPi = 3.14159265358979f;

// Maps the 20 x 20 design grid onto the screen square.
struct Pen {
    ImDrawList* dl;
    ImVec2 origin;
    float k;  // pixels per grid unit
    ImU32 color;
    float thick;

    ImVec2 p(float x, float y) const { return {origin.x + x * k, origin.y + y * k}; }
    void line(float x1, float y1, float x2, float y2) const {
        dl->AddLine(p(x1, y1), p(x2, y2), color, thick);
    }
    void poly(std::initializer_list<ImVec2> pts, bool closed = false) const {
        ImVec2 prev{};
        ImVec2 first{};
        bool have = false;
        for (const ImVec2& v : pts) {
            ImVec2 q = p(v.x, v.y);
            if (have)
                dl->AddLine(prev, q, color, thick);
            else
                first = q;
            prev = q;
            have = true;
        }
        if (closed && have)
            dl->AddLine(prev, first, color, thick);
    }
    void rect(float x1, float y1, float x2, float y2) const {
        dl->AddRect(p(x1, y1), p(x2, y2), color, 0, 0, thick);
    }
    void circle(float cx, float cy, float r) const {
        dl->AddCircle(p(cx, cy), r * k, color, 0, thick);
    }
    // Angles in degrees, counter-clockwise on the screen with y pointing up.
    void arc(float cx, float cy, float r, float a0, float a1) const {
        dl->PathClear();
        const int n = 16;
        for (int i = 0; i <= n; ++i) {
            const float a = (a0 + (a1 - a0) * static_cast<float>(i) / n) * kPi / 180.0f;
            dl->PathLineTo(p(cx + r * std::cos(a), cy - r * std::sin(a)));
        }
        dl->PathStroke(color, 0, thick);
    }
    void dot(float x, float y, float r = 1.1f) const { dl->AddCircleFilled(p(x, y), r * k, color); }
    void arrow(float x1, float y1, float x2, float y2) const {
        line(x1, y1, x2, y2);
        const float a = std::atan2(y2 - y1, x2 - x1);
        for (float s : {-0.45f, 0.45f})
            line(x2, y2, x2 - 3.0f * std::cos(a + s), y2 - 3.0f * std::sin(a + s));
    }
};

}  // namespace

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 min, float size, ImU32 color) {
    const Pen g{dl, min, size / 20.0f, color, std::max(1.0f, 1.5f * size / 20.0f)};
    switch (icon) {
        case Icon::New:
            g.poly({{5, 2.5f}, {11.5f, 2.5f}, {15, 6}, {15, 17.5f}, {5, 17.5f}}, true);
            g.poly({{11.5f, 2.5f}, {11.5f, 6}, {15, 6}});
            break;
        case Icon::Open:
            g.poly({{2.5f, 16}, {2.5f, 5}, {8, 5}, {9.5f, 7}, {16, 7}, {16, 10}});
            g.poly({{2.5f, 16}, {5, 10}, {18, 10}, {15.5f, 16}}, true);
            break;
        case Icon::Save:
            g.poly({{3, 3}, {14, 3}, {17, 6}, {17, 17}, {3, 17}}, true);
            g.rect(6, 3, 12, 7);
            g.rect(6, 11, 14, 17);
            break;
        case Icon::Undo:
            g.arc(10, 11, 5.5f, 160, -90);
            g.poly({{4.5f, 4}, {4.5f, 9}, {9.5f, 9}});
            break;
        case Icon::Redo:
            g.arc(10, 11, 5.5f, 20, 270);
            g.poly({{15.5f, 4}, {15.5f, 9}, {10.5f, 9}});
            break;
        case Icon::Line:
            g.line(4, 16, 16, 4);
            g.dot(4, 16);
            g.dot(16, 4);
            break;
        case Icon::Polyline:
            g.poly({{3, 15}, {7, 6}, {12, 13}, {17, 4}});
            g.dot(3, 15, 1);
            g.dot(7, 6, 1);
            g.dot(12, 13, 1);
            g.dot(17, 4, 1);
            break;
        case Icon::Circle:
            g.circle(10, 10, 6.5f);
            g.dot(10, 10, 0.9f);
            break;
        case Icon::Arc:
            g.arc(10, 14, 9, 30, 150);
            g.dot(2.2f, 9.5f, 1);
            g.dot(17.8f, 9.5f, 1);
            break;
        case Icon::Rectangle:
            g.rect(3.5f, 5.5f, 16.5f, 14.5f);
            break;
        case Icon::Hatch:
            g.rect(3.5f, 3.5f, 16.5f, 16.5f);
            g.line(3.5f, 10, 10, 3.5f);
            g.line(3.5f, 16.5f, 16.5f, 3.5f);
            g.line(10, 16.5f, 16.5f, 10);
            break;
        case Icon::Text:
            g.line(4, 4.5f, 16, 4.5f);
            g.line(10, 4.5f, 10, 16.5f);
            g.line(7.5f, 16.5f, 12.5f, 16.5f);
            break;
        case Icon::Move:
            g.line(10, 2.5f, 10, 17.5f);
            g.line(2.5f, 10, 17.5f, 10);
            g.poly({{8, 4.5f}, {10, 2.5f}, {12, 4.5f}});
            g.poly({{8, 15.5f}, {10, 17.5f}, {12, 15.5f}});
            g.poly({{4.5f, 8}, {2.5f, 10}, {4.5f, 12}});
            g.poly({{15.5f, 8}, {17.5f, 10}, {15.5f, 12}});
            break;
        case Icon::Copy:
            g.rect(3.5f, 6.5f, 12, 16);
            g.poly({{7, 6.5f}, {7, 3.5f}, {16.5f, 3.5f}, {16.5f, 13}, {12, 13}});
            break;
        case Icon::Rotate:
            g.arc(10, 10, 6.5f, 40, 330);
            g.poly({{14.5f, 2.5f}, {15.5f, 5.5f}, {12.5f, 6.5f}});
            break;
        case Icon::Scale:
            g.rect(3.5f, 9.5f, 10.5f, 16.5f);
            g.arrow(9, 11, 16.5f, 3.5f);
            break;
        case Icon::Mirror:
            g.line(10, 2.5f, 10, 17.5f);
            g.poly({{7.5f, 5}, {3, 15}, {7.5f, 15}}, true);
            g.poly({{12.5f, 5}, {17, 15}, {12.5f, 15}}, true);
            break;
        case Icon::Stretch:
            g.rect(3, 6.5f, 8.5f, 13.5f);
            g.arrow(8.5f, 10, 17.5f, 10);
            break;
        case Icon::Trim:
            g.line(9, 2.5f, 9, 17.5f);
            g.line(2.5f, 10, 9, 10);
            g.line(11, 10, 12.5f, 10);
            g.line(14.5f, 10, 17.5f, 10);
            break;
        case Icon::Offset:
            g.arc(3, 17, 14, 0, 90);
            g.arc(3, 17, 8, 0, 90);
            break;
        case Icon::Fillet:
            g.line(3, 3.5f, 3, 12);
            g.arc(8, 12, 5, 180, 270);
            g.line(8, 17, 17, 17);
            break;
        case Icon::Erase:
            g.poly({{3.5f, 12}, {9, 5}, {16.5f, 9.5f}, {11, 16}}, true);
            g.line(6.5f, 14, 13.5f, 8);
            g.line(11, 16.5f, 17.5f, 16.5f);
            break;
        case Icon::ZoomExtents:
            g.poly({{3, 7}, {3, 3}, {7, 3}});
            g.poly({{13, 3}, {17, 3}, {17, 7}});
            g.poly({{17, 13}, {17, 17}, {13, 17}});
            g.poly({{7, 17}, {3, 17}, {3, 13}});
            g.rect(7, 7, 13, 13);
            break;
        case Icon::Count:
            break;
    }
}

bool iconButton(const char* id, Icon icon, float size, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    if (enabled && ImGui::IsItemHovered())
        dl->AddRectFilled(lo, hi, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    if (enabled && ImGui::IsItemActive())
        dl->AddRectFilled(lo, hi, ImGui::GetColorU32(ImGuiCol_ButtonActive), 3.0f);
    const float pad = size * 0.18f;
    const ImU32 col = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    drawIcon(dl, icon, ImVec2(lo.x + pad, lo.y + pad), size - 2 * pad, col);
    return pressed && enabled;
}

}  // namespace tbcad::app
