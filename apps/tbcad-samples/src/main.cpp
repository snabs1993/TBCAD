// tbcad-samples <folder>: writes the demo drawings (thai-house.tbcad, .dxf and the A3 sheet as
// PDF).
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "tbcad/blocks/display.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/export/plot.h"
#include "tbcad/hatch/hatch.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"
#include "tbcad/samples/house.h"
#include "tbcad/text/text.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: tbcad-samples <output folder>\n");
        return 2;
    }
    // The plot draws what the display generators produce, so they have to be registered.
    tbcad::hatch::registerHatchDisplay();
    tbcad::blocks::registerInsertDisplay();
    tbcad::dim::registerDisplay();
    tbcad::text::registerDisplay();
    namespace fs = std::filesystem;
    const fs::path dir = argv[1];
    std::error_code ec;
    fs::create_directories(dir, ec);

    auto doc = tbcad::samples::buildThaiHouse();
    if (auto r = tbcad::io::writeTbcad(*doc, dir / "thai-house.tbcad"); !r) {
        std::fprintf(stderr, "thai-house.tbcad: %s\n", r.error().message.c_str());
        return 1;
    }
    tbcad::dxf::Report report;
    auto dxf = tbcad::dxf::exportDxf(*doc, {}, report);
    if (!dxf) {
        std::fprintf(stderr, "thai-house.dxf: %s\n", dxf.error().message.c_str());
        return 1;
    }
    std::ofstream out(dir / "thai-house.dxf", std::ios::binary);
    out.write(dxf.value().bytes.data(), static_cast<std::streamsize>(dxf.value().bytes.size()));
    if (!out) {
        std::fprintf(stderr, "thai-house.dxf: cannot write\n");
        return 1;
    }
    // The sheet as a PDF, so the sample can be looked at without opening the program.
    auto sheet = tbcad::layout::buildLayoutSheet(*doc, "A3 1-100");
    if (!sheet) {
        std::fprintf(stderr, "layout: %s\n", sheet.error().message.c_str());
        return 1;
    }
    auto pdf = tbcad::exportlib::pdfBytes(sheet.value(), true);
    if (!pdf) {
        std::fprintf(stderr, "thai-house.pdf: %s\n", pdf.error().message.c_str());
        return 1;
    }
    std::ofstream pdfOut(dir / "thai-house-A3.pdf", std::ios::binary);
    pdfOut.write(pdf.value().data(), static_cast<std::streamsize>(pdf.value().size()));
    std::printf("wrote %s and %s\n", (dir / "thai-house.tbcad").string().c_str(),
                (dir / "thai-house.dxf").string().c_str());
    return 0;
}
