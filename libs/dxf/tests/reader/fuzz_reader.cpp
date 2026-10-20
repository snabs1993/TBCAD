// libFuzzer target: the DXF reader (and through it the lexer, encoding detection, the entity
// mapping, the document's validation, drawing and the writer) must never crash, hang or read out
// of bounds.
// Build: cmake -DTBCAD_DXF_FUZZ=ON (clang), run tbcad_dxf_fuzz_reader <corpus dir>
// -max_total_time=30
#include <cstddef>
#include <cstdint>
#include <span>

#include "tbcad/blocks/display.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/geom/entity.h"
#include "tbcad/hatch/hatch.h"
#include "tbcad/mline/mline.h"
#include "tbcad/table/table.h"
#include "tbcad/text/text.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace tbcad::dxf;
    static const bool registered = [] {
        tbcad::dim::registerDisplay();
        tbcad::text::registerDisplay();
        tbcad::mline::registerDisplay();
        tbcad::table::registerDisplay();
        tbcad::hatch::registerHatchDisplay();
        tbcad::blocks::registerInsertDisplay();
        return true;
    }();
    (void)registered;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
    ImportOptions options;
    if (size > 0 && (data[0] & 1))
        options.splitLayers = "~";
    if (size > 1 && (data[1] & 1))
        options.dimensionGraphics = false;
    Report report;
    tbcad::Result<ImportResult> r = importDxf(bytes, options, report);
    if (r.ok()) {
        (void)r.value().summary.toJson(report);
        // Whatever was read can be measured and drawn.
        const tbcad::Document& doc = *r.value().document;
        doc.forEachEntity([&](const tbcad::Entity& e) {
            (void)tbcad::geom::bounds(e, doc);
            tbcad::DisplayList list;
            tbcad::geom::buildDisplay(doc, e, tbcad::geom::DisplayContext{}, list);
        });
        // Whatever was read can be saved again (hostile numbers reach the writer this way).
        Report out;
        (void)exportDxf(*r.value().document, ExportOptions{}, out);
    }
    return 0;
}
