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
    ImU32 accent;  // colour of the accent part of an icon

    // The same pen drawing in the accent colour.
    Pen a() const {
        Pen b = *this;
        b.color = accent;
        return b;
    }
    // Same colour at a fraction of its opacity (for soft fills).
    static ImU32 faint(ImU32 c, int alpha) {
        return (c & 0x00ffffffu) | (static_cast<ImU32>((c >> 24) * alpha / 255) << 24);
    }
    // A solid shape (convex) in a faint tone of the pen colour.
    void fill(std::initializer_list<ImVec2> pts, int alpha = 70) const {
        ImVec2 buf[12];
        int n = 0;
        for (const ImVec2& v : pts)
            if (n < 12)
                buf[n++] = p(v.x, v.y);
        dl->AddConvexPolyFilled(buf, n, faint(color, alpha));
    }
    void fillRect(float x1, float y1, float x2, float y2, int alpha = 255) const {
        dl->AddRectFilled(p(x1, y1), p(x2, y2), faint(color, alpha));
    }
    void dash(float x1, float y1, float x2, float y2) const {
        const float len = std::hypot(x2 - x1, y2 - y1);
        const float on = 1.8f, off = 1.4f;
        for (float t = 0; t < len; t += on + off) {
            const float e = std::min(t + on, len);
            line(x1 + (x2 - x1) * t / len, y1 + (y2 - y1) * t / len, x1 + (x2 - x1) * e / len,
                 y1 + (y2 - y1) * e / len);
        }
    }
    void dashRect(float x1, float y1, float x2, float y2) const {
        dash(x1, y1, x2, y1);
        dash(x2, y1, x2, y2);
        dash(x2, y2, x1, y2);
        dash(x1, y2, x1, y1);
    }

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

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 min, float size, ImU32 color, ImU32 accent) {
    const Pen g{dl,    min,   size / 20.0f, color, std::max(1.0f, 1.5f * size / 20.0f),
                accent != 0 ? accent : color};
    const Pen a = g.a();
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
        case Icon::Ellipse:
            g.dl->PathClear();
            for (int i = 0; i <= 32; ++i) {
                const float ang = 2.0f * kPi * static_cast<float>(i) / 32.0f;
                g.dl->PathLineTo(g.p(10 + 7.5f * std::cos(ang), 10 + 4.5f * std::sin(ang)));
            }
            g.dl->PathStroke(g.color, 0, g.thick);
            break;
        case Icon::Spline:
            g.dl->AddBezierCubic(g.p(2.5f, 14), g.p(6, 3), g.p(11, 18), g.p(17.5f, 6), g.color,
                                 g.thick);
            g.dot(2.5f, 14, 1);
            g.dot(17.5f, 6, 1);
            break;
        case Icon::Polygon:
            g.poly({{10, 2.5f}, {16.5f, 7.5f}, {14, 16}, {6, 16}, {3.5f, 7.5f}}, true);
            break;
        case Icon::Point:
            g.line(10, 4, 10, 16);
            g.line(4, 10, 16, 10);
            g.circle(10, 10, 4.5f);
            break;
        case Icon::Dimension:
            g.line(3, 6, 3, 15);
            g.line(17, 6, 17, 15);
            g.line(3, 11, 17, 11);
            g.poly({{6, 9.5f}, {3, 11}, {6, 12.5f}});
            g.poly({{14, 9.5f}, {17, 11}, {14, 12.5f}});
            break;
        case Icon::Block:
            g.rect(3.5f, 3.5f, 16.5f, 16.5f);
            g.rect(7, 7, 13, 13);
            break;
        case Icon::Insert:
            g.rect(8, 8, 16.5f, 16.5f);
            g.arrow(3, 3, 8, 8);
            break;
        case Icon::Array:
            for (float x : {3.5f, 11.0f})
                for (float y : {3.5f, 11.0f})
                    g.rect(x, y, x + 5.5f, y + 5.5f);
            break;
        case Icon::Explode:
            g.line(10, 10, 10, 3);
            g.line(10, 10, 17, 10);
            g.line(10, 10, 10, 17);
            g.line(10, 10, 3, 10);
            g.dot(10, 10, 1.4f);
            g.line(5, 5, 7, 7);
            g.line(15, 5, 13, 7);
            g.line(5, 15, 7, 13);
            g.line(15, 15, 13, 13);
            break;
        case Icon::Chamfer:
            g.line(3, 3.5f, 3, 10);
            g.line(3, 10, 10, 17);
            g.line(10, 17, 17, 17);
            break;
        case Icon::Extend:
            g.line(16, 3, 16, 17);
            g.line(3, 10, 11, 10);
            g.poly({{11, 10}, {12.5f, 10}});
            g.line(13, 10, 16, 10);
            g.poly({{12.5f, 8}, {15.5f, 10}, {12.5f, 12}});
            break;
        case Icon::Join:
            g.line(2.5f, 13, 8.5f, 13);
            g.line(11.5f, 7, 17.5f, 7);
            g.line(8.5f, 13, 11.5f, 7);
            g.dot(8.5f, 13, 1.2f);
            g.dot(11.5f, 7, 1.2f);
            break;
        // ---- draw
        case Icon::Ray:
            g.line(4, 16, 14, 6);
            a.arrow(11, 9, 17, 3);
            a.dot(4, 16, 1.4f);
            break;
        case Icon::Xline:
            g.line(3.5f, 16.5f, 16.5f, 3.5f);
            g.poly({{3.5f, 12.5f}, {3.5f, 16.5f}, {7.5f, 16.5f}});
            g.poly({{12.5f, 3.5f}, {16.5f, 3.5f}, {16.5f, 7.5f}});
            a.dot(10, 10, 1.5f);
            break;
        case Icon::Mline:
            g.line(3, 5.5f, 17, 5.5f);
            a.line(3, 10, 17, 10);
            g.line(3, 14.5f, 17, 14.5f);
            g.line(3, 4, 3, 16);
            g.line(17, 4, 17, 16);
            break;
        case Icon::Sketch:
            g.dl->AddBezierCubic(g.p(2.5f, 13), g.p(5, 4), g.p(8, 4), g.p(10, 10), g.color, g.thick);
            g.dl->AddBezierCubic(g.p(10, 10), g.p(11.5f, 15), g.p(14, 16), g.p(16.5f, 8), g.color,
                                 g.thick);
            a.dot(16.5f, 8, 1.5f);
            break;
        case Icon::Donut:
            g.circle(10, 10, 7);
            a.circle(10, 10, 3.2f);
            break;
        case Icon::Revcloud:
            for (float cx : {6.0f, 10.0f, 14.0f}) {
                g.arc(cx, 6, 2, 0, 180);
                g.arc(cx, 14, 2, 180, 360);
            }
            g.arc(4, 8, 2, 90, 270);
            g.arc(4, 12, 2, 90, 270);
            g.arc(16, 8, 2, -90, 90);
            g.arc(16, 12, 2, -90, 90);
            a.dot(10, 10, 1.1f);
            break;
        case Icon::Divide:
            g.line(3, 14, 17, 6);
            a.dot(6.5f, 12, 1.2f);
            a.dot(10, 10, 1.2f);
            a.dot(13.5f, 8, 1.2f);
            g.line(3, 11.5f, 3, 16.5f);
            g.line(17, 3.5f, 17, 8.5f);
            break;
        case Icon::Measure:
            g.rect(2.5f, 7, 17.5f, 13);
            for (int i = 1; i < 6; ++i)
                g.line(2.5f + 2.5f * i, 7, 2.5f + 2.5f * i, i % 2 ? 10 : 11);
            a.line(2.5f + 7.5f, 7, 2.5f + 7.5f, 13);
            break;
        case Icon::Wipeout:
            g.line(2.5f, 7, 6, 7);
            g.line(14, 7, 17.5f, 7);
            g.line(2.5f, 13, 6, 13);
            g.line(14, 13, 17.5f, 13);
            a.fillRect(6, 4, 14, 16, 90);
            a.rect(6, 4, 14, 16);
            break;
        case Icon::Solid:
            g.fill({{3.5f, 15.5f}, {16.5f, 15.5f}, {13.5f, 4.5f}, {6.5f, 4.5f}}, 255);
            a.dot(10, 10, 1.4f);
            break;
        case Icon::Boundary:
            g.line(2.5f, 7.5f, 17.5f, 7.5f);
            g.line(2.5f, 12.5f, 17.5f, 12.5f);
            g.line(7.5f, 2.5f, 7.5f, 17.5f);
            g.line(12.5f, 2.5f, 12.5f, 17.5f);
            a.rect(7.5f, 7.5f, 12.5f, 12.5f);
            a.dot(10, 10, 1);
            break;
        case Icon::Region:
            g.poly({{3.5f, 14}, {5, 5.5f}, {14, 3.5f}, {16.5f, 12}, {9, 16.5f}}, true);
            a.fill({{3.5f, 14}, {5, 5.5f}, {14, 3.5f}, {16.5f, 12}, {9, 16.5f}}, 90);
            break;
        case Icon::Table:
            g.rect(3, 4, 17, 16);
            a.fillRect(3, 4, 17, 7.5f, 90);
            g.line(3, 7.5f, 17, 7.5f);
            g.line(3, 11.5f, 17, 11.5f);
            g.line(9, 4, 9, 16);
            break;
        // ---- modify
        case Icon::Align:
            g.rect(3, 11, 8, 16);
            g.rect(11, 4, 17, 16);
            a.arrow(8.5f, 10.5f, 12.5f, 6.5f);
            a.dot(3, 11, 1);
            break;
        case Icon::Break:
            g.line(2.5f, 10, 7.5f, 10);
            g.line(12.5f, 10, 17.5f, 10);
            a.poly({{7.5f, 10}, {9, 6.5f}, {11, 13.5f}, {12.5f, 10}});
            break;
        case Icon::BreakAtPoint:
            g.line(2.5f, 10, 8.5f, 10);
            g.line(11.5f, 10, 17.5f, 10);
            a.circle(10, 10, 3.2f);
            a.dot(10, 10, 0.8f);
            break;
        case Icon::Blend:
            g.line(2.5f, 14.5f, 6.5f, 14.5f);
            g.line(13.5f, 5.5f, 17.5f, 5.5f);
            a.dl->AddBezierCubic(a.p(6.5f, 14.5f), a.p(11, 14.5f), a.p(9, 5.5f), a.p(13.5f, 5.5f),
                                 a.color, a.thick);
            break;
        case Icon::Lengthen:
            g.line(3, 10, 11, 10);
            g.line(3, 6.5f, 3, 13.5f);
            a.arrow(11, 10, 17.5f, 10);
            break;
        case Icon::Extrim:
            g.line(10, 2.5f, 10, 17.5f);
            g.line(2.5f, 6, 7.5f, 6);
            a.poly({{7.5f, 3.5f}, {10, 6}, {7.5f, 8.5f}});
            g.line(2.5f, 14, 10, 14);
            g.line(12, 14, 13.5f, 14);
            g.line(15.5f, 14, 17.5f, 14);
            break;
        case Icon::Reverse:
            g.arrow(3, 7, 17, 7);
            a.arrow(17, 13, 3, 13);
            break;
        case Icon::Overkill:
            g.line(3, 13, 14, 5);
            g.line(6, 15, 17, 7);
            a.poly({{10.5f, 15}, {12.5f, 17.5f}, {17, 12}});
            break;
        case Icon::EditObject:
            g.poly({{3, 15}, {8, 6}, {13, 13}, {17, 5}});
            for (const ImVec2 v : {ImVec2{3, 15}, ImVec2{8, 6}, ImVec2{13, 13}, ImVec2{17, 5}})
                a.rect(v.x - 1.4f, v.y - 1.4f, v.x + 1.4f, v.y + 1.4f);
            break;
        case Icon::MatchProp:
            g.rect(2.5f, 6, 8, 12);
            a.fillRect(12, 6, 17.5f, 12, 120);
            g.rect(12, 6, 17.5f, 12);
            a.arrow(8.5f, 15, 14.5f, 15);
            break;
        case Icon::Group:
            a.dashRect(2.5f, 2.5f, 17.5f, 17.5f);
            g.rect(5, 5, 10.5f, 10.5f);
            g.rect(9.5f, 9.5f, 15, 15);
            break;
        case Icon::Ungroup:
            a.poly({{2.5f, 6}, {2.5f, 2.5f}, {6, 2.5f}});
            a.poly({{14, 17.5f}, {17.5f, 17.5f}, {17.5f, 14}});
            g.rect(3.5f, 3.5f, 8.5f, 8.5f);
            g.rect(11.5f, 11.5f, 16.5f, 16.5f);
            break;
        case Icon::DrawOrder:
            g.rect(3, 3, 12, 12);
            a.fillRect(8, 8, 17, 17, 110);
            a.rect(8, 8, 17, 17);
            break;
        case Icon::Union:
            g.arc(7.5f, 10, 5.5f, -63, 63);
            g.arc(12.5f, 10, 5.5f, 117, 243);
            a.arc(7.5f, 10, 5.5f, 63, 297);
            a.arc(12.5f, 10, 5.5f, -117, 117);
            break;
        case Icon::Subtract:
            g.arc(12.5f, 10, 5.5f, -117, 117);
            a.arc(7.5f, 10, 5.5f, 63, 297);
            a.arc(12.5f, 10, 5.5f, 117, 243);
            break;
        case Icon::Intersect:
            g.arc(7.5f, 10, 5.5f, 63, 297);
            g.arc(12.5f, 10, 5.5f, -117, 117);
            a.arc(7.5f, 10, 5.5f, -63, 63);
            a.arc(12.5f, 10, 5.5f, 117, 243);
            break;
        case Icon::Paste:
            g.rect(4, 4.5f, 16, 17.5f);
            a.fillRect(7.5f, 2.5f, 12.5f, 6, 255);
            g.line(7, 10, 13, 10);
            g.line(7, 13.5f, 11, 13.5f);
            break;
        case Icon::Cut:
            g.line(6, 3, 14, 13);
            g.line(14, 3, 6, 13);
            a.circle(5.5f, 15.5f, 2);
            a.circle(14.5f, 15.5f, 2);
            break;
        // ---- annotate
        case Icon::MText:
            g.line(3, 4.5f, 17, 4.5f);
            g.line(3, 8.5f, 17, 8.5f);
            g.line(3, 12.5f, 17, 12.5f);
            a.line(3, 16.5f, 11, 16.5f);
            break;
        case Icon::Field:
            g.rect(2.5f, 5.5f, 17.5f, 14.5f);
            a.fillRect(6, 8, 14, 12, 120);
            a.rect(6, 8, 14, 12);
            break;
        case Icon::DimAligned:
            g.line(5, 17, 2, 14);
            g.line(13, 9, 10, 6);
            a.arrow(6, 10, 2, 14);
            a.arrow(6, 10, 10, 6);
            g.dot(5, 17, 1);
            g.dot(13, 9, 1);
            break;
        case Icon::DimAngular:
            g.line(3, 16, 17, 16);
            g.line(3, 16, 13, 3);
            a.arc(3, 16, 9, 0, 52);
            a.dot(3, 16, 1);
            break;
        case Icon::DimRadius:
            g.circle(10, 10, 7);
            a.arrow(10, 10, 14.9f, 5.1f);
            a.dot(10, 10, 1.1f);
            break;
        case Icon::DimDiameter:
            g.circle(10, 10, 7);
            a.arrow(10, 10, 14.9f, 5.1f);
            a.arrow(10, 10, 5.1f, 14.9f);
            break;
        case Icon::Leader:
            g.arrow(9, 11, 3, 17);
            g.line(9, 11, 13, 7);
            g.line(13, 7, 17, 7);
            a.line(13, 11, 17, 11);
            a.line(13, 14, 16, 14);
            break;
        case Icon::CenterMark:
            g.circle(10, 10, 5.5f);
            a.line(10, 2.5f, 10, 6.5f);
            a.line(10, 13.5f, 10, 17.5f);
            a.line(2.5f, 10, 6.5f, 10);
            a.line(13.5f, 10, 17.5f, 10);
            a.line(8.5f, 10, 11.5f, 10);
            a.line(10, 8.5f, 10, 11.5f);
            break;
        // ---- block
        case Icon::Attribute:
            g.rect(2.5f, 4, 17.5f, 16);
            a.poly({{5, 10}, {8, 7}, {15, 7}, {15, 13}, {8, 13}}, true);
            a.dot(7.5f, 10, 0.8f);
            break;
        case Icon::Xref:
            g.rect(3, 3, 11, 11);
            a.dashRect(9, 9, 17, 17);
            g.line(9, 9, 11, 11);
            break;
        // ---- inquire
        case Icon::Distance:
            g.line(3, 15, 17, 5);
            g.line(1.8f, 12.3f, 5.5f, 17.5f);
            g.line(14.5f, 2.5f, 18.2f, 7.7f);
            a.dot(3, 15, 1.4f);
            a.dot(17, 5, 1.4f);
            break;
        case Icon::Area:
            g.poly({{4, 15}, {6, 5}, {15, 4}, {16, 14}}, true);
            a.fill({{4, 15}, {6, 5}, {15, 4}, {16, 14}}, 90);
            break;
        case Icon::Angle:
            g.arc(10, 16, 8, 0, 180);
            g.line(2, 16, 18, 16);
            for (float t : {45.0f, 90.0f, 135.0f})
                g.line(10 + 6.5f * std::cos(t * kPi / 180), 16 - 6.5f * std::sin(t * kPi / 180),
                       10 + 8 * std::cos(t * kPi / 180), 16 - 8 * std::sin(t * kPi / 180));
            a.line(10, 16, 15.2f, 10.8f);
            break;
        case Icon::Id:
            g.circle(10, 8, 5);
            g.line(10, 17.5f, 6.2f, 11.6f);
            g.line(10, 17.5f, 13.8f, 11.6f);
            a.dot(10, 8, 1.6f);
            break;
        case Icon::List:
            for (float y : {5.0f, 10.0f, 15.0f}) {
                a.dot(4, y, 1.2f);
                g.line(7.5f, y, 17, y);
            }
            break;
        case Icon::Calc:
            g.rect(4, 2.5f, 16, 17.5f);
            a.fillRect(6, 4.5f, 14, 7.5f, 120);
            for (float y : {10.5f, 13.0f, 15.5f})
                for (float x : {7.0f, 10.0f, 13.0f})
                    g.dot(x, y, 0.7f);
            break;
        case Icon::Audit:
            g.poly({{5, 2.5f}, {11.5f, 2.5f}, {15, 6}, {15, 17.5f}, {5, 17.5f}}, true);
            a.poly({{7.5f, 11}, {9.5f, 13.5f}, {13, 8.5f}});
            break;
        case Icon::Purge:
            g.line(3.5f, 6, 16.5f, 6);
            g.poly({{5, 6}, {6, 17.5f}, {14, 17.5f}, {15, 6}});
            g.poly({{8, 6}, {8, 3.5f}, {12, 3.5f}, {12, 6}});
            a.line(8.5f, 9, 8.5f, 14.5f);
            a.line(11.5f, 9, 11.5f, 14.5f);
            break;
        // ---- output
        case Icon::Plot:
            g.rect(3, 8, 17, 15);
            g.poly({{6, 8}, {6, 3}, {14, 3}, {14, 8}});
            a.rect(6, 12, 14, 17.5f);
            a.dot(14.5f, 10.5f, 0.8f);
            break;
        case Icon::Pdf:
            g.poly({{5, 2.5f}, {11.5f, 2.5f}, {15, 6}, {15, 17.5f}, {5, 17.5f}}, true);
            g.poly({{11.5f, 2.5f}, {11.5f, 6}, {15, 6}});
            a.fillRect(3.5f, 10, 16.5f, 14.5f, 255);
            break;
        case Icon::Layers:
            g.poly({{10, 3}, {17.5f, 7}, {10, 11}, {2.5f, 7}}, true);
            a.fill({{10, 3}, {17.5f, 7}, {10, 11}, {2.5f, 7}}, 90);
            g.poly({{2.5f, 10.5f}, {10, 14.5f}, {17.5f, 10.5f}});
            g.poly({{2.5f, 14}, {10, 18}, {17.5f, 14}});
            break;
        case Icon::Levels:
            g.line(4, 2.5f, 4, 17.5f);
            g.line(4, 4.5f, 17, 4.5f);
            a.line(4, 10, 17, 10);
            g.line(4, 15.5f, 17, 15.5f);
            a.dot(4, 10, 1.4f);
            break;
        // ---- switches
        case Icon::Snap:
            for (float x : {4.0f, 10.0f, 16.0f})
                for (float y : {4.0f, 10.0f, 16.0f})
                    g.dot(x, y, 0.8f);
            a.circle(10, 10, 3);
            a.line(10, 5.5f, 10, 14.5f);
            a.line(5.5f, 10, 14.5f, 10);
            break;
        case Icon::Grid:
            g.rect(3, 3, 17, 17);
            g.line(7.67f, 3, 7.67f, 17);
            g.line(12.33f, 3, 12.33f, 17);
            g.line(3, 7.67f, 17, 7.67f);
            g.line(3, 12.33f, 17, 12.33f);
            a.dot(10, 10, 1.3f);
            break;
        case Icon::Ortho:
            g.poly({{4, 3.5f}, {4, 16}, {16.5f, 16}});
            a.rect(4, 11.5f, 8.5f, 16);
            break;
        case Icon::Polar:
            g.arc(4, 16, 11, 0, 90);
            g.line(4, 16, 17, 16);
            g.line(4, 16, 4, 3);
            a.dash(4, 16, 13, 7);
            a.dot(4, 16, 1.3f);
            break;
        case Icon::Osnap:
            g.line(3, 16, 17, 4);
            a.rect(14, 1.5f, 18.5f, 6);
            g.poly({{8.2f, 10.5f}, {12, 10.5f}, {10.1f, 13.7f}}, true);
            break;
        case Icon::Count:
            break;
    }
}

