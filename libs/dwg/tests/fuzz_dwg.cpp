// libFuzzer target: the DWG reader must never crash, hang or read out of bounds, whatever the
// bytes. Build: cmake -DTBCAD_DWG_FUZZ=ON (clang), run tbcad_dwg_fuzz_dwg <corpus dir>
// -max_total_time=30
#include <cstddef>
#include <cstdint>
#include <span>

#include "tbcad/dwg/dwg.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace tbcad;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    dxf::ImportOptions options;
    dxf::Report report;
    Result<dxf::ImportResult> r = dwg::importDwg(bytes, options, report);
    if (r.ok())
        (void)r.value().summary.toJson(report);
    return 0;
}
