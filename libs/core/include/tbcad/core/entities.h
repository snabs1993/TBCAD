#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "tbcad/core/color.h"
#include "tbcad/core/display.h"
#include "tbcad/core/ids.h"
#include "tbcad/core/math.h"

namespace tbcad {

// Heap-held value with deep-copy semantics. Used inside EntityData for the larger entity kinds so
// that an Entity stays small (a million lines must fit comfortably in memory).
template <class T>
class Indirect {
public:
    Indirect() : p_(std::make_unique<T>()) {}
    Indirect(T v) : p_(std::make_unique<T>(std::move(v))) {}  // NOLINT: implicit by design
    Indirect(const Indirect& o) : p_(o.p_ ? std::make_unique<T>(*o.p_) : nullptr) {}
    Indirect(Indirect&&) noexcept = default;
    Indirect& operator=(const Indirect& o) {
        if (this != &o)
            p_ = o.p_ ? std::make_unique<T>(*o.p_) : nullptr;
        return *this;
    }
    Indirect& operator=(Indirect&&) noexcept = default;

    T& operator*() { return *p_; }
    const T& operator*() const { return *p_; }
    T* operator->() { return p_.get(); }
    const T* operator->() const { return p_.get(); }

private:
    std::unique_ptr<T> p_{};
};

struct EntityCommon {
    EntityId id{};  // assigned by the document; ignored by Transaction::add
    double linetypeScale = 1;
    LayerId layer{};  // null is replaced by the document's current layer on add
    LinetypeId linetype = kLinetypeByLayer;
    Color color = Color::byLayer();
    LineWeight lineweight = LineWeight::byLayer();
    bool visible = true;
};

enum class EntityKind : std::uint8_t {
    Line,
    Circle,
    Arc,
    Polyline,
    Point,
    Ellipse,
    Spline,
    Text,
    MText,
    Hatch,
    Insert,
    Dimension,
    Leader,
    Proxy,
};

struct LineData {
    Vec2 a{}, b{};
};

struct CircleData {
    Vec2 center{};
    double radius = 0;
};

// Counter-clockwise from startAngle to endAngle, angles in [0, 2*pi). A full circle is a
// CircleData, never an arc with equal angles.
struct ArcData {
    Vec2 center{};
    double radius = 0;
    double startAngle = 0, endAngle = 0;
};

// bulge = tan(sweep / 4); positive = counter-clockwise. The bulge belongs to the segment that
// starts at this vertex.
struct PolyVertex {
    Vec2 p{};
    double bulge = 0;
    double startWidth = 0, endWidth = 0;
};

struct PolylineData {
    std::vector<PolyVertex> v{};
    bool closed = false;
    double constantWidth = 0;
};

struct PointData {
    Vec2 p{};
};

struct EllipseData {
    Vec2 center{};
    Vec2 majorAxis{};  // vector from the center to the end of the major axis
    double ratio = 1;  // minor / major
    double startParam = 0;
    double endParam = kTwoPi;
};

struct SplineData {
    int degree = 3;
    std::vector<double> knots{};
    std::vector<Vec2> control{};
    std::vector<double> weights{};  // empty = non-rational
    std::vector<Vec2> fit{};
    bool closed = false;
    bool periodic = false;
};

enum class HAlign : std::uint8_t { Left, Center, Right, Aligned, Middle, Fit };
enum class VAlign : std::uint8_t { Baseline, Bottom, Middle, Top };

struct TextData {
    Vec2 position{};
    Vec2 alignPoint{};
    double height = 2.5;
    double rotation = 0;
    double widthFactor = 1;
    double oblique = 0;
    TextStyleId style{};  // null = document's current text style
    HAlign h = HAlign::Left;
    VAlign v = VAlign::Baseline;
    std::string utf8{};
};

enum class Attachment : std::uint8_t {
    TopLeft,
    TopCenter,
    TopRight,
    MiddleLeft,
    MiddleCenter,
    MiddleRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

struct MTextData {
    Vec2 position{};
    double height = 2.5;
    double width = 0;  // 0 = no wrapping
    double rotation = 0;
    Attachment attach = Attachment::TopLeft;
    TextStyleId style{};
    std::string utf8{};  // TBCAD markup
};

struct HatchLineEdge {
    Vec2 a{}, b{};
};
struct HatchArcEdge {
    Vec2 center{};
    double radius = 0;
    double startAngle = 0, endAngle = 0;
    bool ccw = true;
};
struct HatchEllipseEdge {
    Vec2 center{}, majorAxis{};
    double ratio = 1;
    double startParam = 0, endParam = kTwoPi;
    bool ccw = true;
};
struct HatchSplineEdge {
    SplineData spline{};
};
struct HatchPolylineEdge {
    std::vector<PolyVertex> v{};
    bool closed = true;
};
using HatchEdge =
    std::variant<HatchLineEdge, HatchArcEdge, HatchEllipseEdge, HatchSplineEdge, HatchPolylineEdge>;

struct HatchLoop {
    std::vector<HatchEdge> edges{};
    bool outer = true;
};

struct HatchData {
    std::vector<HatchLoop> loops{};
    bool solid = true;
    std::string pattern{};  // pattern name when !solid
    double patternScale = 1;
    double patternAngle = 0;
    Vec2 patternOrigin{};
    Color fill = Color::byLayer();
    std::vector<EntityId> sourceBoundary{};  // for later associativity
};

struct InsertData {
    BlockId block{};
    Vec2 position{};
    Vec2 scale{1, 1};
    double rotation = 0;
};

enum class DimKind : std::uint8_t {
    Linear,
    Aligned,
    Radius,
    Diameter,
    Angular2Line,
    Angular3Point,
    ArcLength,  // reserved
};

struct DimensionData {
    DimKind kind = DimKind::Linear;
    double rotation = 0;  // Linear only
    std::array<Vec2, 4> defPts{};
    Vec2 textPos{};
    bool textPosUser = false;
    std::string textOverride{};
    DimStyleId style{};      // null = document's current dim style
    double measurement = 0;  // cache
};

struct LeaderData {
    std::vector<Vec2> vertices{};
    DimStyleId style{};
    std::string utf8{};  // annotation
    Vec2 textPos{};
};

// Read-only placeholder for an unknown imported entity.
struct ProxyData {
    std::string originalType{};
    std::vector<std::uint8_t> raw{};
    DisplayList graphics{};
};

// The alternatives are in EntityKind order. Larger kinds are boxed in Indirect<> to keep Entity
// small; use entityAs<T>() / visitEntity() instead of std::get_if / std::visit.
using EntityData = std::variant<LineData, CircleData, ArcData, PolylineData, PointData,
                                Indirect<EllipseData>, Indirect<SplineData>, Indirect<TextData>,
                                Indirect<MTextData>, Indirect<HatchData>, Indirect<InsertData>,
                                Indirect<DimensionData>, Indirect<LeaderData>, Indirect<ProxyData>>;

struct Entity {
    EntityCommon common{};
    EntityData data{};
};

inline EntityKind kind(const Entity& e) {
    return static_cast<EntityKind>(e.data.index());
}

namespace detail {
template <class T>
struct IsIndirect : std::false_type {};
template <class T>
struct IsIndirect<Indirect<T>> : std::true_type {};
}  // namespace detail

// Pointer to the entity's payload when it is of type T, else nullptr.
template <class T>
T* entityAs(Entity& e) {
    if constexpr (std::is_same_v<T, EllipseData> || std::is_same_v<T, SplineData> ||
                  std::is_same_v<T, TextData> || std::is_same_v<T, MTextData> ||
                  std::is_same_v<T, HatchData> || std::is_same_v<T, InsertData> ||
                  std::is_same_v<T, DimensionData> || std::is_same_v<T, LeaderData> ||
                  std::is_same_v<T, ProxyData>) {
        auto* p = std::get_if<Indirect<T>>(&e.data);
        return p ? &**p : nullptr;
    } else {
        return std::get_if<T>(&e.data);
    }
}
template <class T>
const T* entityAs(const Entity& e) {
    return entityAs<T>(const_cast<Entity&>(e));
}

// Calls fn(const XxxData&) / fn(XxxData&) with the unwrapped payload.
template <class Fn>
decltype(auto) visitEntity(Entity& e, Fn&& fn) {
    return std::visit(
        [&](auto& d) -> decltype(auto) {
            if constexpr (detail::IsIndirect<std::decay_t<decltype(d)>>::value) {
                return fn(*d);
            } else {
                return fn(d);
            }
        },
        e.data);
}
template <class Fn>
decltype(auto) visitEntity(const Entity& e, Fn&& fn) {
    return std::visit(
        [&](const auto& d) -> decltype(auto) {
            if constexpr (detail::IsIndirect<std::decay_t<decltype(d)>>::value) {
                return fn(*d);
            } else {
                return fn(d);
            }
        },
        e.data);
}

}  // namespace tbcad
