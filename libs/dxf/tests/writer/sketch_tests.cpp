// Constraints and parameters saved in a DXF file and read back (docs/constraints.md).
#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"

namespace tbcad::dxf {

TEST_CASE("constraint and parameter text survive a DXF round trip") {
    Document doc;
    const std::string cons = "1 Distance 3 0 4 0 100 |Width * 2\n2 Horizontal 3 0 0 0 0\n";
    const std::string params = "Width\t50\nHeight\tWidth * 0.5\n";
    {
        auto tx = doc.begin("sketch");
        Entity e;
        e.data = LineData{{0, 0}, {10, 0}};
        REQUIRE(tx.add(std::move(e)).ok());
        REQUIRE(tx.modifyHeader([&](Header& h) {
                      h.custom["tbcad.constraints"] = cons;
                      h.custom["tbcad.parameters"] = params;
                  }).ok());
        tx.commit();
    }
    Report r1;
    auto out = exportDxf(doc, ExportOptions{}, r1);
    REQUIRE(out.ok());
    Report r2;
    auto in = importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, r2);
    REQUIRE(in.ok());
    const auto& custom = in.value().document->header().custom;
    REQUIRE(custom.count("tbcad.constraints"));
    CHECK(custom.at("tbcad.constraints") == cons);
    REQUIRE(custom.count("tbcad.parameters"));
    CHECK(custom.at("tbcad.parameters") == params);
}

}  // namespace tbcad::dxf