const char* iconName(Icon icon) {
    switch (icon) {
#define X(name, group) \
    case Icon::name:   \
        return #name;
        TBCAD_ICON_LIST(X)
#undef X
        case Icon::Count:
            break;
    }
    return "";
}

IconGroup iconGroup(Icon icon) {
    switch (icon) {
#define X(name, group) \
    case Icon::name:   \
        return IconGroup::group;
        TBCAD_ICON_LIST(X)
#undef X
        case Icon::Count:
            break;
    }
    return IconGroup::General;
}

ImU32 iconAccent(IconGroup group) {
    switch (group) {
        case IconGroup::Draw:
            return IM_COL32(79, 163, 255, 255);
        case IconGroup::Modify:
            return IM_COL32(255, 152, 56, 255);
        case IconGroup::Annotate:
            return IM_COL32(72, 199, 116, 255);
        case IconGroup::Block:
            return IM_COL32(178, 124, 255, 255);
        case IconGroup::Inquire:
            return IM_COL32(38, 198, 218, 255);
        case IconGroup::Output:
            return IM_COL32(236, 100, 140, 255);
        case IconGroup::Toggle:
            return IM_COL32(240, 200, 60, 255);
        case IconGroup::General:
            break;
    }
    return 0;
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
    drawIcon(dl, icon, ImVec2(lo.x + pad, lo.y + pad), size - 2 * pad, col,
             enabled ? iconAccent(iconGroup(icon)) : 0);
    return pressed && enabled;
}

}  // namespace tbcad::app
