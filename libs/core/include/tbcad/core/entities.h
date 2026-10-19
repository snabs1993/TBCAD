#pragma once

#include <algorithm>
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
#include "tbcad/core/tables.h"

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

// An optional value held on the heap: nothing but a pointer when it is not set, so a field that
// most entities leave empty (an attribute definition, an array) does not make every entity as big
// as the value. Reads like std::optional; copies are deep.
template <class T>
class Boxed {
public:
    Boxed() = default;
    Boxed(std::nullopt_t) {}                               // NOLINT
    Boxed(T v) : p_(std::make_unique<T>(std::move(v))) {}  // NOLINT
    Boxed(std::optional<T> v) { *this = std::move(v); }    // NOLINT
    Boxed(const Boxed& o) : p_(o.p_ ? std::make_unique<T>(*o.p_) : nullptr) {}
    Boxed(Boxed&&) noexcept = default;
    Boxed& operator=(const Boxed& o) {
        if (this != &o)
            p_ = o.p_ ? std::make_unique<T>(*o.p_) : nullptr;
        return *this;
    }
    Boxed& operator=(Boxed&&) noexcept = default;
    Boxed& operator=(std::nullopt_t) {
        p_.reset();
        return *this;
    }
    Boxed& operator=(T v) {
        if (p_)
            *p_ = std::move(v);
        else
            p_ = std::make_unique<T>(std::move(v));
        return *this;
    }
    Boxed& operator=(std::optional<T> v) {
        if (v)
            *this = std::move(*v);
        else
            p_.reset();
        return *this;
    }
    operator std::optional<T>() const {  // NOLINT
        return p_ ? std::optional<T>(*p_) : std::nullopt;
    }

    bool has_value() const { return p_ != nullptr; }
    explicit operator bool() const { return p_ != nullptr; }
    T& operator*() { return *p_; }
    const T& operator*() const { return *p_; }
    T* operator->() { return p_.get(); }
    const T* operator->() const { return p_.get(); }
    T& value() {
        if (!p_)
            throw std::bad_optional_access();
        return *p_;
    }
    const T& value() const {
        if (!p_)
            throw std::bad_optional_access();
        return *p_;
    }
    template <class U>
    T value_or(U&& fallback) const {
        return p_ ? *p_ : static_cast<T>(std::forward<U>(fallback));
    }
    void reset() { p_.reset(); }
    template <class... Args>
    T& emplace(Args&&... args) {
        p_ = std::make_unique<T>(std::forward<Args>(args)...);
        return *p_;
    }
    friend bool operator==(const Boxed& a, const Boxed& b) {
        return a.p_ ? (b.p_ && *a.p_ == *b.p_) : !b.p_;
    }

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

// One value of extended entity data or of a dictionary record, tagged with its DXF group code. The
// code decides which member is meaningful: see DataKind.
struct DataItem {
    int code = 0;
    std::string text{};        // string, handle and binary (hex) groups
    std::int64_t integer = 0;  // integer and boolean groups
    double real = 0;           // floating point groups; a point is three items (x, y, z codes)
    friend bool operator==(const DataItem&, const DataItem&) = default;
};
enum class DataKind : std::uint8_t { Text, Integer, Real };
// Which member of DataItem a DXF group code uses.
DataKind dataKindOf(int code);

// XDATA of one application: the items between its 1001 group and the next (1002 braces included).
struct XdataApp {
    std::string app{};
    std::vector<DataItem> items{};
    friend bool operator==(const XdataApp&, const XdataApp&) = default;
};
// An entry of an entity's extension dictionary: a named XRECORD and its items.
struct DictRecord {
    std::string name{};
    std::vector<DataItem> items{};
    friend bool operator==(const DictRecord&, const DictRecord&) = default;
};
// Application data an entity carries from other programs, kept so that saving does not lose it.
struct ExtData {
    std::vector<XdataApp> xdata{};
    std::vector<DictRecord> dict{};
    bool empty() const { return xdata.empty() && dict.empty(); }
    friend bool operator==(const ExtData&, const ExtData&) = default;
};
// Heap-held ExtData with deep-copy semantics; null for the usual entity that has none.
class ExtDataPtr {
public:
    ExtDataPtr() = default;
    ExtDataPtr(ExtData v) {  // NOLINT: implicit by design
        if (!v.empty())
            p_ = std::make_unique<ExtData>(std::move(v));
    }
    ExtDataPtr(const ExtDataPtr& o) {
        if (o.p_)
            p_ = std::make_unique<ExtData>(*o.p_);
    }
    ExtDataPtr(ExtDataPtr&&) noexcept = default;
    ExtDataPtr& operator=(const ExtDataPtr& o) {
        if (this != &o)
            p_ = o.p_ ? std::make_unique<ExtData>(*o.p_) : nullptr;
        return *this;
    }
    ExtDataPtr& operator=(ExtDataPtr&&) noexcept = default;

