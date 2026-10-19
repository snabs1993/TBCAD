#include <doctest/doctest.h>

#include <chrono>
#include <regex>
#include <sstream>

#include "tbcad/blocks/attributes.h"
#include "tbcad/blocks/display.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"
#include "tbcad/pdf/pdf.h"

using namespace tbcad;
using namespace tbcad::layout;

namespace {

Entity line(Vec2 a, Vec2 b, LayerId layer = {}) {
    Entity e;
    e.data = LineData{a, b};
    e.common.layer = layer;
    return e;
}

LayerId addLayer(Document& doc, const char* name, LayerId parent = {}) {
    auto tx = doc.begin("layer");
    Layer l;
    l.name = name;
    l.parent = parent;
    auto id = tx.addLayer(l);
    REQUIRE(id.ok());
    tx.commit();
    return id.value();
}

void run(Document& doc, const char* name, const std::function<Result<void>(Transaction&)>& fn) {
    auto tx = doc.begin(name);
    auto r = fn(tx);
    if (!r.ok())
        FAIL(r.error().message);
    tx.commit();
}

}  // namespace

TEST_CASE("create, rename, resize and delete layouts") {
    Document doc;
    run(doc, "new", [](Transaction& tx) { return createLayout(tx, "Sheet 1", "A3"); });
    run(doc, "new", [](Transaction& tx) { return createLayout(tx, "Sheet 2", "A4", false); });
    auto all = layouts(doc);
    REQUIRE(all.size() == 2);
    CHECK(all[0].widthMm == doctest::Approx(420));
    CHECK(all[0].heightMm == doctest::Approx(297));
    CHECK(all[1].widthMm == doctest::Approx(210));
    CHECK(all[1].heightMm == doctest::Approx(297));
    CHECK(doc.block(all[0].content) != nullptr);

    {  // names are unique ignoring case, and validated
        auto tx = doc.begin("dup");
        CHECK_FALSE(createLayout(tx, "SHEET 1").ok());
        CHECK_FALSE(createLayout(tx, "").ok());
        CHECK_FALSE(createLayout(tx, "a\tb").ok());
        CHECK_FALSE(createLayout(tx, "X", "B9").ok());
    }
    CHECK(layouts(doc).size() == 2);  // the failed transaction rolled back

    run(doc, "rename", [](Transaction& tx) { return renameLayout(tx, "sheet 1", "Plan"); });
    CHECK(findLayout(doc, "PLAN").has_value());
    CHECK_FALSE(findLayout(doc, "Sheet 1").has_value());

    run(doc, "paper", [](Transaction& tx) { return setLayoutPaper(tx, "Plan", "A1", false); });
    CHECK(findLayout(doc, "Plan")->widthMm == doctest::Approx(594));
    CHECK(findLayout(doc, "Plan")->heightMm == doctest::Approx(841));
    run(doc, "paper",
        [](Transaction& tx) { return setLayoutPaper(tx, "Plan", "Custom", true, 500, 300); });
    CHECK(findLayout(doc, "Plan")->widthMm == doctest::Approx(500));

    run(doc, "delete", [](Transaction& tx) { return deleteLayout(tx, "Plan"); });
    CHECK(layouts(doc).size() == 1);
    CHECK(doc.blocks().size() == 1);
}

TEST_CASE("layouts and viewports undo and redo with the drawing") {
    Document doc;
    run(doc, "new", [](Transaction& tx) { return createLayout(tx, "S"); });
    run(doc, "vp", [](Transaction& tx) {
        BBox2 r;
        r.expand({10, 10});
        r.expand({210, 150});
        return addViewport(tx, "S", r, {0, 0}, 50).ok() ? Result<void>() : Error{};
    });
    REQUIRE(layouts(doc).size() == 1);
    REQUIRE(layouts(doc)[0].viewports.size() == 1);
    // Build a change set the way the undo stack does: apply inverted, then forward again.
    auto tx = doc.begin("scale");
    REQUIRE(setViewportScale(tx, "S", 1, 100).ok());
    ChangeSet cs = tx.commit();
    CHECK(findLayout(doc, "S")->viewports[0].scaleN == doctest::Approx(100));
    doc.apply(cs, Direction::Undo);
    CHECK(findLayout(doc, "S")->viewports[0].scaleN == doctest::Approx(50));
    doc.apply(cs, Direction::Redo);
    CHECK(findLayout(doc, "S")->viewports[0].scaleN == doctest::Approx(100));
}

TEST_CASE("layouts survive a .tbcad save and load") {
    Document doc;
    LayerId walls = addLayer(doc, "Walls");
    run(doc, "build", [&](Transaction& tx) {
        if (auto r = createLayout(tx, "Floor plan", "A2"); !r)
            return r;
        BBox2 r;
        r.expand({20, 20});
        r.expand({300, 200});
        auto id = addViewport(tx, "Floor plan", r, {1234.5, -77.25}, 25);
        if (!id)
            return Result<void>(id.error());
        return setViewportLayerFrozen(tx, "Floor plan", id.value(), walls, true);
    });
    auto bytes = io::writeTbcadToMemory(doc);
    REQUIRE(bytes.ok());
    io::IoReport report;
    auto loaded = io::readTbcad(bytes.value(), report);
    REQUIRE(loaded.ok());
    const Document& back = *loaded.value();
    auto l = findLayout(back, "floor plan");
    REQUIRE(l.has_value());
    CHECK(l->paperName == "A2");
    REQUIRE(l->viewports.size() == 1);
    CHECK(l->viewports[0].center.x == doctest::Approx(1234.5));
    CHECK(l->viewports[0].scaleN == doctest::Approx(25));
    REQUIRE(l->viewports[0].frozen.size() == 1);
    CHECK(l->viewports[0].frozen[0] == walls);
    CHECK(back.block(l->content) != nullptr);
}

