// libFuzzer target: a user's pattern file is parsed, and every pattern in it fills a square and an
// island. Parsing and filling must never crash, hang or run out of memory, whatever the spacing,
// dash lengths or angles.
// Build: cmake -DTBCAD_FORMATS_FUZZ=ON (clang), run tbcad_hatch_fuzz_patterns <corpus dir>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "tbcad/hatch/fill.h"
#include "tbcad/hatch/pattern.h"

using namespace tbcad;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto patterns =
        hatch::parsePatterns(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!patterns.ok())
        return 0;
    HatchData h;
    auto ring = [](double x0, double y0, double s) {
        HatchPolylineEdge e;
        e.v = {{{x0, y0}}, {{x0 + s, y0}}, {{x0 + s, y0 + s}}, {{x0, y0 + s}}};
        HatchLoop l;
        l.edges.push_back(e);
        return l;
    };
    h.loops = {ring(0, 0, 1000), ring(400, 400, 200)};
    for (const hatch::Pattern& p : patterns.value()) {
        for (double scale : {1.0, 0.001}) {
            h.patternScale = scale;
            (void)hatch::generateFill(h, p);
        }
    }
    return 0;
}