    bool empty() const { return !p_ || p_->empty(); }
    const ExtData& get() const {
        static const ExtData none;
        return p_ ? *p_ : none;
    }
    // Mutable access; creates the data on first use. Call trim() after emptying it.
    ExtData& edit() {
        if (!p_)
            p_ = std::make_unique<ExtData>();
        return *p_;
    }
    void trim() {
        if (p_ && p_->empty())
            p_.reset();
    }
    friend bool operator==(const ExtDataPtr& a, const ExtDataPtr& b) { return a.get() == b.get(); }

private:
    std::unique_ptr<ExtData> p_{};
};

// Transparency of an object or layer, in percent: 0 (opaque) to 90. Objects may also follow their
// layer or, inside a block, the insert.
inline constexpr std::int8_t kTransparencyByLayer = -1;
inline constexpr std::int8_t kTransparencyByBlock = -2;
inline constexpr std::int8_t kTransparencyMax = 90;
inline constexpr bool validObjectTransparency(int v) {
    return v == kTransparencyByLayer || v == kTransparencyByBlock ||
           (v >= 0 && v <= kTransparencyMax);
}

struct EntityCommon {
    EntityId id{};  // assigned by the document; ignored by Transaction::add
    float linetypeScale = 1;
    LayerId layer{};  // null is replaced by the document's current layer on add
    LevelId level{};  // null is replaced by the document's current level on add
    LinetypeId linetype = kLinetypeByLayer;
    Color color = Color::byLayer();
    LineWeight lineweight = LineWeight::byLayer();
    std::int8_t transparency = kTransparencyByLayer;
    bool visible : 1 = true;
    // Annotative: text height, dimension sizes and table text follow the annotation scale
    // (Header custom "tbcad.annoscale", or the viewport's scale when plotting a layout).
    bool annotative : 1 = false;
    // Wipeout: a closed polyline that hides the objects drawn before it, instead of showing its
    // outline.
    bool wipeout : 1 = false;
    // Scales (N of 1:N) the annotative object is made for. Empty: it follows the drawing's current
    // scale. Otherwise the first is the one it is drawn at in model space; a layout viewport shows
    // it only when its scale is listed, and draws it at the viewport scale.
    ScaleList annoScales{};
    // Extended data (XDATA) and extension dictionary records from other programs.
    ExtDataPtr ext{};
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
    Ray,    // RAY and XLINE
    MLine,
    MLeader,  // MLEADER: one object with several leader lines and a note or block
};

// Number of entity kinds, for tables indexed by kind.
inline constexpr std::size_t kEntityKindCount = static_cast<std::size_t>(EntityKind::MLeader) + 1;

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
    // PEDIT Ltype gen off: a dashed linetype starts again at every vertex instead of running on
    // along the whole polyline. (Next to `closed` so the two flags share one word: an Entity
    // stays within 96 bytes.)
    bool restartLinetype = false;
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
    friend bool operator==(const AttDefInfo&, const AttDefInfo&) = default;
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
    Boxed<AttDefInfo> attdef{};  // set: this text is an attribute definition
    // Text generation (DXF 71): drawn mirrored left to right / top to bottom about its insertion
    // point. Combined with the same flags of the text style (either one turns it on, both cancel).
    bool backward = false;
    bool upsideDown = false;
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
    // Columns: with `columns` > 1 and a `width` the text flows through that many columns of
    // `width` each, `columnGap` apart (0 = twice the text height), balanced so they end at about
    // the same height.
    std::uint8_t columns = 1;
    double columnGap = 0;
    // Line pitch as a multiple of the usual one (DXF 44: 0.25 to 4).
    double lineSpacing = 1.0;
    // Background mask (DXF 90/63/45): 0 none, 1 hides what is behind in the drawing background
    // colour (like a wipeout), 2 fills with palette colour `bgColor`. The box is the text frame
    // grown by (bgScale - 1) text heights on every side (bgScale 1 to 5).
    std::uint8_t bgMode = 0;
    int bgColor = 7;
    double bgScale = 1.5;
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
    bool doubled = false;  // pattern drawn twice, the second set turned 90 degrees (DXF 77)
    Vec2 patternOrigin{};
    Color fill = Color::byLayer();
    std::vector<EntityId> sourceBoundary{};  // for later associativity
    std::optional<GradientData> gradient{};  // set: fill with a colour gradient
};

