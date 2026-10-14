#include <doctest/doctest.h>

#include <regex>
#include <sstream>

#include "tbcad/blocks/attributes.h"
#include "tbcad/blocks/display.h"
#include "tbcad/dxf/reader.h"
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

TEST_CASE("a viewport can show one level only; the choice is saved and plotted") {
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

TEST_CASE("title block and sheet objects are plotted; viewports can be switched off") {
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

TEST_CASE("the title block is a block with attributes; a second one reuses the definition") {
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
