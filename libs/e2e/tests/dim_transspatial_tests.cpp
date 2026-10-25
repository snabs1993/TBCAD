// Dimensions made on a layout sheet from model objects seen through a viewport (trans-spatial):
// they follow the object, the viewport's scale and view, show model lengths, and survive files.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dim_assoc.h"
#include "tbcad/core/dump.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"

using namespace tbcad;
using cmd::ScriptItem;

namespace {

// Model line (1000,500)-(3000,500) seen at 1:50 in a viewport whose middle shows (1000,500):
// the line runs from sheet (120,70) to (160,70).
struct Sheet {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    EntityId line{}, dim{};
    int vp = 0;
    BlockId sheet{};
    Sheet() : host(doc, (registerAllCommands(reg), reg)) {
        auto tx = doc.begin("setup");
        Entity e;
        e.data = LineData{{1000, 500}, {3000, 500}};
        line = tx.add(std::move(e)).value();
        REQUIRE(layout::createLayout(tx, "A", "A3").ok());
        vp = layout::addViewport(tx, "A", BBox2{{20, 20}, {220, 120}}, {1000, 500}, 50).value();
        tx.commit();
        sheet = layout::findLayout(doc, "A")->content;
        host.showLayout("A", nullptr);
        host.setDrawingOwner(sheet);
        REQUIRE(host.run("DIMLINEAR", {"120,70", "160,70", "140,50"}).finished);
        doc.forEachEntity([&](const Entity& en) {
            if (entityAs<DimensionData>(en))
                dim = en.common.id;
        });
        REQUIRE(dim);
    }
    const DimensionData& d() const { return *entityAs<DimensionData>(*doc.find(dim)); }
    const LineData& l() const { return *entityAs<LineData>(*doc.find(line)); }
    double text() const { return std::stod(dim::dimensionText(dim::styleOf(doc, d().style), d())); }
    Vec2 sheetOf(Vec2 model) const {
        const auto lay = *layout::findLayout(doc, "A");
        const layout::Viewport& v = *lay.findViewport(vp);
        return (v.paper.min + v.paper.max) * 0.5 + (model - v.center) / v.scaleN;
    }
};

}  // namespace

TEST_CASE("a dimension on the sheet links to the model line seen through the viewport") {
    Sheet s;
    CHECK(s.d().assoc[0].kind == DimAssoc::Kind::Vertex);
    CHECK(s.d().assoc[0].target == s.line);
    CHECK(s.d().assoc[0].viewport == s.vp);
    CHECK(s.d().assoc[1].index == 1);
    CHECK(s.d().modelScale == doctest::Approx(50));
    // The sheet length is 40 mm, the model length 2000.
    CHECK(s.d().measurement == doctest::Approx(40));
    CHECK(s.text() == doctest::Approx(2000));
}

TEST_CASE("the dimension follows the model line when it is edited") {
    Sheet s;
    s.host.showLayout("", nullptr);
    s.host.setDrawingOwner({});
    s.host.setSelection({s.line});
    REQUIRE(s.host.run("MOVE", {"0,0", "500,0"}).finished);
    CHECK(distance(s.d().defPts[0], s.sheetOf(s.l().a)) < 1e-9);
    CHECK(distance(s.d().defPts[1], s.sheetOf(s.l().b)) < 1e-9);
    s.host.setSelection({s.line});
    REQUIRE(s.host.run("SCALE", {"1500,500", "2"}).finished);
    CHECK(distance(s.d().defPts[1], s.sheetOf(s.l().b)) < 1e-9);
    CHECK(s.text() == doctest::Approx(4000));
    while (s.host.undo()) {
    }
    CHECK(s.l().a.x == doctest::Approx(1000));
}

TEST_CASE("changing the viewport scale or view moves the dimension's points and its text") {
    Sheet s;
    {
        auto tx = s.doc.begin("vpscale");
        REQUIRE(layout::setViewportScale(tx, "A", s.vp, 100).ok());
        tx.commit();
    }
    CHECK(s.d().modelScale == doctest::Approx(100));
    CHECK(distance(s.d().defPts[0], s.sheetOf(s.l().a)) < 1e-9);
    CHECK(distance(s.d().defPts[1], s.sheetOf(s.l().b)) < 1e-9);
    CHECK(s.d().measurement == doctest::Approx(20));
    CHECK(s.text() == doctest::Approx(2000));  // the model line did not change
    {
        auto tx = s.doc.begin("vpcenter");
        REQUIRE(layout::setViewportCenter(tx, "A", s.vp, {1500, 500}).ok());
        tx.commit();
    }
    CHECK(distance(s.d().defPts[0], s.sheetOf(s.l().a)) < 1e-9);
    CHECK(s.d().assoc[0].target == s.line);
}

TEST_CASE("a trans-spatial dimension survives .tbcad and DXF") {
    Sheet s;
    const auto file = std::filesystem::temp_directory_path() / "tbcad_transspatial.tbcad";
    REQUIRE(io::writeTbcad(s.doc, file).ok());
    io::IoReport rep;
    auto back = io::readTbcad(file, rep);
    REQUIRE(back.ok());
    CHECK(canonicalDump(*back.value()) == canonicalDump(s.doc));
    std::error_code ec;
    std::filesystem::remove(file, ec);

    dxf::Report er;
    auto out = dxf::exportDxf(s.doc, dxf::ExportOptions{}, er);
    REQUIRE(out.ok());
    const std::string& text = out.value().bytes;
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
        bytes[i] = static_cast<std::byte>(text[i]);
    dxf::Report ir;
    auto in = dxf::importDxf(bytes, dxf::ImportOptions{}, ir);
    REQUIRE(in.ok());
    bool found = false;
    in.value().document->forEachEntity([&](const Entity& e) {
        if (const auto* dm = entityAs<DimensionData>(e)) {
            found = true;
            CHECK(dm->assoc[0].viewport != 0);
            CHECK(dm->modelScale == doctest::Approx(50));
        }
    });
    CHECK(found);
}
