// A solid hatch on a layer whose colour is a grey index prints grey through a plot style table
// that keeps object colours for the greys (the S2312 columns).
#include <doctest/doctest.h>

#include "tbcad/export/plot.h"
#include "tbcad/hatch/hatch.h"

using namespace tbcad;
using namespace tbcad::exportlib;

TEST_CASE(
    "a solid hatch on a layer of index colour 253 keeps its grey through a table that leaves it") {
    hatch::registerHatchDisplay();
    Document doc;
    LayerId col;
    {
        auto tx = doc.begin("layers");
        Layer l;
        l.name = "x_col_03$0$S-_COLNHATC_E-";
        l.color = Color::index(253);
        col = tx.addLayer(l).value();
        Entity h;
        HatchData hd;
        HatchLoop loop;
        HatchPolylineEdge pe;
        pe.v = {{{0, 0}, 0, 0, 0}, {{500, 0}, 0, 0, 0}, {{500, 500}, 0, 0, 0}, {{0, 500}, 0, 0, 0}};
        loop.edges.push_back(pe);
        hd.loops.push_back(loop);
        h.data = hd;
        h.common.layer = col;
        REQUIRE(tx.add(h).ok());
        tx.commit();
    }
    // Like the office table: every colour black except the greys, which keep the object colour.
    plotstyle::PlotStyleTable t;
    for (int i = 1; i <= 249; ++i)
        t.entries[static_cast<std::size_t>(i)].color = Rgba{0, 0, 0, 255};
    PlotSettings s;
    s.styleTable = std::make_shared<plotstyle::PlotStyleTable>(t);
    s.traceColors = true;
    auto sheet = buildSheet(doc, s);
    REQUIRE(sheet.ok());
    bool found = false;
    for (const auto& p : sheet.value().prims)
        if (const auto* f = std::get_if<PlotFill>(&p)) {
            CHECK(f->color == Rgba{130, 130, 130, 255});
            found = true;
        }
    CHECK(found);
    CHECK_FALSE(sheet.value().trace.empty());
}
