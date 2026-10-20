// libFuzzer target: a PDF attached as an underlay is read by our own parser (objects, xref,
// streams, filters, content operators, fonts). A damaged or hostile file must give an error or a
// drawing, never a crash, hang or out-of-bounds read.
// Build: cmake -DTBCAD_FORMATS_FUZZ=ON (clang), run tbcad_underlay_fuzz_pdf <corpus dir>
#include <cstddef>
#include <cstdint>
#include <span>

#include "tbcad/underlay/pdf.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    tbcad::underlay::PdfOptions options;
    tbcad::underlay::PdfReport report;
    (void)tbcad::underlay::importPdf(std::as_bytes(std::span<const std::uint8_t>(data, size)),
                                     options, &report);
    return 0;
}
