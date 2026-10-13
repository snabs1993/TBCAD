// libFuzzer target: the lexer, header scan, encoding detection and text decoding must never
// crash, hang or read out of bounds on arbitrary input.
// Build: cmake -DTBCAD_DXF_FUZZ=ON (clang), run tbcad_dxf_fuzz_lexer <corpus dir>
// -max_total_time=30
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "tbcad/dxf/encoding.h"
#include "tbcad/dxf/lexer.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace tbcad::dxf;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    Report report;
    auto lx = Lexer::open(bytes, report);
    if (lx) {
        std::string text;
        while (auto g = lx->next()) {
            if (g->type() == ValueType::String) {
                text += g->str();
            }
        }
        const HeaderHints hints = scanHeaderHints(bytes);
        const Encoding enc = detectEncoding(bytes, hints.acadVer, hints.dwgCodepage, report);
        const std::string utf8 = decodeText(text, enc);
        (void)stripMText(utf8);
        (void)report.toJson();
    }
    return 0;
}