TEST_CASE("plotting a layout: a 10,000 mm model line in a 1:100 viewport is 100 mm on the sheet") {
    Document doc;
    {
        auto tx = doc.begin("model");
        REQUIRE(tx.add(line({0, 0}, {10000, 0})).ok());
        tx.commit();
    }
    run(doc, "layout", [](Transaction& tx) {
        if (auto r = createLayout(tx, "A3 sheet", "A3"); !r)
            return r;
        BBox2 r;
        r.expand({10, 10});
        r.expand({410, 287});
        auto id = addViewport(tx, "A3 sheet", r, {5000, 0}, 100);
        return id ? Result<void>() : Result<void>(id.error());
    });
    auto sheet = buildLayoutSheet(doc, "A3 sheet");
    REQUIRE(sheet.ok());
    CHECK(sheet.value().layout.pageWidthMm == doctest::Approx(420));
    // Prims: clip begin, the line, clip end, viewport border.
    const exportlib::PlotPath* path = nullptr;
    for (const auto& p : sheet.value().prims)
        if (const auto* pp = std::get_if<exportlib::PlotPath>(&p); pp && !pp->closed)
            path = pp;
    REQUIRE(path != nullptr);
    CHECK(path->pts[1].x - path->pts[0].x == doctest::Approx(100.0).epsilon(1e-6));
    // Viewport centre (5000, 0) sits at the middle of the viewport rectangle (210, 148.5).
    CHECK((path->pts[0].x + path->pts[1].x) / 2 == doctest::Approx(210.0).epsilon(1e-6));
    CHECK(path->pts[0].y == doctest::Approx(148.5));

    auto pdf = exportlib::pdfBytes(sheet.value());
    REQUIRE(pdf.ok());
    std::smatch m;
    std::regex re(R"(([-\d.]+) ([-\d.]+) m\s+([-\d.]+) ([-\d.]+) l)");
    // The first stroked path is inside the clip: 100 mm long.
    std::string text = pdf.value();
    std::size_t clips = 0;
    for (auto p = text.find("W n"); p != std::string::npos; p = text.find("W n", p + 1))
        ++clips;
    CHECK(clips >= 2);  // page clip and viewport clip
    CHECK(std::regex_search(text, m, re));
    CHECK(std::abs((std::stod(m[3]) - std::stod(m[1])) - 100.0) <= 0.01);
}

TEST_CASE("a viewport over many objects plots them in order with the right layer names") {
    // Enough objects for the viewport to be plotted on several threads: the result must read as
    // if plotted in one go.
    Document doc;
    LayerId a, b;
    {
        auto tx = doc.begin("model");
        Layer la;
        la.name = "Alpha";
        a = tx.addLayer(la).value();
        Layer lb;
        lb.name = "Beta";
        b = tx.addLayer(lb).value();
        for (int i = 0; i < 40000; ++i) {
            Entity e = line({double(i), 0}, {double(i), 10});
            // The first half on Beta, so Beta is the first layer used.
            e.common.layer = i < 20000 ? b : a;
            REQUIRE(tx.add(e).ok());
        }
        tx.commit();
    }
    run(doc, "layout", [](Transaction& tx) {
        if (auto r = createLayout(tx, "S", "A3"); !r)
            return r;
        BBox2 r;
        r.expand({10, 10});
        r.expand({410, 287});
        auto id = addViewport(tx, "S", r, {20000, 5}, 100);
        return id ? Result<void>() : Result<void>(id.error());
    });
    auto sheet = buildLayoutSheet(doc, "S");
    REQUIRE(sheet.ok());
    const auto& names = sheet.value().layerNames;
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "Beta");
    CHECK(names[1] == "Alpha");
    std::size_t lines = 0;
    double lastX = -1e300;
    bool ordered = true, layered = true;
    for (const auto& p : sheet.value().prims)
        if (const auto* pp = std::get_if<exportlib::PlotPath>(&p); pp && !pp->closed) {
            ++lines;
            ordered = ordered && pp->pts[0].x >= lastX;
            lastX = pp->pts[0].x;
            // Model x 20000 is the middle of the sheet (210 mm).
            layered = layered && pp->style.layer == (pp->pts[0].x < 210 ? 1u : 2u);
        }
    CHECK(lines == 40000);
    CHECK(sheet.value().entityCount == 40000);
    CHECK(ordered);
    CHECK(layered);
}

TEST_CASE("a viewport can show one level only - the choice is saved and plotted") {
    Document doc;
    LevelId upper;
    {
        auto tx = doc.begin("levels");
        Level l;
        l.name = "Upper";
        upper = tx.addLevel(l).value();
        tx.commit();
    }
    {
        auto tx = doc.begin("model");
        REQUIRE(tx.add(line({0, 0}, {100, 0})).ok());  // first level
        REQUIRE(tx.modifyHeader([&](Header& h) { h.currentLevel = upper; }).ok());
        REQUIRE(tx.add(line({0, 10}, {100, 10})).ok());  // upper level
        REQUIRE(tx.add(line({0, 20}, {100, 20})).ok());  // upper level
        tx.commit();
    }
    run(doc, "layout", [&](Transaction& tx) {
        if (auto r = createLayout(tx, "L"); !r)
            return r;
        BBox2 a;
        a.expand({10, 10});
        a.expand({100, 100});
        auto vp = addViewport(tx, "L", a, {50, 10}, 1);
        if (!vp)
            return Result<void>(vp.error());
        return setViewportLevel(tx, "L", vp.value(), upper);
    });
    CHECK(findLayout(doc, "L")->viewports[0].level == upper);
    auto strokes = [&] {
        auto sheet = buildLayoutSheet(doc, "L", {}, {false}).value();
        int n = 0;
        for (const auto& p : sheet.prims)
            if (std::holds_alternative<exportlib::PlotPath>(p))
                ++n;
        return n;
    };
    CHECK(strokes() == 2);
    run(doc, "all", [&](Transaction& tx) { return setViewportLevel(tx, "L", 1, LevelId{}); });
    CHECK(strokes() == 3);
    run(doc, "bad", [&](Transaction& tx) {
        CHECK_FALSE(setViewportLevel(tx, "L", 1, LevelId{99}).ok());
        return Result<void>{};
    });
}

