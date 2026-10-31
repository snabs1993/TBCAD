// Per-sheet layer overrides: one layout shows, hides or restyles layers apart from the drawing and
// from the other layouts.
#include <doctest/doctest.h>

#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"

using namespace tbcad;
using namespace tbcad::layout;

namespace {

Entity line(Vec2 a, Vec2 b, LayerId layer) {
    Entity e;
    e.data = LineData{a, b};
    e.common.layer = layer;
    return e;
}

LayerId addLayer(Document& doc, const char* name, LayerId parent = {},
                 Color color = Color::index(7)) {
    auto tx = doc.begin("layer");
    Layer l;
    l.name = name;
    l.parent = parent;
    l.color = color;
    auto id = tx.addLayer(l);
    REQUIRE(id.ok());
    tx.commit();
    return id.value();
}

template <class F>
void run(Document& doc, const char* label, F f) {
    auto tx = doc.begin(label);
    REQUIRE(f(tx).ok());
    tx.commit();
}

// A drawing with Walls (red, sub-layer Inner) and Grid (yellow), one line each, and two layouts
// "A" and "B" with one viewport each over the lines.
struct Sheets {
    Document doc;
    LayerId walls, inner, grid;
    Sheets() {
        walls = addLayer(doc, "Walls", {}, Color::index(1));
        inner = addLayer(doc, "Inner", walls, Color::index(1));
        grid = addLayer(doc, "Grid", {}, Color::index(2));
        auto tx = doc.begin("model");
        REQUIRE(tx.add(line({0, 0}, {100, 0}, walls)).ok());
        REQUIRE(tx.add(line({0, 10}, {100, 10}, inner)).ok());
        REQUIRE(tx.add(line({0, 20}, {100, 20}, grid)).ok());
        tx.commit();
        for (const char* name : {"A", "B"})
            run(doc, "layout", [&](Transaction& t) {
                if (auto r = createLayout(t, name); !r)
                    return r;
                BBox2 box;
                box.expand({10, 10});
                box.expand({200, 150});
                return addViewport(t, name, box, {50, 10}, 1).ok() ? Result<void>() : Error{};
            });
    }
    void set(const char* sheet, const LayerOverride& o) {
        run(doc, "override", [&](Transaction& t) { return setSheetLayerOverride(t, sheet, o); });
    }
    // The colours of the strokes drawn inside the viewports of a sheet.
    std::vector<Rgba> strokes(const char* sheet) const {
        auto built = buildLayoutSheet(doc, sheet, {}, {false});
        REQUIRE(built.ok());
        std::vector<Rgba> out;
        bool inside = false;
        for (const auto& p : built.value().prims) {
            if (const auto* c = std::get_if<exportlib::PlotClip>(&p))
                inside = c->begin;
            else if (const auto* path = std::get_if<exportlib::PlotPath>(&p); path && inside)
                out.push_back(path->style.color);
        }
        return out;
    }
};

bool has(const std::vector<Rgba>& v, Rgba c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

}  // namespace

TEST_CASE("a layer hidden on one sheet still shows in the drawing and on the other sheets") {
    Sheets s;
    LayerOverride o;
    o.layer = s.grid;
    o.on = false;
    s.set("A", o);
    CHECK(s.strokes("A").size() == 2);
    CHECK(s.strokes("B").size() == 3);
    CHECK(s.doc.effective(s.grid).visible());

    // Hiding the parent on the sheet hides its sub-layer there too.
    o.layer = s.walls;
    s.set("A", o);
    CHECK(s.strokes("A").empty());
    CHECK(s.strokes("B").size() == 3);
}

TEST_CASE("a sheet can show a layer that is off in the drawing") {
    Sheets s;
    run(s.doc, "off",
        [&](Transaction& t) { return t.modifyLayer(s.grid, [](Layer& l) { l.on = false; }); });
    CHECK(s.strokes("A").size() == 2);
    LayerOverride o;
    o.layer = s.grid;
    o.on = true;
    s.set("A", o);
    CHECK(s.strokes("A").size() == 3);
    CHECK(s.strokes("B").size() == 2);
}

TEST_CASE("a sheet's colour and lineweight change the plotted lines on that sheet only") {
    Sheets s;
    const Rgba red = paletteColor(1), blue = paletteColor(5);
    CHECK(has(s.strokes("A"), red));
    LayerOverride o;
    o.layer = s.walls;
    o.color = Color::index(5);
    o.lineweight = LineWeight::hundredths(70);
    s.set("A", o);
    const auto a = s.strokes("A");
    CHECK(std::count(a.begin(), a.end(), blue) == 1);  // Walls; Inner keeps its own colour
    CHECK(has(a, red));
    CHECK_FALSE(has(s.strokes("B"), blue));
    auto built = buildLayoutSheet(s.doc, "A", {}, {false}).value();
    bool thick = false;
    for (const auto& p : built.prims)
        if (const auto* path = std::get_if<exportlib::PlotPath>(&p))
            thick = thick || path->style.widthMm == doctest::Approx(0.7);
    CHECK(thick);
}

TEST_CASE("a viewport's own override wins over the sheet's") {
    Sheets s;
    LayerOverride o;
    o.layer = s.grid;
    o.color = Color::index(3);
    s.set("A", o);
    // As read from another program's file: a P line after the viewport.
    run(s.doc, "vp", [&](Transaction& t) {
        return t.modifyHeader([&](Header& h) {
            std::string& text = h.custom[kHeaderKey];
            const std::size_t o2 = text.find("\nO\t");
            text.insert(o2 + 1, "P\t" + std::to_string(s.grid.value) + "\t\ti5\t\t\t\t\n");
        });
    });
    const Layout a = findLayout(s.doc, "A").value();
    REQUIRE(a.viewports.size() == 1);
    REQUIRE(a.viewports[0].overrides.size() == 1);
    CHECK(sheetLayer(s.doc, a, &a.viewports[0], s.grid).color == Color::index(5));
    CHECK(sheetLayer(s.doc, a, nullptr, s.grid).color == Color::index(3));
    CHECK(has(s.strokes("A"), paletteColor(5)));
}

TEST_CASE("sheet overrides are undone, saved, copied with the layout and cleared") {
    Sheets s;
    LayerOverride o;
    o.layer = s.grid;
    o.on = false;
    o.color = Color::rgb(10, 20, 30);
    o.linetype = kLinetypeContinuous;
    o.transparency = 40;
    o.plot = false;
    auto tx = s.doc.begin("override");
    REQUIRE(setSheetLayerOverride(tx, "A", o).ok());
    ChangeSet cs = tx.commit();
    REQUIRE(findLayout(s.doc, "A")->layerOverrides.size() == 1);
    CHECK(findLayout(s.doc, "A")->layerOverrides[0] == o);
    s.doc.apply(cs, Direction::Undo);
    CHECK(findLayout(s.doc, "A")->layerOverrides.empty());
    s.doc.apply(cs, Direction::Redo);
    CHECK(findLayout(s.doc, "A")->layerOverrides.size() == 1);

    auto bytes = io::writeTbcadToMemory(s.doc);
    REQUIRE(bytes.ok());
    io::IoReport report;
    auto loaded = io::readTbcad(bytes.value(), report);
    REQUIRE(loaded.ok());
    CHECK(findLayout(*loaded.value(), "A")->layerOverrides.at(0) == o);

    run(s.doc, "copy", [&](Transaction& t) { return copyLayout(t, "A", "A copy"); });
    CHECK(findLayout(s.doc, "A copy")->layerOverrides.size() == 1);

    run(s.doc, "clear", [&](Transaction& t) { return clearSheetLayerOverrides(t, "A"); });
    CHECK(findLayout(s.doc, "A")->layerOverrides.empty());
    CHECK(findLayout(s.doc, "A copy")->layerOverrides.size() == 1);

    // An empty override removes the entry.
    run(s.doc, "empty",
        [&](Transaction& t) { return setSheetLayerOverride(t, "A copy", LayerOverride{s.grid}); });
    CHECK(findLayout(s.doc, "A copy")->layerOverrides.empty());
}

TEST_CASE("sheet and viewport overrides go through a DXF file and back") {
    Sheets s;
    LayerOverride o;
    o.layer = s.grid;
    o.on = false;
    s.set("A", o);
    LayerOverride w;
    w.layer = s.walls;
    w.color = Color::rgb(1, 2, 3);
    w.lineweight = LineWeight::hundredths(50);
    w.linetype = kLinetypeContinuous;
    w.transparency = 30;
    w.plot = false;
    s.set("B", w);
    dxf::Report report;
    auto out = dxf::exportDxf(s.doc, {}, report);
    REQUIRE(out.ok());
    const std::string& text = out.value().bytes;
    auto in = dxf::importDxf(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()),
        {}, report);
    REQUIRE(in.ok());
    const Document& back = *in.value().document;
    const auto a = findLayout(back, "A");
    const auto b = findLayout(back, "B");
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a->layerOverrides.size() == 1);
    CHECK(back.layer(a->layerOverrides[0].layer)->name == "Grid");
    CHECK(a->layerOverrides[0].on == std::optional<bool>(false));
    REQUIRE(b->layerOverrides.size() == 1);
    const LayerOverride& bw = b->layerOverrides[0];
    CHECK(back.layer(bw.layer)->name == "Walls");
    CHECK(bw.color == std::optional<Color>(Color::rgb(1, 2, 3)));
    CHECK(bw.lineweight == std::optional<LineWeight>(LineWeight::hundredths(50)));
    REQUIRE(bw.linetype);
    CHECK(back.linetype(*bw.linetype) != nullptr);
    CHECK(bw.transparency == std::optional<int>(30));
    CHECK(bw.plot == std::optional<bool>(false));
}

