#pragma once

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
    Color color = Color::index(7);                        // never ByLayer / ByBlock
    LinetypeId linetype{};                                // filled with "Continuous" when null
    LineWeight lineweight = LineWeight::defaultWeight();  // Default or a value
    bool on = true;
    bool frozen = false;
    bool locked = false;
    bool plot = true;
};

struct Linetype {
    LinetypeId id{};
    std::string name{};
    std::string description{};
    std::vector<double> pattern{};  // + dash, - gap, 0 dot; in mm. Empty = continuous
};

struct TextStyle {
    TextStyleId id{};
    std::string name{};
    std::string fontFamily{};  // empty = application default
    std::string fontFile{};
    double widthFactor = 1;
    double oblique = 0;  // radians
};

enum class ArrowType { ClosedFilled, Open30, ArchTick, Dot, None };
enum class DimTextPlacement { AboveLine, Centered };
enum class DimTextAlign { WithDimLine, Horizontal };

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
};

struct BlockDef {
    BlockId id{};
    std::string name{};
    Vec2 base{};
    std::vector<EntityId> entities{};
    bool anonymous = false;
};

}  // namespace tbcad