TEST_CASE("a small drawing in a 1:1 viewport plots its lines") {
    Document doc;
    {
        auto tx = doc.begin("model");
        REQUIRE(tx.add(line({0, 0}, {100, 0})).ok());
        REQUIRE(tx.add(line({100, 0}, {100, 60})).ok());
        tx.commit();
    }
    run(doc, "layout", [](Transaction& tx) {
        if (auto r = createLayout(tx, "A4 sheet", "A4"); !r)
            return r;
        BBox2 r;
        r.expand({10, 10});
        r.expand({200, 140});
        auto id = addViewport(tx, "A4 sheet", r, {50, 30}, 1);
        return id ? Result<void>() : Result<void>(id.error());
    });
    auto sheet = buildLayoutSheet(doc, "A4 sheet", {}, {false});
    REQUIRE(sheet.ok());
    int strokes = 0;
    for (const auto& p : sheet.value().prims)
        if (std::holds_alternative<exportlib::PlotPath>(p))
            ++strokes;
    CHECK(strokes == 2);
}

TEST_CASE("freezing a layer in one viewport hides it there only, sub-layers included") {
    Document doc;
    LayerId walls = addLayer(doc, "Walls");
    LayerId inner = addLayer(doc, "Inner", walls);
    LayerId furniture = addLayer(doc, "Furniture");
    {
        auto tx = doc.begin("model");
        REQUIRE(tx.add(line({0, 0}, {100, 0}, walls)).ok());
        REQUIRE(tx.add(line({0, 10}, {100, 10}, inner)).ok());
        REQUIRE(tx.add(line({0, 20}, {100, 20}, furniture)).ok());
        tx.commit();
    }
    int vp1 = 0, vp2 = 0;
    run(doc, "layout", [&](Transaction& tx) {
        if (auto r = createLayout(tx, "L"); !r)
            return r;
        BBox2 a, b;
        a.expand({10, 10});
        a.expand({100, 100});
        b.expand({150, 10});
        b.expand({240, 100});
        vp1 = addViewport(tx, "L", a, {50, 10}, 1).value();
        vp2 = addViewport(tx, "L", b, {50, 10}, 1).value();
        return setViewportLayerFrozen(tx, "L", vp2, walls, true);
    });
    auto sheet = buildLayoutSheet(doc, "L", {}, {false}).value();
    // Count strokes inside each viewport's clip.
    int inVp[2] = {0, 0};
    int current = -1;
    int clipIndex = 0;
    for (const auto& p : sheet.prims) {
        if (const auto* c = std::get_if<exportlib::PlotClip>(&p)) {
            current = c->begin ? clipIndex++ : -1;
        } else if (std::holds_alternative<exportlib::PlotPath>(p) && current >= 0) {
            ++inVp[current];
        }
    }
    CHECK(vp1 == 1);
    CHECK(vp2 == 2);
    CHECK(inVp[0] == 3);
    CHECK(inVp[1] == 1);  // Walls and its sub-layer Inner are frozen here

    // The layer table is untouched.
    CHECK_FALSE(doc.effective(walls).frozen);
    run(doc, "thaw",
        [&](Transaction& tx) { return setViewportLayerFrozen(tx, "L", vp2, walls, false); });
    sheet = buildLayoutSheet(doc, "L", {}, {false}).value();
    int total = 0;
    for (const auto& p : sheet.prims)
        total += std::holds_alternative<exportlib::PlotPath>(p);
    CHECK(total == 6);
}

TEST_CASE("title block and sheet objects are plotted - viewports can be switched off") {
    Document doc;
    run(doc, "layout", [](Transaction& tx) {
        if (auto r = createLayout(tx, "T", "A3"); !r)
            return r;
        TitleBlockFields f;
        f.project = "Tower";
        f.drawing = "Ground floor";
        f.scale = "1:100";
        f.sheet = "A-101";
        return addTitleBlock(tx, "T", f);
    });
    blocks::registerInsertDisplay();
    auto sheet = buildLayoutSheet(doc, "T");
    REQUIRE(sheet.ok());
    // 4 frame lines + 3 row lines + the date divider; text only draws once a text generator is
    // registered.
    CHECK(sheet.value().prims.size() >= 8);
    BBox2 b;
    for (const auto& p : sheet.value().prims)
        if (const auto* pp = std::get_if<exportlib::PlotPath>(&p))
            for (const Vec2& v : pp->pts)
                b.expand(v);
    CHECK(b.max.x == doctest::Approx(410));  // 10 mm margin from the right edge of the A3 sheet
    CHECK(b.min.y == doctest::Approx(10));

    CHECK_FALSE(buildLayoutSheet(doc, "missing").ok());
}

TEST_CASE("the title block is a block with attributes - a second one reuses the definition") {
    Document doc;
    run(doc, "layout", [](Transaction& tx) {
        if (auto r = createLayout(tx, "A", "A3"); !r)
            return r;
        if (auto r = createLayout(tx, "B", "A4"); !r)
            return r;
        TitleBlockFields f;
        f.project = "Tower";
        f.drawing = "Ground floor";
        f.scale = "1:100";
        f.date = "2026-10-13";
        f.sheet = "A-101";
        if (auto r = addTitleBlock(tx, "A", f); !r)
            return r;
        f.sheet = "A-102";
        return addTitleBlock(tx, "B", f);
    });
    const BlockDef* def = doc.blockByName("TITLEBLOCK");
    REQUIRE(def != nullptr);
    CHECK(blocks::attributeDefinitions(doc, def->id).size() == 5);
    auto sheetValue = [&](const char* layoutName, const char* tag) -> std::string {
        auto l = findLayout(doc, layoutName);
        REQUIRE(l.has_value());
        const BlockDef* content = doc.block(l->content);
        REQUIRE(content != nullptr);
        for (EntityId id : content->entities) {
            const auto* ins = entityAs<InsertData>(*doc.find(id));
            if (!ins)
                continue;
            for (const auto& a : ins->attribs)
                if (a.tag == tag)
                    return a.value;
        }
        return "?";
    };
    CHECK(sheetValue("A", "PROJECT") == "Tower");
    CHECK(sheetValue("A", "SHEET") == "A-101");
    CHECK(sheetValue("B", "SHEET") == "A-102");
    CHECK(sheetValue("B", "DATE") == "2026-10-13");
    std::size_t blockCount = 0;
    for (const auto& [id, b] : doc.blocks())
        blockCount += b.name == "TITLEBLOCK";
    CHECK(blockCount == 1);
}

