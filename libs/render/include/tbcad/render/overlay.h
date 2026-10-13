#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "tbcad/core/color.h"
#include "tbcad/core/math.h"
#include "tbcad/render/camera.h"

namespace tbcad::render {

enum class Space : std::uint8_t { World, Screen };

// Snap and grip markers. Square = endpoint, Triangle = midpoint, Circle = centre,
// X = intersection, Cross = node/insertion point.
enum class MarkerKind : std::uint8_t { Square, Triangle, Circle, X, Cross };

struct OverlayStyle {
    Rgba color{255, 255, 255, 255};
    double widthPx = 1.0;
    // Dashes are measured in screen pixels. dashPx == 0 means a solid line.
    double dashPx = 0;
    double gapPx = 0;
};

struct OverlayVertex {
    float x = 0, y = 0;  // screen pixels, origin top-left, y down
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
};

// Triangle list in screen space; this is all the GPU needs to draw an overlay.
struct OverlayMesh {
    std::vector<OverlayVertex> vertices{};
};

// Immediate-mode drawing for previews, grips, snap markers, rubber bands and selection windows.
// Rebuilt every frame: clear(), add shapes, then Renderer::draw(overlay). World-space input is
// converted with the camera in double precision, so far coordinates stay exact. Shapes keep
// their order; later shapes are drawn over earlier ones.
class Overlay {
public:
    void clear() { shapes_.clear(); }
    bool empty() const { return shapes_.empty(); }

    void line(Space space, Vec2 a, Vec2 b, const OverlayStyle& style);
    void polyline(Space space, std::span<const Vec2> pts, bool closed, const OverlayStyle& style);
    // Filled simple polygon (concave allowed, no holes).
    void fill(Space space, std::span<const Vec2> ring, Rgba color);
    // A marker of `sizePx` pixels (full width) centred on `p`.
    void marker(Space space, Vec2 p, MarkerKind kind, double sizePx, Rgba color,
                bool filled = false, double widthPx = 1.5);
    // Dashed line from the base point to the cursor while a command waits for the next point.
    void rubberBand(Space space, Vec2 a, Vec2 b, Rgba color = {255, 255, 255, 255});
    // Selection rectangle in screen pixels. A window selection (drag left to right) is a solid
    // blue border over a translucent blue fill; a crossing selection (right to left) is a
    // dashed green border over a translucent green fill.
    void selectionWindow(Vec2 cornerA, Vec2 cornerB, bool crossing);
    // Same box with the colours chosen by the caller (theme tokens).
    void selectionWindow(Vec2 cornerA, Vec2 cornerB, bool crossing, Rgba border, Rgba fill);

    OverlayMesh build(const Camera2D& camera) const;

private:
    struct Shape {
        enum class Type : std::uint8_t { Polyline, Fill, Marker } type = Type::Polyline;
        Space space = Space::World;
        std::vector<Vec2> pts{};
        bool closed = false;
        OverlayStyle style{};
        MarkerKind marker = MarkerKind::Square;
        double sizePx = 0;
        bool filled = false;
    };
    std::vector<Shape> shapes_{};
};

}  // namespace tbcad::render
