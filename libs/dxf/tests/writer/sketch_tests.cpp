// Constraints and parameters saved in a DXF file and read back (docs/constraints.md).
#include <algorithm>

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

TEST_CASE("associative arrays are fused back into one insert after a DXF round trip") {
    Document doc;
    ArrayInfo rect;
    rect.rows = 2;
    rect.columns = 3;
    rect.rowSpacing = 4;
    rect.columnSpacing = 6;
    ArrayInfo polar;
    polar.kind = ArrayInfo::Kind::Polar;
    polar.center = {0, 0};
    polar.count = 5;
    polar.fillAngle = 3;
    ArrayInfo path;
    path.kind = ArrayInfo::Kind::Path;
    path.path = {PolyVertex{{0, 0}, 0, 0, 0}, PolyVertex{{20, 0}, 0, 0, 0},
                 PolyVertex{{20, 10}, 0, 0, 0}};
    path.count = 4;
    path.pathSpacing = 0;
    {
        auto tx = doc.begin("arrays");
        BlockDef def;
        def.name = "*A1";
        def.anonymous = true;
        BlockId b = tx.addBlock(def).value();
        Entity c;
        c.data = CircleData{{10, 0}, 1};
        REQUIRE(tx.add(std::move(c), b).ok());
        for (const ArrayInfo* a : {&rect, &polar, &path}) {
            InsertData ins;
            ins.block = b;
            ins.position = {a == &rect ? 0.0 : a == &polar ? 100.0 : 200.0, 0};
            ins.array = *a;
            Entity e;
            e.data = Indirect<InsertData>(ins);
            REQUIRE(tx.add(std::move(e)).ok());
        }
        tx.commit();
    }
    Report r1;
    auto out = exportDxf(doc, ExportOptions{}, r1);
    REQUIRE(out.ok());
    Report r2;
    auto in = importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, r2);
    REQUIRE(in.ok());
    std::vector<InsertData> got;
    in.value().document->forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            got.push_back(*i);
    });
    REQUIRE(got.size() == 3);
    int arrays = 0;
    for (const InsertData& i : got) {
        REQUIRE(i.array);
        ++arrays;
        if (i.array->kind == ArrayInfo::Kind::Rectangular) {
            CHECK(i.array->rows == 2);
            CHECK(i.array->columns == 3);
            CHECK(i.array->columnSpacing == doctest::Approx(6));
            CHECK(i.position.x == doctest::Approx(0));
        } else if (i.array->kind == ArrayInfo::Kind::Polar) {
            CHECK(i.array->count == 5);
            CHECK(i.array->fillAngle == doctest::Approx(3));
            CHECK(i.position.x == doctest::Approx(100));
        } else {
            CHECK(i.array->path.size() == 3);
            CHECK(i.array->count == 4);
            CHECK(i.position.x == doctest::Approx(200));
        }
    }
    CHECK(arrays == 3);
}

TEST_CASE("removed and replaced array items survive a DXF round trip") {
    Document doc;
    BlockId swapBlock;
    {
        auto tx = doc.begin("arrays");
        BlockId b = tx.addBlock(BlockDef{.name = "*A1", .anonymous = true}).value();
        Entity c;
        c.data = CircleData{{0, 0}, 1};
        REQUIRE(tx.add(std::move(c), b).ok());
        swapBlock = tx.addBlock(BlockDef{.name = "*A2", .anonymous = true}).value();
        Entity l;
        l.data = LineData{{0, -1}, {0, 1}};
        REQUIRE(tx.add(std::move(l), swapBlock).ok());
        ArrayInfo rect;
        rect.rows = 2;
        rect.columns = 3;
        rect.rowSpacing = 4;
        rect.columnSpacing = 6;
        rect.removed = {0, 4};  // the first item too: the note moves to item 1
        rect.swaps = {ArrayItemSwap{2, swapBlock, {0, 0}}};
        InsertData ins;
        ins.block = b;
        ins.position = {50, 0};
        ins.array = rect;
        Entity e;
        e.data = Indirect<InsertData>(ins);
        REQUIRE(tx.add(std::move(e)).ok());
        tx.commit();
    }
    Report r1;
    auto out = exportDxf(doc, ExportOptions{}, r1);
    REQUIRE(out.ok());
    std::string text = out.value().bytes;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    std::size_t inserts = 0;
    for (std::size_t at = text.find("\nINSERT\n"); at != std::string::npos;
         at = text.find("\nINSERT\n", at + 1))
        ++inserts;
    CHECK(inserts == 4);  // six items, two removed
    Report r2;
    auto in = importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, r2);
    REQUIRE(in.ok());
    const Document& back = *in.value().document;
    std::vector<InsertData> got;
    back.forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            got.push_back(*i);
    });
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].array);
    const ArrayInfo& a = *got[0].array;
    CHECK(got[0].position.x == doctest::Approx(50));
    CHECK(a.rows == 2);
    CHECK(a.columns == 3);
    CHECK(a.removed == std::vector<int>{0, 4});
    REQUIRE(a.swaps.size() == 1);
    CHECK(a.swaps[0].item == 2);
    const BlockDef* sb = back.block(a.swaps[0].block);
    REQUIRE(sb);
    REQUIRE(sb->entities.size() == 1);
    CHECK(entityAs<LineData>(*back.find(sb->entities[0])));
}

}  // namespace tbcad::dxf