// Value of one attribute of an Insert, matched to the block's attribute definition by tag.
struct AttribValue {
    std::string tag{};
    std::string value{};
    // Where the value is drawn, relative to its attribute definition (block coordinates); moved
    // by the attribute's grip.
    Vec2 offset{};
};

// An associative array: the Insert's block holds the source objects and the Insert is shown once
// per item. All geometry is in the block's own coordinates, so moving, rotating or scaling the
// Insert moves the whole array, and the parameters stay editable (ARRAYEDIT).
// An array item shown as other objects (ARRAYEDIT REPlace): the base point of `block` lands on
// `at` (a point in the array's own block) carried along with the item.
struct ArrayItemSwap {
    int item = 0;
    BlockId block{};
    Vec2 at{};
    friend bool operator==(const ArrayItemSwap&, const ArrayItemSwap&) = default;
};

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
    // Items taken out (Ctrl+click, ARRAYREMOVE) and items shown as other objects (ARRAYEDIT
    // REPlace), by item index, both sorted; an item is in at most one of them. RESet clears them.
    std::vector<int> removed{};
    std::vector<ArrayItemSwap> swaps{};
};

struct InsertData {
    BlockId block{};
    Vec2 position{};
    Vec2 scale{1, 1};
    double rotation = 0;
    std::vector<AttribValue> attribs{};  // non-constant attributes of the block, by tag
    Boxed<ArrayInfo> array{};            // set: shown once per array item
    // Values of the block's dynamic parameters, in parameter order (missing = the default).
    std::vector<double> dynValues{};
    // Values of the block's assembly parameters by name (missing = the default).
    std::vector<AsmValue> asmValues{};
    bool joinOff = false;  // assembly insert: stays separate from overlapping assemblies
    // Clipping boundary (XCLIP): a closed polygon in the block's own coordinates, so it follows
    // the insert when that is moved, rotated or scaled. Fewer than 3 points = not clipped.
    std::vector<Vec2> clip{};
    bool clipOn = true;       // false: the boundary is kept but the whole block shows
    bool clipInvert = false;  // true: show what is outside the boundary (XCLIP Invert)
};

enum class DimKind : std::uint8_t {
    Linear,
    Aligned,
    Radius,
    Diameter,
    Angular2Line,
    Angular3Point,
    ArcLength,  // defPts = {centre, start point, end point, point on the dimension arc}
    // defPts = {feature point, leader end, origin}; rotation 0 measures X (leader runs along Y),
    // pi / 2 measures Y.
    Ordinate,
    // defPts = {true centre, point on the arc, centre override, jog centre}; the text sits at
    // textPos (or on the line when textPosUser is false).
    Jogged,
};