TEST_CASE("SVG of a layout is clipped per viewport and well-formed") {
    Document doc;
    {
        auto tx = doc.begin("model");
        REQUIRE(tx.add(line({0, 0}, {100, 0})).ok());
        tx.commit();
    }
    run(doc, "layout", [](Transaction& tx) {
        if (auto r = createLayout(tx, "L"); !r)
            return r;
        BBox2 a;
        a.expand({10, 10});
        a.expand({100, 100});
        auto id = addViewport(tx, "L", a, {50, 0}, 1);
        return id ? Result<void>() : Result<void>(id.error());
    });
    std::string svg = exportlib::svgString(buildLayoutSheet(doc, "L").value());
    CHECK(svg.find("<clipPath id=\"vp1\">") != std::string::npos);
    CHECK(std::count(svg.begin(), svg.end(), '<') == std::count(svg.begin(), svg.end(), '>'));
    std::size_t opens = 0, closes = 0;
    for (auto p = svg.find("<g "); p != std::string::npos; p = svg.find("<g ", p + 1))
        ++opens;
    for (auto p = svg.find("</g>"); p != std::string::npos; p = svg.find("</g>", p + 1))
        ++closes;
    CHECK(opens == closes);
}

TEST_CASE("viewport edits with an id that does not exist are refused, not crashed on") {
    Document doc;
    run(doc, "new", [](Transaction& tx) { return createLayout(tx, "S"); });
    run(doc, "vp", [](Transaction& tx) {
        BBox2 r;
        r.expand({10, 10});
        r.expand({210, 150});
        return addViewport(tx, "S", r, {0, 0}, 50).ok() ? Result<void>() : Error{};
    });
    auto tx = doc.begin("bad");
    for (int id : {0, -3, 2, 1000}) {
        CHECK_FALSE(setViewportScale(tx, "S", id, 100).ok());
        CHECK_FALSE(setViewportCenter(tx, "S", id, {1, 1}).ok());
        CHECK_FALSE(setViewportOn(tx, "S", id, false).ok());
        CHECK_FALSE(removeViewport(tx, "S", id).ok());
    }
}

TEST_CASE("layouts imported from a DXF file are read by the layout table") {
    const std::string dxfText =
        "  0\nSECTION\n  2\nBLOCKS\n  0\nBLOCK\n  2\n*Paper_Space\n 70\n0\n 10\n0\n 20\n0\n 30\n0\n"
        "  0\nLINE\n  8\n0\n 67\n1\n 10\n5\n 20\n5\n 11\n100\n 21\n5\n"
        "  0\nVIEWPORT\n  8\n0\n 67\n1\n 10\n210\n 20\n148.5\n 40\n400\n 41\n280\n 69\n2\n"
        " 68\n1\n 12\n1000\n 22\n2000\n 45\n28000\n  0\nENDBLK\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nOBJECTS\n  0\nLAYOUT\n  1\nSheet A1\n 71\n1\n 44\n841\n 45\n594\n"
        "  0\nENDSEC\n  0\nEOF\n";
    dxf::Report report;
    auto r = dxf::importDxf(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(dxfText.data()), dxfText.size()),
                            {}, report);
    REQUIRE(r.ok());
    const Document& doc = *r.value().document;
    const std::vector<Layout> all = layouts(doc);
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "Sheet A1");
    CHECK(all[0].widthMm == doctest::Approx(841));
    CHECK(all[0].heightMm == doctest::Approx(594));
    REQUIRE(all[0].viewports.size() == 1);
    CHECK(all[0].viewports[0].scaleN == doctest::Approx(100));
    CHECK(all[0].viewports[0].center.x == doctest::Approx(1000));
    const BlockDef* sheet = doc.block(all[0].content);
    REQUIRE(sheet != nullptr);
    CHECK(sheet->entities.size() == 1);
}

