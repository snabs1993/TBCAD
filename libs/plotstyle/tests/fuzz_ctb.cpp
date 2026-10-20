// libFuzzer target: plot style tables arrive as AutoCAD .ctb/.stb files (zlib-packed text) and as
// our own text format. Reading either must never crash, hang or read out of bounds.
// Build: cmake -DTBCAD_FORMATS_FUZZ=ON (clang), run tbcad_plotstyle_fuzz_ctb <corpus dir>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "tbcad/plotstyle/plotstyle.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace tbcad::plotstyle;
    auto table = importCtb(std::span<const std::uint8_t>(data, size));
    (void)fromText(std::string(reinterpret_cast<const char*>(data), size));
    if (table.ok())
        (void)fromText(toText(table.value()));
    return 0;
}