// A gap in a dimension line or extension line (DIMBREAK): the part of the line between a and b is
// not drawn. Both points lie on the line, so a break that no longer matches a line (the dimension
// was moved or restyled) does nothing.
struct DimBreak {
    Vec2 a{}, b{};
    friend bool operator==(const DimBreak&, const DimBreak&) = default;
};

// A definition point of a dimension that follows a point of another object (an associative
// dimension). Vertex: a line's end (0 = a, 1 = b), a polyline vertex, an arc's end (0 = start,
// 1 = end) or a point object. Center: the centre of a circle or an arc.
struct DimAssoc {
    enum class Kind : std::uint8_t { None, Vertex, Center };
    Kind kind = Kind::None;
    EntityId target{};
    std::uint32_t index = 0;
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
    std::vector<DimBreak> breaks{};
    // DIMEDIT Oblique: how far the extension lines of a linear or aligned dimension lean from the
    // perpendicular of the dimension line (radians, counter-clockwise).
    double oblique = 0;
    // DIMEDIT Rotate: the text angle when it is fixed by hand, else it follows the style.
    bool textRotated = false;
    double textRotation = 0;
    // DIMJOGLINE: where the dimension line of a linear or aligned dimension has a jog (a Z-shaped
    // kink that says the measured length is shortened on the drawing). Points on the line.
    std::vector<Vec2> jogs{};
    // Associative dimensions: the objects whose points the definition points follow. When such an
    // object is edited the point moves with it; moving the dimension alone, or copying it, cuts the
    // link.
    std::array<DimAssoc, 4> assoc{};
};

struct LeaderData {
    std::vector<Vec2> vertices{};
    DimStyleId style{};
    std::string utf8{};  // annotation
    Vec2 textPos{};
};

// Read-only placeholder for an unknown imported entity.
//
// For a DXF entity this program has no reader for (3DSOLID, REGION, SURFACE, TABLE ...) `raw`
// holds the groups of the record, so that saving as DXF writes the entity back as it was:
// kProxyDxfFresh, then lines "code TAB value" (see libs/dxf/src/keep). Anything that changes the
// entity's place (transform) turns the prefix into kProxyDxfStale: the groups no longer fit and
// only the display graphics are written.
struct ProxyData {
    std::string originalType{};
    std::vector<std::uint8_t> raw{};
    DisplayList graphics{};
};

inline constexpr std::string_view kProxyDxfFresh = "TBDXF1\n";
inline constexpr std::string_view kProxyDxfStale = "TBDXF0\n";

// The preserved DXF groups of a proxy, or empty when it has none or they are stale.
inline std::string_view proxyDxfGroups(const ProxyData& p) {
    if (p.raw.size() <= kProxyDxfFresh.size())
        return {};
    const std::string_view all(reinterpret_cast<const char*>(p.raw.data()), p.raw.size());
    return all.starts_with(kProxyDxfFresh) ? all.substr(kProxyDxfFresh.size()) : std::string_view{};
}

// The proxy moved or changed shape: its preserved DXF groups are out of date.
inline void staleProxyDxf(ProxyData& p) {
    if (proxyDxfGroups(p).empty())
        return;
    std::copy(kProxyDxfStale.begin(), kProxyDxfStale.end(), p.raw.begin());
}

// One cell of a table. A merged range is stored on its top-left cell (colSpan/rowSpan > 1); the
// other cells of the range are `covered` and ignored.
struct TableCell {
    std::string utf8{};  // MText markup (plain text is fine)
    std::uint16_t colSpan = 1;
    std::uint16_t rowSpan = 1;
    bool covered = false;
};

