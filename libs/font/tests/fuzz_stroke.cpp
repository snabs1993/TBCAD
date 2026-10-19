// libFuzzer target: compiled AutoCAD shape fonts (.shx: shapes, unifont) and TBCAD's own .tbsf
// stroke fonts arrive from the user's font folders. Reading one must never crash, hang or read
// out of bounds, and every glyph it makes must be finite.
// Build: cmake -DTBCAD_FORMATS_FUZZ=ON (clang), run tbcad_font_fuzz_stroke <corpus dir>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "stroke_font.h"

using namespace tbcad::font;

namespace {

void walk(const StrokeFont& f) {
    for (char32_t cp = 0; cp < 0x10000; ++cp) {
        const StrokeFont::Glyph* g = f.glyph(cp);
        if (!g)
            continue;
        if (!std::isfinite(g->advance))
            std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    {
        StrokeFont f;
        if (f.loadShx(data, size, size > 0 && (data[size - 1] & 1)).empty())
            walk(f);
    }
    {
        StrokeFont f;
        if (f.parse(std::string_view(reinterpret_cast<const char*>(data), size)).empty())
            walk(f);
    }
    return 0;
}