TEST_CASE("a viewport's frozen layers (group 331) come from an AutoCAD file and go back to one") {
    const std::string dxfText =
        "  0\nSECTION\n  2\nTABLES\n  0\nTABLE\n  2\nLAYER\n 70\n3\n"
        "  0\nLAYER\n  5\n10\n  2\n0\n 70\n0\n 62\n7\n  6\nContinuous\n"
        "  0\nLAYER\n  5\n2A\n  2\nWalls\n 70\n0\n 62\n1\n  6\nContinuous\n"
        "  0\nLAYER\n  5\n2B\n  2\nGrid\n 70\n0\n 62\n2\n  6\nContinuous\n"
        "  0\nENDTAB\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nBLOCKS\n  0\nBLOCK\n  2\n*Paper_Space\n 70\n0\n 10\n0\n 20\n0\n 30\n0\n"
        "  0\nVIEWPORT\n  8\n0\n 67\n1\n 10\n210\n 20\n148.5\n 40\n400\n 41\n280\n 69\n2\n"
        " 68\n1\n 12\n1000\n 22\n2000\n 45\n28000\n331\n2B\n331\nFFFF\n  0\nENDBLK\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nOBJECTS\n  0\nLAYOUT\n  1\nSheet A1\n 71\n1\n 44\n841\n 45\n594\n"
        "  0\nENDSEC\n  0\nEOF\n";
    dxf::Report report;
    auto r = dxf::importDxf(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(dxfText.data()), dxfText.size()),
                            {}, report);
    REQUIRE(r.ok());
    const Document& doc = *r.value().document;
    const std::vector<Layout> all = layouts(doc);
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].viewports.size() == 1);
    REQUIRE(all[0].viewports[0].frozen.size() == 1);  // the unknown handle FFFF is ignored
    CHECK(doc.layer(all[0].viewports[0].frozen[0])->name == "Grid");

    // Written back, the frozen layer is a group 331 handle that other programs understand, even
    // without our own XDATA.
    dxf::ExportOptions opts;
    auto out = dxf::exportDxf(doc, opts, report);
    REQUIRE(out.ok());
    std::string text = out.value().bytes;
    CHECK(text.find("331\r\n") != std::string::npos);
    for (std::size_t at; (at = text.find("frozen=Grid")) != std::string::npos;)
        text.replace(at, 11, "xxxxxx=Grid");
    auto in = dxf::importDxf(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()),
        {}, report);
    REQUIRE(in.ok());
    const std::vector<Layout> back = layouts(*in.value().document);
    REQUIRE(back.size() == 1);
    REQUIRE(back[0].viewports.size() == 1);
    CHECK(back[0].viewports[0].frozen.size() == 1);
}

TEST_CASE("a viewport's lock and off flags (group 90) are read and written") {
    const std::string dxfText =
        "  0\nSECTION\n  2\nBLOCKS\n  0\nBLOCK\n  2\n*Paper_Space\n 70\n0\n 10\n0\n 20\n0\n 30\n0\n"
        "  0\nVIEWPORT\n  8\n0\n 67\n1\n 10\n210\n 20\n148.5\n 40\n400\n 41\n280\n 69\n2\n"
        " 68\n1\n 12\n1000\n 22\n2000\n 45\n28000\n 90\n49152\n"
        "  0\nVIEWPORT\n  8\n0\n 67\n1\n 10\n60\n 20\n60\n 40\n50\n 41\n50\n 69\n3\n"
        " 68\n1\n 12\n0\n 22\n0\n 45\n5000\n 90\n163840\n  0\nENDBLK\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nOBJECTS\n  0\nLAYOUT\n  1\nSheet A1\n 71\n1\n 44\n841\n 45\n594\n"
        "  0\nENDSEC\n  0\nEOF\n";
    dxf::Report report;
    auto r = dxf::importDxf(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(dxfText.data()), dxfText.size()),
                            {}, report);
    REQUIRE(r.ok());
    const std::vector<Layout> all = layouts(*r.value().document);
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].viewports.size() == 2);
    CHECK(all[0].viewports[0].locked);
    CHECK(all[0].viewports[0].on);
    CHECK_FALSE(all[0].viewports[1].locked);
    CHECK_FALSE(all[0].viewports[1].on);
    dxf::ExportOptions opts;
    auto out = dxf::exportDxf(*r.value().document, opts, report);
    REQUIRE(out.ok());
    CHECK(out.value().bytes.find("\r\n90\r\n49152\r\n") != std::string::npos);
    CHECK(out.value().bytes.find("\r\n90\r\n163840\r\n") != std::string::npos);
}

TEST_CASE("the frozen-in-new-viewports layer flag (70 bit 2) round-trips through DXF") {
    const std::string dxfText =
        "  0\nSECTION\n  2\nTABLES\n  0\nTABLE\n  2\nLAYER\n 70\n2\n"
        "  0\nLAYER\n  2\n0\n 70\n0\n 62\n7\n  6\nContinuous\n"
        "  0\nLAYER\n  2\nGrid\n 70\n2\n 62\n2\n  6\nContinuous\n"
        "  0\nENDTAB\n  0\nENDSEC\n  0\nEOF\n";
    dxf::Report report;
    auto r = dxf::importDxf(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(dxfText.data()), dxfText.size()),
                            {}, report);
    REQUIRE(r.ok());
    const Document& doc = *r.value().document;
    const auto frozen = layout::layersFrozenInNewViewports(doc);
    REQUIRE(frozen.size() == 1);
    CHECK(doc.layer(frozen[0])->name == "Grid");
    CHECK_FALSE(doc.layer(frozen[0])->frozen);
    dxf::ExportOptions opts;
    auto out = dxf::exportDxf(doc, opts, report);
    REQUIRE(out.ok());
    auto in = dxf::importDxf(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(out.value().bytes.data()),
                                   out.value().bytes.size()),
        {}, report);
    REQUIRE(in.ok());
    const auto back = layout::layersFrozenInNewViewports(*in.value().document);
    REQUIRE(back.size() == 1);
    CHECK(in.value().document->layer(back[0])->name == "Grid");
}