TEST_CASE("AutoCAD's viewport layer overrides (layer XRECORDs) are read into the viewport") {
    const std::string dxfText =
        "  0\nSECTION\n  2\nTABLES\n  0\nTABLE\n  2\nLTYPE\n 70\n1\n"
        "  0\nLTYPE\n  5\n14\n  2\nContinuous\n 70\n0\n  3\nSolid\n 72\n65\n 73\n0\n 40\n0\n"
        "  0\nLTYPE\n  5\n15\n  2\nDASHED\n 70\n0\n  3\n__ __\n 72\n65\n 73\n2\n 40\n10\n"
        " 49\n6\n 74\n0\n 49\n-4\n 74\n0\n  0\nENDTAB\n"
        "  0\nTABLE\n  2\nLAYER\n 70\n2\n"
        "  0\nLAYER\n  5\n10\n  2\n0\n 70\n0\n 62\n7\n  6\nContinuous\n"
        "  0\nLAYER\n  5\n2A\n102\n{ACAD_XDICTIONARY\n360\n40\n102\n}\n  2\nWalls\n 70\n0\n"
        " 62\n1\n  6\nContinuous\n"
        "  0\nENDTAB\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nBLOCKS\n  0\nBLOCK\n  2\n*Paper_Space\n 70\n0\n 10\n0\n 20\n0\n 30\n0\n"
        "  0\nVIEWPORT\n  5\n3C\n  8\n0\n 67\n1\n 10\n210\n 20\n148.5\n 40\n400\n 41\n280\n"
        " 69\n2\n 68\n1\n 12\n1000\n 22\n2000\n 45\n28000\n"
        "  0\nVIEWPORT\n  5\n3D\n  8\n0\n 67\n1\n 10\n500\n 20\n148.5\n 40\n100\n 41\n100\n"
        " 69\n3\n 68\n1\n 12\n1000\n 22\n2000\n 45\n10000\n  0\nENDBLK\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nOBJECTS\n"
        "  0\nDICTIONARY\n  5\n40\n100\nAcDbDictionary\n280\n1\n"
        "  3\nADSK_XREC_LAYER_COLOR_OVR\n360\n41\n  3\nADSK_XREC_LAYER_LINEWT_OVR\n360\n42\n"
        "  3\nADSK_XREC_LAYER_LINETYPE_OVR\n360\n43\n"
        "  0\nXRECORD\n  5\n41\n100\nAcDbXrecord\n280\n1\n102\n{ADSK_LYR_COLOR_OVERRIDE\n"
        "335\n3C\n420\n-1023410171\n102\n}\n"
        "102\n{ADSK_LYR_COLOR_OVERRIDE\n335\n3D\n420\n-1040187392\n102\n}\n"
        "  0\nXRECORD\n  5\n42\n100\nAcDbXrecord\n280\n1\n102\n{ADSK_LYR_LINEWT_OVERRIDE\n"
        "335\n3C\n 91\n50\n102\n}\n"
        "  0\nXRECORD\n  5\n43\n100\nAcDbXrecord\n280\n1\n102\n{ADSK_LYR_LINETYPE_OVERRIDE\n"
        "335\n3D\n343\n15\n102\n}\n"
        "  0\nLAYOUT\n  1\nSheet A1\n 71\n1\n 44\n841\n 45\n594\n"
        "  0\nENDSEC\n  0\nEOF\n";
    // 0xC3000005 (palette 5) = -1023410171; 0xC2000000 (RGB 0,0,0) = -1040187392.
    dxf::Report report;
    auto r = dxf::importDxf(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(dxfText.data()), dxfText.size()),
                            {}, report);
    REQUIRE(r.ok());
    const Document& doc = *r.value().document;
    const std::vector<Layout> all = layouts(doc);
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].viewports.size() == 2);
    const Viewport& v1 = all[0].viewports[0];
    const Viewport& v2 = all[0].viewports[1];
    REQUIRE(v1.overrides.size() == 1);
    CHECK(doc.layer(v1.overrides[0].layer)->name == "Walls");
    CHECK(v1.overrides[0].color == std::optional<Color>(Color::index(5)));
    CHECK(v1.overrides[0].lineweight == std::optional<LineWeight>(LineWeight::hundredths(50)));
    REQUIRE(v2.overrides.size() == 1);
    CHECK(v2.overrides[0].color == std::optional<Color>(Color::rgb(0, 0, 0)));
    REQUIRE(v2.overrides[0].linetype);
    CHECK(doc.linetype(*v2.overrides[0].linetype)->name == "DASHED");
}
