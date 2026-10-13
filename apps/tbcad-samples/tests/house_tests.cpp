#include <doctest/doctest.h>

#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"
#include "tbcad/samples/house.h"

using namespace tbcad;

TEST_CASE("the Thai house sample has levels, sub-layers, annotative dimensions and a layout") {
    auto doc = samples::buildThaiHouse();
    CHECK(doc->levels().size() == 3);
    int dims = 0, hatches = 0, thaiText = 0, inserts = 0;
    int annotative = 0;
    doc->forEachEntity([&](const Entity& e) {
        switch (kind(e)) {
            case EntityKind::Dimension:
                ++dims;
                break;
            case EntityKind::Hatch:
                ++hatches;
                break;
            case EntityKind::Insert:
                ++inserts;
                break;
            case EntityKind::Text:
                if (std::get<Indirect<TextData>>(e.data)->utf8.find("ห้อง") != std::string::npos)
                    ++thaiText;
                break;
            default:
                break;
        }
        annotative += e.common.annotative;
    });
    CHECK(dims >= 12);
    CHECK(hatches >= 30);
    CHECK(inserts >= 20);
    CHECK(thaiText >= 6);
    CHECK(annotative >= dims);
    CHECK(doc->layerByName("A-WALL") != nullptr);
    CHECK(doc->layerByName(doc->layerByName("A-WALL")->id, "exterior") != nullptr);
    auto lay = layout::findLayout(*doc, "A3 1-100");
    REQUIRE(lay.has_value());
    CHECK(lay->viewports.size() == 2);
    CHECK(lay->viewports[0].level != lay->viewports[1].level);
    CHECK(doc->header().custom.at(kAnnoScaleKey) == "100");
}

TEST_CASE("the Thai house sample is deterministic and survives .tbcad and DXF export") {
    auto a = samples::buildThaiHouse();
    auto b = samples::buildThaiHouse();
    auto bytesA = io::writeTbcadToMemory(*a);
    auto bytesB = io::writeTbcadToMemory(*b);
    REQUIRE(bytesA.ok());
    REQUIRE(bytesB.ok());
    CHECK(bytesA.value() == bytesB.value());

    io::IoReport report;
    auto back = io::readTbcad(bytesA.value(), report);
    REQUIRE(back.ok());
    CHECK(back.value()->modelSpaceCount() == a->modelSpaceCount());

    dxf::Report dr;
    auto out = dxf::exportDxf(*a, {}, dr);
    REQUIRE(out.ok());
    CHECK(out.value().summary.skipped == 0);
    CHECK(out.value().bytes.find("\\U+0E") != std::string::npos);  // Thai text as \U+nnnn

    // The DXF brings the levels back.
    std::vector<std::byte> raw(out.value().bytes.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<std::byte>(out.value().bytes[i]);
    dxf::Report rr;
    auto imported = dxf::importDxf(raw, {}, rr);
    REQUIRE(imported.ok());
    CHECK(imported.value().document->levels().size() == 3);
}