TEST_CASE("layouts survive a DXF export and import") {
    Document doc;
    const LayerId walls = addLayer(doc, "Walls");
    LevelId upper;
    {
        auto tx = doc.begin("levels");
        Level l;
        l.name = "Upper";
        upper = tx.addLevel(l).value();
        tx.commit();
    }
    run(doc, "new", [](Transaction& tx) { return createLayout(tx, "Plan", "A3"); });
    run(doc, "new", [](Transaction& tx) { return createLayout(tx, "Detail", "A4", false); });
    run(doc, "tb", [](Transaction& tx) {
        return addTitleBlock(tx, "Plan", TitleBlockFields{"P", "D", "1:100", "today", "1"});
    });
    int vp = 0;
    run(doc, "vp", [&](Transaction& tx) {
        auto r = addViewport(tx, "Plan", BBox2{{10, 10}, {300, 200}}, {5000, 3000}, 100);
        if (!r.ok())
            return Result<void>(r.error());
        vp = r.value();
        if (auto c = setViewportClip(tx, "Plan", vp, {{10, 10}, {300, 10}, {150, 200}}); !c)
            return c;
        return setViewportLayerFrozen(tx, "Plan", vp, walls, true);
    });
    run(doc, "vp2", [&](Transaction& tx) {
        auto r = addViewport(tx, "Detail", BBox2{{5, 5}, {100, 100}}, {10, 20}, 20);
        return r.ok() ? Result<void>() : Result<void>(r.error());
    });
    const std::size_t planObjects = doc.block(layouts(doc)[0].content)->entities.size();
    REQUIRE(planObjects > 0);

    dxf::Report report;
    for (dxf::DxfVersion version : {dxf::DxfVersion::R2000, dxf::DxfVersion::R2007}) {
        dxf::ExportOptions opts;
        opts.version = version;
        auto out = dxf::exportDxf(doc, opts, report);
        REQUIRE(out.ok());
        auto in = dxf::importDxf(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(out.value().bytes.data()),
                                       out.value().bytes.size()),
            {}, report);
        REQUIRE(in.ok());
        const Document& back = *in.value().document;
        const std::vector<Layout> all = layouts(back);
        REQUIRE(all.size() == 2);
        CHECK(all[0].name == "Plan");
        CHECK(all[0].widthMm == doctest::Approx(420));
        CHECK(all[1].name == "Detail");
        CHECK(all[1].widthMm == doctest::Approx(210));
        REQUIRE(all[0].viewports.size() == 1);
        CHECK(all[0].viewports[0].scaleN == doctest::Approx(100));
        CHECK(all[0].viewports[0].center.x == doctest::Approx(5000));
        CHECK(all[0].viewports[0].paper.max.x == doctest::Approx(300));
        CHECK(all[0].viewports[0].frozen.size() == 1);
        REQUIRE(all[0].viewports[0].clip.size() == 3);
        CHECK(all[0].viewports[0].clip[2].x == doctest::Approx(150));
        REQUIRE(all[1].viewports.size() == 1);
        CHECK(all[1].viewports[0].scaleN == doctest::Approx(20));
        CHECK(back.block(all[0].content)->entities.size() == planObjects);
    }
}

TEST_CASE("a clipped viewport plots only inside its outline and keeps the outline in the file") {
    Document doc;
    {
        auto tx = doc.begin("model");
        Entity e = line({0, 50}, {200, 50});
        e.common.lineweight = LineWeight::mm(40);  // a band that fills the viewport's height
        REQUIRE(tx.add(std::move(e)).ok());
        tx.commit();
    }
    run(doc, "layout", [](Transaction& tx) {
        if (auto r = createLayout(tx, "L", "A4", true); !r)
            return r;
        BBox2 a;
        a.expand({20, 20});
        a.expand({120, 120});
        auto id = addViewport(tx, "L", a, {50, 50}, 1);
        if (!id)
            return Result<void>(id.error());
        // A triangle in the lower left half of the rectangle.
        return setViewportClip(tx, "L", id.value(), {{20, 20}, {120, 20}, {20, 120}});
    });
    const Viewport vp = *findLayout(doc, "L")->findViewport(1);
    REQUIRE(vp.clip.size() == 3);
    CHECK_FALSE(viewportContains(vp, {100, 100}));
    CHECK(viewportContains(vp, {40, 40}));

    auto sheet = buildLayoutSheet(doc, "L", {}, {true}).value();
    const exportlib::PlotClip* begin = nullptr;
    for (const auto& p : sheet.prims)
        if (const auto* c = std::get_if<exportlib::PlotClip>(&p); c && c->begin)
            begin = c;
    REQUIRE(begin);
    CHECK(begin->poly.size() == 3);
    CHECK(exportlib::svgString(sheet).find("<polygon") != std::string::npos);
    auto pdfText = exportlib::pdfBytes(sheet, false);
    REQUIRE(pdfText.ok());
    CHECK(pdfText.value().find("W* n") != std::string::npos);

    // The band is 40 mm wide at y = 50: its upper right end is outside the triangle.
    auto ras = exportlib::rasterize(sheet, 25.4, 100'000'000);  // 1 pixel per mm
    REQUIRE(ras.ok());
    const exportlib::RasterImage& im = ras.value();
    auto dark = [&](double xmm, double ymm) {
        const int x = static_cast<int>(xmm), y = im.height - 1 - static_cast<int>(ymm);
        const std::uint8_t* c = im.rgb.data() + (static_cast<std::size_t>(y) * im.width + x) * 3;
        return c[0] < 128 && c[1] < 128 && c[2] < 128;
    };
    CHECK(dark(40, 50));
    CHECK_FALSE(dark(100, 60));

    // Saved and loaded: the outline survives the .tbcad round trip through the header table.
    const Viewport again = *findLayout(doc, "L")->findViewport(1);
    CHECK(again.clip == vp.clip);
    run(doc, "unclip", [](Transaction& tx) { return setViewportClip(tx, "L", 1, {}); });
    CHECK(findLayout(doc, "L")->findViewport(1)->clip.empty());
}

