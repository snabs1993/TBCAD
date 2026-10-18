// ACI colour index -> TBCAD palette index.
//
// TBCAD's index colours are AutoCAD's table (see core/color.cpp), so an ACI number is the same
// index and the same colour. Keeping the number matters: plot style tables (CTB) assign pens by
// index, so index 50 must stay 50 even though it looks like index 2.
#include "importer.h"

namespace tbcad::dxf::detail {

Color mapAci(int aci) {
    if (aci < 1 || aci > 255)
        return Color::index(7);
    return Color::index(static_cast<std::uint8_t>(aci));
}

}  // namespace tbcad::dxf::detail
