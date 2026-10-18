#pragma once

#include <optional>
#include <string>
#include <vector>

#include "tbcad/core/color.h"
#include "tbcad/core/ids.h"
#include "tbcad/core/math.h"
#include "tbcad/core/units.h"

namespace tbcad {

// Table rows carry their id; the document assigns it when a row is added.

struct Layer {
    LayerId id{};
    std::string name{};
    LayerId parent{};  // null = top level; names are unique per parent (S-FLOOR > edge)
    Color color = Color::index(7);                        // never ByLayer / ByBlock
    LinetypeId linetype{};                                // filled with "Continuous" when null
    LineWeight lineweight = LineWeight::defaultWeight();  // Default, a value, or ByParent
    bool on = true;
    bool frozen = false;
    bool locked = false;
    bool plot = true;
    std::uint8_t transparency = 0;  // percent, 0 to 90
    std::string description{};
};

// A level groups objects the way a floor plan groups storeys: the user works on one level at a
// time and objects on different levels may occupy the same XY position without interfering
// (see docs/levels.md). Independent of layers: every object has both a layer and a level.
struct Level {
    LevelId id{};
    std::string name{};
    double elevation = 0;  // mm, for ordering and display; the model stays 2D
    bool visible = true;   // switched off: hidden everywhere (also in plots)
    bool locked = false;   // shown but cannot be selected, snapped to or edited
};

// A text string or shape drawn along a line at a fixed place in the linetype's cycle (the
// bracketed elements of a .lin file). It takes no length of its own: `pattern` holds the dashes
// and gaps around it.
struct LinetypeMark {
    enum class Kind : std::uint8_t { Text, Shape };
    // How `rotation` is read: from the line's direction, from the X axis, or from the line's
    // direction but never upside down.
    enum class Rotation : std::uint8_t { Relative, Absolute, Upright };
    Kind kind = Kind::Text;
    double offset = 0;    // along the cycle where the mark sits, in pattern units
    std::string text{};   // the text, or the name of the shape
    std::string style{};  // text style name (text) or shape file (shape)
    double scale = 1;     // text height or shape size; 0 = the text style's own height
    double rotation = 0;  // radians
    Rotation rotationKind = Rotation::Relative;
    Vec2 shift{};  // from the pen position, along and across the line
    friend bool operator==(const LinetypeMark&, const LinetypeMark&) = default;
};

struct Linetype {
    LinetypeId id{};
    std::string name{};
    std::string description{};
    std::vector<double> pattern{};      // + dash, - gap, 0 dot; in mm. Empty = continuous
    std::vector<LinetypeMark> marks{};  // text and shapes along the line, in cycle order
};

struct TextStyle {
    TextStyleId id{};
    std::string name{};
    std::string fontFamily{};  // empty = application default
    std::string fontFile{};
    double widthFactor = 1;
    double oblique = 0;  // radians
    // Fixed text height (AutoCAD's STYLE height). 0 = variable: the height is asked for or taken
    // from the drawing's sizing; above 0 every TEXT / MTEXT in this style gets that height.
    double height = 0;
    // The AutoCAD shape font (and big font) a DXF/DWG style named, for example "romans.shx". Used
    // when the file can be found; else the stroke font stands in (fontFamily). widthFactor then
    // includes the stand-in's 1/0.92 correction, which a real SHX font undoes.
    std::string shxFile{};
    std::string bigFontFile{};
    // New text in this style is annotative (sized by the annotation scale).
    bool annotative = false;
};

enum class ArrowType { ClosedFilled, Open30, ArchTick, Dot, None };
enum class DimTextPlacement { AboveLine, Centered };
enum class DimTextAlign { WithDimLine, Horizontal };
// How a dimension shows its tolerance next to the measurement: "±0.05", "+0.05 -0.02", or the two
// limits "100.05 / 99.98".
enum class DimTolerance : std::uint8_t { None, Symmetrical, Deviation, Limits };

struct DimStyle {
    DimStyleId id{};
    std::string name{};
    double textHeight = 2.5;
    TextStyleId textStyle{};  // null = document's current text style
    double textGap = 0.625;
    ArrowType arrowType = ArrowType::ClosedFilled;
    double arrowSize = 2.5;
    double extLineOffset = 0.625;
    double extLineExtension = 1.25;
    double dimLineExtension = 0;
    DimTextPlacement textPlacement = DimTextPlacement::AboveLine;
    DimTextAlign textAlign = DimTextAlign::WithDimLine;
    LengthUnit units = LengthUnit::Millimeter;
    Precision precision = 2;
    std::string prefix{};
    std::string suffix{};
    double roundOff = 0;      // 0 = none
    double overallScale = 1;  // DIMSCALE equivalent
    bool suppressLeadingZeros = false;
    bool suppressTrailingZeros = false;
    Color dimLineColor = Color::byBlock();
    Color extLineColor = Color::byBlock();
    Color textColor = Color::byBlock();
    // Alternate units: the measurement a second time, in brackets after the first
    // ("100.00 [3.94]"). `altScale` multiplies the millimetres before they are shown in
    // `altUnit` (25.4 would turn millimetres into inches by itself; the unit does that already,
    // so the default 1 reads "the same length in another unit"). Length dimensions only.
    bool altUnits = false;
    double altScale = 1;
    LengthUnit altUnit = LengthUnit::Inch;
    Precision altPrecision = 2;
    std::string altPrefix{};
    std::string altSuffix{};
    // Tolerances, in millimetres (degrees for angular dimensions). Symmetrical uses `tolUpper`
    // for both signs; Deviation shows +tolUpper and -tolLower; Limits shows measurement + upper
    // and measurement - lower.
    DimTolerance tolerance = DimTolerance::None;
    double tolUpper = 0;
    double tolLower = 0;
    Precision tolPrecision = 2;
};

// ---- Dynamic blocks --------------------------------------------------------------------------
// A block can carry parameters and actions. Each Insert stores one value per parameter
// (InsertData::dynValues); the block's entities are the default state and the actions turn them
// into the state the values ask for. All coordinates are in block space.
struct DynParam {
    enum class Kind : std::uint8_t {
        Linear,      // value = distance from `base` along base->end (default: their distance)
        Rotation,    // value = angle (radians) about `base`; `end` is the handle at angle 0
        Flip,        // value 0 or 1; `base`->`end` is the mirror axis
        Visibility,  // value = index into `states`
    };
    Kind kind = Kind::Linear;
    std::string name{};
    Vec2 base{};
    Vec2 end{1, 0};
    // Linear and rotation limits; used when max > min.
    double min = 0;
    double max = 0;
    // Visibility: state names, and for each the objects shown in it. Objects named by no state are
    // always shown.
    std::vector<std::string> states{};
    std::vector<std::vector<EntityId>> stateObjects{};
};

struct DynAction {
    enum class Kind : std::uint8_t {
        Stretch,  // linear: points of `objects` inside the frame move by the change in distance
        Move,     // linear: `objects` move along the parameter's direction
        Scale,    // linear: `objects` scale about `base` by value / default
        Rotate,   // rotation: `objects` turn about the parameter's base
        Flip,     // flip: `objects` are mirrored in the axis when the value is 1
    };
    Kind kind = Kind::Move;
    int param = 0;  // index into DynamicDef::params
    std::vector<EntityId> objects{};
    Vec2 frameMin{}, frameMax{};  // Stretch only
};

struct DynamicDef {
    std::vector<DynParam> params{};
    std::vector<DynAction> actions{};
};

// ---- Parametric assemblies -------------------------------------------------------------------
// A block with named parameters whose values (per Insert) feed formulas that drive the block's
// dynamic parameters, attribute texts and nested inserts. See docs/assembly.md.
struct AsmValue {
    std::string name{};
    bool isText = false;
    double number = 0;
    std::string text{};
};

struct AsmParam {
    enum class Kind : std::uint8_t { Number, Integer, Text, Choice };
    Kind kind = Kind::Number;
    std::string name{};
    double defaultNumber = 0;
    std::string defaultText{};
    // Number / Integer limits, used when max > min.
    double min = 0;
    double max = 0;
    std::vector<std::string> choices{};  // Choice: the allowed texts; the default is the first
};

struct AsmBinding {
    enum class Kind : std::uint8_t {
        DynParam,    // value of a dynamic parameter of this block (number; a state name or index)
        Attribute,   // text of an attribute of this block
        Nested,      // a parameter of the insert `entity` inside this block
        ArrayField,  // rows, columns, count, rowSpacing, columnSpacing or pathSpacing of the array
                     // of the insert `entity` inside this block
    };
    Kind kind = Kind::DynParam;
    EntityId entity{};     // Nested, ArrayField: the Insert entity in this block
    std::string target{};  // parameter / attribute tag / array field name
    std::string formula{};
};

struct AsmPreset {
    std::string name{};
    std::vector<AsmValue> values{};
};

struct AssemblyDef {
    std::vector<AsmParam> params{};
    std::vector<AsmBinding> bindings{};
    std::vector<AsmPreset> presets{};
    // Non-empty: inserts of this block that overlap other inserts with the same class merge into
    // one body (see blocks/join.h).
    std::string joinClass{};
};

struct BlockDef {
    BlockId id{};
    std::string name{};
    Vec2 base{};
    std::vector<EntityId> entities{};
    bool anonymous = false;
    // External reference: the block's entities are a snapshot of the model space of another
    // drawing (.tbcad or .dxf) at `xrefPath`, as typed by the user (relative paths are resolved
    // against the host drawing). Empty = an ordinary block. `xrefStamp` is the source file's
    // last-write time (nanoseconds since the Unix epoch) when the snapshot was taken.
    std::string xrefPath{};
    std::int64_t xrefStamp = 0;
    // Overlay xref: shown in this drawing but left out when this drawing is itself used as an
    // xref by another one (an attached xref is carried along, an overlay is not).
    bool xrefOverlay = false;
    std::optional<DynamicDef> dynamic{};
    std::optional<AssemblyDef> assembly{};
};

}  // namespace tbcad

namespace tbcad {

// A layer's state after resolving sub-layer inheritance: on/plot are ANDed over the ancestors,
// frozen/locked ORed, and ByParent colour/linetype/lineweight follow the chain upwards.
struct EffectiveLayer {
    LayerId id{};
    Color color = Color::index(7);                        // Index or Rgb
    LinetypeId linetype{1};                               // never null / ByParent
    LineWeight lineweight = LineWeight::defaultWeight();  // Default or a value
    std::uint8_t transparency = 0;
    bool on = true;
    bool frozen = false;
    bool locked = false;  // includes `inactive`
    bool plot = true;
    // Locked only because the document is in "active only" mode and this layer is outside the
    // current layer's branch.
    bool inactive = false;
    // Locked only because the object lies on a level other than the current one.
    bool otherLevel = false;

    // Drawn at all: on and not frozen.
    bool visible() const { return on && !frozen; }
    // Visible and on the current level (or levels are all shown): takes part in snapping and
    // boundary detection.
    bool live() const { return visible() && !otherLevel; }
};

}  // namespace tbcad