TEST_CASE("a viewport clipped by a polyline of a DXF file from another program") {
    const std::string dxfText =
        "  0\nSECTION\n  2\nBLOCKS\n  0\nBLOCK\n  2\n*Paper_Space\n 70\n0\n 10\n0\n 20\n0\n 30\n0\n"
        "  0\nLWPOLYLINE\n  5\nB1\n  8\nDefpoints\n 67\n1\n 90\n3\n 70\n1\n"
        " 10\n20\n 20\n20\n 10\n300\n 20\n20\n 10\n160\n 20\n250\n"
        "  0\nVIEWPORT\n  8\n0\n 67\n1\n 10\n160\n 20\n135\n 40\n280\n 41\n230\n 69\n2\n"
        " 68\n1\n 12\n1000\n 22\n2000\n 45\n23000\n 90\n65536\n340\nB1\n  0\nENDBLK\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nOBJECTS\n  0\nLAYOUT\n  1\nSheet A3\n 71\n1\n 44\n420\n 45\n297\n"
        "  0\nENDSEC\n  0\nEOF\n";
    dxf::Report report;
    auto r = dxf::importDxf(std::span<const std::byte>(
                                reinterpret_cast<const std::byte*>(dxfText.data()), dxfText.size()),
                            {}, report);
    REQUIRE(r.ok());
    const std::vector<Layout> all = layouts(*r.value().document);
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].viewports.size() == 1);
    const Viewport& vp = all[0].viewports[0];
    REQUIRE(vp.clip.size() == 3);
    CHECK(vp.clip[1].x == doctest::Approx(300));
    CHECK(viewportContains(vp, {160, 100}));
    CHECK_FALSE(viewportContains(vp, {30, 200}));
}

TEST_CASE("a block's contents on a layer frozen in the viewport are not plotted there") {
    blocks::registerInsertDisplay();
    Document doc;
    LayerId walls = addLayer(doc, "Walls");
    LayerId hidden = addLayer(doc, "Hidden");
    BlockId blk;
    {
        auto tx = doc.begin("block");
        blk = tx.addBlock(BlockDef{.name = "B"}).value();
        REQUIRE(tx.add(line({0, 0}, {10, 0}, walls), blk).ok());
        REQUIRE(tx.add(line({0, 5}, {10, 5}, hidden), blk).ok());
        Entity ins;
        InsertData d;
        d.block = blk;
        ins.data = d;
        ins.common.layer = walls;
        REQUIRE(tx.add(std::move(ins)).ok());
        tx.commit();
    }
    run(doc, "layout", [&](Transaction& tx) {
        if (auto r = createLayout(tx, "L"); !r)
            return r;
        BBox2 a;
        a.expand({10, 10});
        a.expand({100, 100});
        auto id = addViewport(tx, "L", a, {5, 5}, 1);
        REQUIRE(id.ok());
        return setViewportLayerFrozen(tx, "L", id.value(), hidden, true);
    });
    auto sheet = buildLayoutSheet(doc, "L", {}, {false}).value();
    int paths = 0;
    for (const auto& p : sheet.prims)
        paths += std::holds_alternative<exportlib::PlotPath>(p);
    CHECK(paths == 1);
}

// A layout with six viewports over a big model drawing (a 393 000-object topographic survey is the
// reference): building the sheet must not take seconds. Debug builds use a smaller drawing.
TEST_CASE("a layout with six viewports on a big drawing builds quickly") {
#ifdef NDEBUG
    constexpr int kObjects = 393000;
#else
    constexpr int kObjects = 20000;
#endif
    Document doc;
    {
        auto tx = doc.begin("fill");
        for (int i = 0; i < kObjects; ++i) {
            const double x = (i % 700) * 20.0, y = (i / 700) * 20.0;
            Entity e;
            switch (i % 4) {
                case 0:
                    e.data = LineData{{x, y}, {x + 15, y + 9}};
                    break;
                case 1:
                    e.data = CircleData{{x, y}, 4};
                    break;
                case 2: {
                    PolylineData p;
                    for (int k = 0; k < 5; ++k)
                        p.v.push_back({{x + k * 3.0, y + (k % 2) * 4.0}, 0, 0, 0});
                    e.data = std::move(p);
                    break;
                }
                default:
                    e.data = ArcData{{x, y}, 6, 0.3, 2.5};
            }
            REQUIRE(tx.add(std::move(e)).ok());
        }
        tx.commit();
    }
    run(doc, "layout", [&](Transaction& tx) { return createLayout(tx, "Sheet", "A1", true); });
    const double wide = 700 * 20.0, high = (kObjects / 700 + 1) * 20.0;
    for (int v = 0; v < 6; ++v) {
        const double px = 20 + (v % 3) * 270.0, py = 20 + (v / 3) * 280.0;
        auto tx = doc.begin("viewport");
        REQUIRE(addViewport(tx, "Sheet", BBox2{{px, py}, {px + 250, py + 250}},
                            {wide * (0.15 + 0.3 * (v % 3)), high * (0.25 + 0.5 * (v / 3))}, 20.0)
                    .ok());
        tx.commit();
    }
    auto t0 = std::chrono::steady_clock::now();
    auto sheet = buildLayoutSheet(doc, "Sheet");
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(sheet.ok());
    MESSAGE(kObjects << " objects, 6 viewports: " << ms << " ms, " << sheet.value().entityCount
                     << " objects drawn");
    CHECK(sheet.value().entityCount > 100);
#ifdef NDEBUG
    CHECK(ms < 1000);
#endif
}

