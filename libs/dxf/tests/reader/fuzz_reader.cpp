// libFuzzer target: the DXF reader (and through it the lexer, encoding detection, the entity
// mapping and the document's validation) must never crash, hang or read out of bounds.
// Build: cmake -DTBCAD_DXF_FUZZ=ON (clang), run tbcad_dxf_fuzz_reader <corpus dir>
// -max_total_time=30
#include <cstddef>
#include <cstdint>
#include <span>

#include "tbcad/dxf/reader.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace tbcad::dxf;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    ImportOptions options;
    if (size > 0 && (data[0] & 1))
        options.splitLayers = "~";
    if (size > 1 && (data[1] & 1))
        options.dimensionGraphics = false;
    Report report;
    Result<ImportResult> r = importDxf(bytes, options, report);
    if (r.ok())
        (void)r.value().summary.toJson(report);
    return 0;
}
