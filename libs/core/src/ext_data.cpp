#include "tbcad/core/entities.h"

namespace tbcad {

DataKind dataKindOf(int code) {
    const auto in = [&](int lo, int hi) { return code >= lo && code <= hi; };
    if (in(10, 59) || in(110, 149) || in(210, 239) || in(460, 469) || in(1010, 1059))
        return DataKind::Real;
    if (in(60, 79) || in(90, 99) || in(160, 179) || in(270, 299) || in(370, 389) || in(400, 409) ||
        in(420, 429) || in(440, 459) || in(1060, 1071))
        return DataKind::Integer;
    return DataKind::Text;
}

}  // namespace tbcad
