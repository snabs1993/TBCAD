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
    // Text generation flags of the style (DXF 71): mirrored left to right / top to bottom.
    bool backward = false;
    bool upsideDown = false;
};

enum class ArrowType { ClosedFilled, Open30, ArchTick, Dot, None };
// Below is DIMTAD 4: the text hangs under the dimension line (linear dimensions).
enum class DimTextPlacement { AboveLine, Centered, Below };
enum class DimTextAlign { WithDimLine, Horizontal };
// How a dimension shows its tolerance next to the measurement: "±0.05", "+0.05 -0.02", or the two
// limits "100.05 / 99.98".
enum class DimTolerance : std::uint8_t { None, Symmetrical, Deviation, Limits };

// What a dimension does when the text and arrows do not both fit between the extension lines
// (DIMATFIT): Best and ArrowsFirst move the arrows out first, then the text; TextFirst moves the
// text out first; BothOutside moves both as soon as they do not fit together.
enum class DimFit : std::uint8_t { Best, ArrowsFirst, TextFirst, BothOutside };
// DIMTMOVE: where text goes when it is dragged away. Outside the extension lines it keeps a
// leader (WithLine, Leader) or none (Free).
enum class DimTextMove : std::uint8_t { WithLine, Leader, Free };
// DIMLUNIT: how a length dimension writes its measurement. Architectural is 12'-6 1/2", Fractional
// 150 1/2" (inches); `precision` is then the fraction (denominator 2^precision, at most 256).
enum class DimLinearFormat : std::uint8_t { Decimal, Architectural, Fractional };

// DIMAUNIT: how an angular dimension writes its measurement.
enum class DimAngleFormat : std::uint8_t { Degrees, Dms, Gradians, Radians };

struct DimStyle {
    DimStyleId id{};
    std::string name{};
    double textHeight = 2.5;
    TextStyleId textStyle{};  // null = document's current text style
    double textGap = 0.625;
    ArrowType arrowType = ArrowType::ClosedFilled;
    ArrowType leaderArrow = ArrowType::ClosedFilled;  // DIMLDRBLK: the arrow of leaders
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
    // DIMLWD / DIMLWE: the lineweights of dimension and extension lines (ByBlock: the
    // dimension's own lineweight).
    LineWeight dimLineWeight = LineWeight::byBlock();
    LineWeight extLineWeight = LineWeight::byBlock();
    // DIMLTYPE / DIMLTEX1 / DIMLTEX2: the linetypes of the dimension line and of each extension
    // line, by name ("ByLayer", or a linetype of the document). Empty or "ByBlock": the
    // dimension's own linetype; a name the document lacks is ignored the same way.
    std::string dimLinetype{};
    std::string ext1Linetype{};
    std::string ext2Linetype{};
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
    // DIMLFAC: multiplies the measurement of every length dimension before it is shown.
    double linearFactor = 1;
    DimFit fit = DimFit::Best;
    DimTextMove textMove = DimTextMove::WithLine;
    // Text and line rules for linear dimensions. `textAlign` is DIMTIH (text between the
    // extension lines), `textAlignOutside` DIMTOH (text beyond them).
    DimTextAlign textAlignOutside = DimTextAlign::WithDimLine;
    bool textInsideForced = false;       // DIMTIX: text always between the extension lines
    bool suppressArrowsOutside = false;  // DIMSOXD: no arrows when they do not fit
    // DIMTOFL: the dimension line is drawn between the extension lines even when the arrows are
    // outside. On by default (how TBCAD always drew it); a file's own value (often 0) is kept.
    bool forceLineInside = true;
    bool suppressExt1 = false, suppressExt2 = false;  // DIMSE1, DIMSE2
    bool suppressDim1 = false,
         suppressDim2 = false;  // DIMSD1, DIMSD2 (half dimension line + arrow)
    int textJustify = 0;  // DIMJUST: 0 centred, 1 next to the first extension line, 2 the second
    // Number format: DIMAUNIT, DIMADEC (decimals of angles; -1 = the same as `precision`) and
    // DIMDSEP (a comma instead of the point between the whole and the fraction).
    DimAngleFormat angleFormat = DimAngleFormat::Degrees;
    int angularPrecision = -1;
    bool commaDecimal = false;
    // Zero suppression of angles (DIMAZIN), tolerances (DIMTZIN) and alternate units (DIMALTZ):
    // bit 1 drops leading zeros, bit 2 trailing ones; -1 = as the length (`suppress...Zeros`).
    int angularZeros = -1;
    int tolZeros = -1;
    int altZeros = -1;
    // DIMALTRND: rounds the alternate value (in alternate units; 0 = none). DIMTOLJ: where the
    // measurement sits against a stacked tolerance (0 bottom, 1 middle, 2 top).
    double altRoundOff = 0;
    int tolJustify = 1;
    // DIMDLI: distance between baseline dimension lines (0 = 2.5 times the text height).
    // DIMCEN: size of the centre mark of radius and diameter dimensions (0 none, negative also
    // draws centre lines).
    double baselineSpacing = 0;
    double centerMark = 0;
    // DIMTFILL (0 none, 1 the drawing background, 2 the colour `textFillColor`, palette index)
    // and a frame round the text (DIMGAP below zero in a DXF).
    int textFill = 0;
    int textFillColor = 0;
    bool textFrame = false;
    DimLinearFormat linearFormat = DimLinearFormat::Decimal;
    // DIMTFAC: the height of a stacked tolerance (each line) as a fraction of the text height.
    double tolHeightFactor = 1;
    // DIMFXLON and DIMFXL: extension lines of a fixed length, measured from the dimension line
    // toward the origin (never nearer to it than the offset).
    bool fixedExtLines = false;
    double fixedExtLength = 1;
    // DIMTVP: with the text centred on the line (DIMTAD 0), the text is moved this many text
    // heights above (or below, negative) the dimension line; the line breaks round it only below
    // 0.7.
    double textVertPos = 0;
    bool hasFixedExt() const { return fixedExtLines || fixedExtLength != 1; }
    bool hasTextBox() const { return textFill != 0 || textFrame; }
    bool hasLinearFormat() const {
        return linearFormat != DimLinearFormat::Decimal || tolHeightFactor != 1;
    }
    bool hasNumberFormat() const {
        return angleFormat != DimAngleFormat::Degrees || angularPrecision != -1 || commaDecimal ||
               baselineSpacing != 0 || centerMark != 0;
    }
    friend bool operator==(const DimStyle&, const DimStyle&) = default;
};

