#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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

// A short list of annotation scales, held on the heap (deep copy) so that an entity stays small:
// most have none.
class ScaleList {
public:
    ScaleList() = default;
    ScaleList(std::vector<double> v) {
        if (!v.empty())
            p_ = std::make_unique<std::vector<double>>(std::move(v));
    }
    ScaleList(const ScaleList& o) {
        if (o.p_)
            p_ = std::make_unique<std::vector<double>>(*o.p_);
    }
    ScaleList(ScaleList&&) noexcept = default;
    ScaleList& operator=(const ScaleList& o) {
        if (this != &o)
            p_ = o.p_ ? std::make_unique<std::vector<double>>(*o.p_) : nullptr;
        return *this;
    }
    ScaleList& operator=(ScaleList&&) noexcept = default;

    bool empty() const { return !p_ || p_->empty(); }
    std::size_t size() const { return p_ ? p_->size() : 0; }
    double front() const { return p_->front(); }
    const double* begin() const { return p_ ? p_->data() : nullptr; }
    const double* end() const { return p_ ? p_->data() + p_->size() : nullptr; }
    std::vector<double> get() const { return p_ ? *p_ : std::vector<double>{}; }
    friend bool operator==(const ScaleList& a, const ScaleList& b) { return a.get() == b.get(); }

private:
    std::unique_ptr<std::vector<double>> p_{};
};

struct EntityCommon {
    EntityId id{};  // assigned by the document; ignored by Transaction::add
    float linetypeScale = 1;
    LayerId layer{};  // null is replaced by the document's current layer on add
    LevelId level{};  // null is replaced by the document's current level on add
    LinetypeId linetype = kLinetypeByLayer;
    Color color = Color::byLayer();
    LineWeight lineweight = LineWeight::byLayer();
    bool visible = true;
    // Annotative: text height, dimension sizes and table text follow the annotation scale
    // (Header custom "tbcad.annoscale", or the viewport's scale when plotting a layout).
    bool annotative = false;
    // Scales (N of 1:N) the annotative object is made for. Empty: it follows the drawing's current
    // scale. Otherwise the first is the one it is drawn at in model space; a layout viewport shows
    // it only when its scale is listed, and draws it at the viewport scale.
    ScaleList annoScales{};
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
    Table,  // after Proxy so the numbers of the older kinds stay as written in files
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

// Marks a Text as an attribute definition (ATTDEF). Only meaningful inside a block definition:
// each Insert of the block shows its own value for `tag` in place of this text (see
// InsertData::attribs and docs/blocks.md).
struct AttDefInfo {
    std::string tag{};           // no spaces, upper case
    std::string prompt{};        // shown when the value is asked for
    std::string defaultValue{};  // value when none is given; the fixed value of a constant one
    bool invisible = false;      // value is stored but not drawn
    bool constant = false;       // fixed value, never asked for or edited per insert
    bool verify = false;         // ask twice (stored, not enforced by the data model)
    bool preset = false;         // takes the default without asking, editable later
};

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
    std::optional<AttDefInfo> attdef{};  // set: this text is an attribute definition
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

// A gradient fill: the colour runs from `color1` to `color2` across the hatch, along `angle`
// (radians, direction of the colour change). Linear goes colour 1 -> colour 2; Cylinder goes
// 1 -> 2 -> 1 (brightest in the middle). A hatch with a gradient is solid (`HatchData::solid`).
enum class GradientKind : std::uint8_t { Linear = 1, Cylinder = 2 };

struct GradientData {
    GradientKind kind = GradientKind::Linear;
    Color color1 = Color::rgb(30, 60, 200);
    Color color2 = Color::rgb(190, 220, 255);
    double angle = 0;
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
    std::optional<GradientData> gradient{};  // set: fill with a colour gradient
};

// Value of one attribute of an Insert, matched to the block's attribute definition by tag.
struct AttribValue {
    std::string tag{};
    std::string value{};
};

// An associative array: the Insert's block holds the source objects and the Insert is shown once
// per item. All geometry is in the block's own coordinates, so moving, rotating or scaling the
// Insert moves the whole array, and the parameters stay editable (ARRAYEDIT).
struct ArrayInfo {
    enum class Kind : std::uint8_t { Rectangular, Polar, Path };
    Kind kind = Kind::Rectangular;
    // Rectangular: rows run along the grid's Y axis, columns along X; negative spacing goes the
    // other way. `angle` turns the grid.
    int rows = 1;
    int columns = 1;
    double rowSpacing = 0;
    double columnSpacing = 0;
    double angle = 0;
    // Polar: `count` items including the original; fillAngle 2*pi spreads them evenly around the
    // circle, anything else puts the last item at the end of the sweep. Items turn with the circle
    // when rotateItems is set, otherwise `reference` (a point of the source) travels around it.
    Vec2 center{};
    int count = 1;
    double fillAngle = kTwoPi;
    bool rotateItems = true;
    Vec2 reference{};
    // Path: the polyline `path`; item 0 puts `reference` on its start, the others follow at
    // `pathSpacing` along it (0 = divide the length evenly over `count` items). rotateItems aligns
    // the items to the path direction.
    std::vector<PolyVertex> path{};
    bool pathClosed = false;
    double pathSpacing = 0;
};

struct InsertData {
    BlockId block{};
    Vec2 position{};
    Vec2 scale{1, 1};
    double rotation = 0;
    std::vector<AttribValue> attribs{};  // non-constant attributes of the block, by tag
    std::optional<ArrayInfo> array{};    // set: shown once per array item
    // Values of the block's dynamic parameters, in parameter order (missing = the default).
    std::vector<double> dynValues{};
    // Clipping boundary (XCLIP): a closed polygon in the block's own coordinates, so it follows
    // the insert when that is moved, rotated or scaled. Fewer than 3 points = not clipped.
    std::vector<Vec2> clip{};
    bool clipOn = true;  // false: the boundary is kept but the whole block shows
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

// One cell of a table. A merged range is stored on its top-left cell (colSpan/rowSpan > 1); the
// other cells of the range are `covered` and ignored.
struct TableCell {
    std::string utf8{};  // MText markup (plain text is fine)
    std::uint16_t colSpan = 1;
    std::uint16_t rowSpan = 1;
    bool covered = false;
};

// A grid of text cells. `origin` is the top-left corner, rows go down and columns go right in the
// table's own frame, which is rotated by `rotation` around `origin`.
struct TableData {
    Vec2 origin{};
    double rotation = 0;
    std::vector<double> colWidths{};
    std::vector<double> rowHeights{};
    std::vector<TableCell> cells{};  // row-major, rowHeights.size() * colWidths.size()
    double textHeight = 2.5;
    double margin = 1.0;    // between cell border and text
    TextStyleId style{};    // null = document's current text style
    bool titleRow = false;  // the first row is a title: its text is centred
};

// The alternatives are in EntityKind order. Larger kinds are boxed in Indirect<> to keep Entity
// small; use entityAs<T>() / visitEntity() instead of std::get_if / std::visit.
using EntityData =
    std::variant<LineData, CircleData, ArcData, PolylineData, PointData, Indirect<EllipseData>,
                 Indirect<SplineData>, Indirect<TextData>, Indirect<MTextData>, Indirect<HatchData>,
                 Indirect<InsertData>, Indirect<DimensionData>, Indirect<LeaderData>,
                 Indirect<ProxyData>, Indirect<TableData>>;

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
                  std::is_same_v<T, ProxyData> || std::is_same_v<T, TableData>) {
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
