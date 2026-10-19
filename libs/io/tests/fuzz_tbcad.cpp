// libFuzzer target: reading a .tbcad file (chunks, compression, entity codec, document validation)
// must never crash, hang or read out of bounds, and a document it accepts must save again.
// Build: cmake -DTBCAD_IO_FUZZ=ON (clang), run tbcad_io_fuzz_tbcad <corpus dir> -max_total_time=30
#include <cstddef>
#include <cstdint>
#include <span>

#include "tbcad/io/tbcad.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    tbcad::io::IoReport report;
    auto doc = tbcad::io::readTbcad(std::span<const std::uint8_t>(data, size), report);
    if (doc.ok())
        (void)tbcad::io::writeTbcadToMemory(*doc.value());
    return 0;
}