// How the cells of one role (the title row, the header row, every other row) look: the cell
// styles of a table style. 0 and -1 mean "as the cell is".
struct TableCellStyle {
    double textHeight = 0;  // 0 = the table's text height
    int color = 0;          // text colour, palette index 1..255; 0 = the cell's own
    int fill = 0;           // background fill, palette index 1..255; 0 = none
    int align = -1;         // 0 left, 1 centre, 2 right; -1 = left (centre for a title)
    friend bool operator==(const TableCellStyle&, const TableCellStyle&) = default;
    bool isDefault() const { return *this == TableCellStyle{}; }
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
    // Cell styles by role: the title row (row 0 when titleRow), the header row (the next one,
    // when headerRow) and every other row.
    bool headerRow = false;
    TableCellStyle titleStyle{}, headerStyle{}, dataStyle{};
};

// A line that goes on without end: a ray from `origin` along `dir` (unit vector), or with
// `infinite` set an XLINE through `origin` that runs both ways. It is drawn as a very long
// segment and is left out of zoom extents and plot extents.
struct RayData {
    Vec2 origin{};
    Vec2 dir{1, 0};
    bool infinite = false;
};
// How far a ray or xline is drawn and selected, in drawing units.
inline constexpr double kRayReach = 1e5;
// Distance from the origin of the grip that turns a ray or xline.
inline constexpr double kRayGripReach = 100;

// The drawn segment of a ray or xline: a ray starts at its origin, an xline reaches both ways.
inline Vec2 rayStart(const RayData& d) {
    return d.infinite ? d.origin - d.dir * kRayReach : d.origin;
}
inline Vec2 rayEnd(const RayData& d) {
    return d.origin + d.dir * kRayReach;
}

// A multiline: parallel lines along a path of vertices, offsets from the style (MLSTYLE).
// justification 0 = top, 1 = zero (the path is the centre), 2 = bottom.
// A gap in some of the lines of a multiline (MLEDIT cuts, open tees and crosses). It runs from
// parameter t0 to t1 of segment `seg` (0 = its first vertex, 1 = the next) and hides the lines
// whose rank, counted from the lowest style offset, lies in lo..hi.
struct MlineCut {
    std::uint32_t seg = 0;
    double t0 = 0;
    double t1 = 1;
    std::uint8_t lo = 0;
    std::uint8_t hi = 255;
};

struct MlineData {
    std::string style = "STANDARD";
    double scale = 1;
    int justification = 1;
    bool closed = false;
    std::vector<Vec2> v{};
    std::vector<MlineCut> cuts{};
};

// A multileader: several leader lines that meet at one landing point, then a short dogleg to a
// note or a block. Arrowheads and sizes come from the dimension style (MLEADERSTYLE).
struct MLeaderData {
    enum class Content : std::uint8_t { None, Text, Block };
    std::vector<std::vector<Vec2>> lines{};  // each runs from its arrowhead towards the landing
    Vec2 landing{};                          // where the lines meet
    Vec2 contentPos{};   // end of the dogleg: the note starts here (or ends, on the left side)
    bool dogleg = true;  // draw landing -> contentPos
    DimStyleId style{};
    Content content = Content::Text;
    std::string utf8{};     // note, TBCAD markup
    double textHeight = 0;  // 0 = the dimension style's text height
    BlockId block{};        // Content::Block: inserted at contentPos
    double blockScale = 1;
    double blockRotation = 0;
};

// The alternatives are in EntityKind order. Larger kinds are boxed in Indirect<> to keep Entity
// small; use entityAs<T>() / visitEntity() instead of std::get_if / std::visit.
using EntityData =
    std::variant<LineData, CircleData, ArcData, PolylineData, PointData, Indirect<EllipseData>,
                 Indirect<SplineData>, Indirect<TextData>, Indirect<MTextData>, Indirect<HatchData>,
                 Indirect<InsertData>, Indirect<DimensionData>, Indirect<LeaderData>,
                 Indirect<ProxyData>, Indirect<TableData>, Indirect<RayData>, Indirect<MlineData>,
                 Indirect<MLeaderData>>;

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
                  std::is_same_v<T, ProxyData> || std::is_same_v<T, TableData> ||
                  std::is_same_v<T, RayData> || std::is_same_v<T, MlineData> ||
                  std::is_same_v<T, MLeaderData>) {
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