// The text and line rules of a dimension style packed in one byte (bit 0 outside text horizontal,
// 1 DIMTIX, 2 DIMSOXD, 3 DIMTOFL, 4 DIMSE1, 5 DIMSE2, 6 DIMSD1, 7 DIMSD2) for the file formats.
// The default style packs to kDimRulesDefault.
inline constexpr std::uint8_t kDimRulesDefault = 0x08;
inline std::uint8_t packDimRules(const DimStyle& s) {
    return static_cast<std::uint8_t>(
        (s.textAlignOutside == DimTextAlign::Horizontal ? 1 : 0) | (s.textInsideForced ? 2 : 0) |
        (s.suppressArrowsOutside ? 4 : 0) | (s.forceLineInside ? 8 : 0) |
        (s.suppressExt1 ? 16 : 0) | (s.suppressExt2 ? 32 : 0) | (s.suppressDim1 ? 64 : 0) |
        (s.suppressDim2 ? 128 : 0));
}
inline void unpackDimRules(DimStyle& s, std::uint8_t b, int justify) {
    s.textAlignOutside = (b & 1) ? DimTextAlign::Horizontal : DimTextAlign::WithDimLine;
    s.textInsideForced = (b & 2) != 0;
    s.suppressArrowsOutside = (b & 4) != 0;
    s.forceLineInside = (b & 8) != 0;
    s.suppressExt1 = (b & 16) != 0;
    s.suppressExt2 = (b & 32) != 0;
    s.suppressDim1 = (b & 64) != 0;
    s.suppressDim2 = (b & 128) != 0;
    s.textJustify = justify >= 0 && justify <= 2 ? justify : 0;
}

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
        Lookup,      // value = index into `states` (the rows); each row sets other parameters
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
    // Lookup: the parameters the table drives (indexes into DynamicDef::params) and, for each row
    // (`states`), the value it gives each of them: lookupValues[row][k] for lookupParams[k].
    std::vector<int> lookupParams{};
    std::vector<std::vector<double>> lookupValues{};
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