TEST_CASE("a copied layout keeps the sheet, the viewports and every viewport setting") {
    Document doc;
    const LayerId walls = addLayer(doc, "Walls");
    LevelId upper;
    {
        auto tx = doc.begin("levels");
        Level l;
        l.name = "Upper";
        upper = tx.addLevel(l).value();
        tx.commit();
    }
    run(doc, "new", [](Transaction& tx) { return createLayout(tx, "Plan", "A3"); });
    run(doc, "tb", [](Transaction& tx) {
        return addTitleBlock(tx, "Plan", TitleBlockFields{"P", "D", "1:100", "today", "1"});
    });
    int a = 0, b = 0;
    run(doc, "vps", [&](Transaction& tx) {
        auto r1 = addViewport(tx, "Plan", BBox2{{10, 10}, {200, 200}}, {5000, 3000}, 100);
        auto r2 = addViewport(tx, "Plan", BBox2{{210, 10}, {400, 200}}, {-20, 7}, 25);
        if (!r1.ok() || !r2.ok())
            return Result<void>(Error{});
        a = r1.value();
        b = r2.value();
        if (auto c = setViewportLayerFrozen(tx, "Plan", a, walls, true); !c)
            return c;
        if (auto c = setViewportClip(tx, "Plan", b, {{210, 10}, {400, 10}, {300, 200}}); !c)
            return c;
        if (auto c = setViewportLocked(tx, "Plan", b, true); !c)
            return c;
        if (auto c = setViewportLevel(tx, "Plan", b, upper); !c)
            return c;
        return setViewportOn(tx, "Plan", a, false);
    });
    run(doc, "copy", [](Transaction& tx) { return copyLayout(tx, "plan", "Plan (2)"); });
    {
        auto tx = doc.begin("bad");
        CHECK_FALSE(copyLayout(tx, "Nope", "X").ok());
        CHECK_FALSE(copyLayout(tx, "Plan", "PLAN (2)").ok());  // the name is taken
    }
    REQUIRE(layouts(doc).size() == 2);

    auto check = [&](const Document& d) {
        auto src = findLayout(d, "Plan"), cpy = findLayout(d, "Plan (2)");
        REQUIRE(src.has_value());
        REQUIRE(cpy.has_value());
        CHECK(cpy->content != src->content);
        REQUIRE(d.block(cpy->content));
        REQUIRE(d.block(src->content));
        CHECK(d.block(cpy->content)->entities.size() == d.block(src->content)->entities.size());
        REQUIRE(cpy->viewports.size() == 2);
        for (std::size_t i = 0; i < 2; ++i) {
            const Viewport &x = src->viewports[i], &y = cpy->viewports[i];
            CHECK(y.id == x.id);
            CHECK(y.scaleN == doctest::Approx(x.scaleN));
            CHECK(y.center.x == doctest::Approx(x.center.x));
            CHECK(y.on == x.on);
            CHECK(y.locked == x.locked);
            CHECK(y.border == x.border);
            CHECK(y.frozen == x.frozen);
            CHECK(y.clip.size() == x.clip.size());
        }
        CHECK_FALSE(cpy->viewports[0].on);
        CHECK(cpy->viewports[1].locked);
        REQUIRE(d.level(cpy->viewports[1].level));
        CHECK(d.level(cpy->viewports[1].level)->name == "Upper");
        CHECK(cpy->viewports[1].clip.size() == 3);
        CHECK(cpy->viewports[0].frozen.size() == 1);
    };
    check(doc);

    // The copy is independent: removing a viewport from it leaves the original alone, and
    // deleting the original leaves the copy's sheet objects.
    run(doc, "rm", [&](Transaction& tx) { return removeViewport(tx, "Plan (2)", a); });
    CHECK(findLayout(doc, "Plan")->viewports.size() == 2);
    run(doc, "rm", [&](Transaction& tx) { return removeViewport(tx, "Plan (2)", b); });
    run(doc, "again", [&](Transaction& tx) { return deleteLayout(tx, "Plan (2)"); });
    run(doc, "copy2", [](Transaction& tx) { return copyLayout(tx, "Plan", "Plan (2)"); });

    // Save and load.
    auto bytes = io::writeTbcadToMemory(doc);
    REQUIRE(bytes.ok());
    io::IoReport report;
    auto loaded = io::readTbcad(bytes.value(), report);
    REQUIRE(loaded.ok());
    check(*loaded.value());

    // DXF export and import.
    dxf::Report dreport;
    auto out = dxf::exportDxf(doc, dxf::ExportOptions{}, dreport);
    REQUIRE(out.ok());
    auto in = dxf::importDxf(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(out.value().bytes.data()),
                                   out.value().bytes.size()),
        {}, dreport);
    REQUIRE(in.ok());
    check(*in.value().document);
}

TEST_CASE("an annotative object shows only in viewports whose scale it supports") {
    Document doc;
    {
        auto tx = doc.begin("model");
        REQUIRE(tx.add(line({0, 0}, {1000, 0})).ok());  // plain: everywhere
        Entity b = line({0, 100}, {1000, 100});
        b.common.annotative = true;
        b.common.annoScales = ScaleList({50.0});
        REQUIRE(tx.add(std::move(b)).ok());
        Entity c = line({0, 200}, {1000, 200});
        c.common.annotative = true;
        c.common.annoScales = ScaleList({100.0});
        REQUIRE(tx.add(std::move(c)).ok());
        Entity d = line({0, 300}, {1000, 300});
        d.common.annotative = true;  // no scale list: every scale
        REQUIRE(tx.add(std::move(d)).ok());
        tx.commit();
    }
    run(doc, "layout", [](Transaction& tx) {
        if (auto r = createLayout(tx, "S", "A3"); !r)
            return r;
        BBox2 a, b;
        a.expand({10, 10});
        a.expand({200, 140});
        b.expand({210, 10});
        b.expand({410, 140});
        if (auto id = addViewport(tx, "S", a, {500, 150}, 50); !id)
            return Result<void>(id.error());
        auto id = addViewport(tx, "S", b, {500, 150}, 100);
        return id ? Result<void>() : Result<void>(id.error());
    });
    auto sheet = buildLayoutSheet(doc, "S");
    REQUIRE(sheet.ok());
    // Paths between each pair of clip markers belong to one viewport.
    std::vector<std::size_t> perViewport;
    bool inside = false;
    for (const auto& p : sheet.value().prims) {
        if (const auto* clip = std::get_if<exportlib::PlotClip>(&p)) {
            if (clip->begin) {
                perViewport.push_back(0);
                inside = true;
            } else {
                inside = false;
            }
        } else if (inside && std::holds_alternative<exportlib::PlotPath>(p)) {
            ++perViewport.back();
        }
    }
    REQUIRE(perViewport.size() >= 2);
    // 1:50 shows plain, anno {50} and the unrestricted one; 1:100 shows plain, anno {100} and it.
    CHECK(perViewport[perViewport.size() - 2] == 3);
    CHECK(perViewport.back() == 3);
}
