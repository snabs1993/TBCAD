#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/geom/entity.h"
#include "tbcad/text/field.h"
#include "tbcad/text/text.h"

using namespace tbcad;

namespace {
struct Fixture {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Fixture() : host(doc, (registerAllCommands(reg), reg)) {}
    EntityId last() const {
        EntityId id{};
        doc.forEachEntity([&](const Entity& e) { id = std::max(id, e.common.id); });
        return id;
    }
};
}  // namespace

TEST_CASE("fields work out length, area, text, file name and sheet") {
    Fixture f;
    f.host.run("LINE", {"0,0", "3000,4000", cmd::ScriptItem::enter()});
    const EntityId line = f.last();
    f.host.run("CIRCLE", {"0,0", "1000"});
    const EntityId circle = f.last();
    f.host.run("TEXT", {"0,0", "2.5", "0", "Hello"});
    const EntityId text = f.last();
    f.doc.setFilePath("D:\\jobs\\House A.tbcad");
    f.host.run("FIELD", {"SetSheet", "A-101"});

    auto x = [&](const std::string& s) { return text::expandFields(f.doc, s, "2026-10-13"); };
    CHECK(x("L=%<length:#" + std::to_string(line.value) + ":1:m>% m") == "L=5.0 m");
    CHECK(x("%<area:#" + std::to_string(circle.value) + ":2:m>%") == "3.14");
    CHECK(x("%<text:#" + std::to_string(text.value) + ">%!") == "Hello!");
    CHECK(x("%<filename>%") == "House A.tbcad");
    CHECK(x("%<filepath>%") == "D:\\jobs\\House A.tbcad");
    CHECK(x("%<sheet>%") == "A-101");
    CHECK(x("%<date>%") == "2026-10-13");
    CHECK(x("%<length:#999999>%") == "####");
    CHECK(x("no field, 100%") == "no field, 100%");
}

TEST_CASE("FIELD makes a text - the text follows the object it measures") {
    Fixture f;
    f.host.run("LINE", {"0,0", "100,0", cmd::ScriptItem::enter()});
    const EntityId line = f.last();
    REQUIRE(f.host.run("FIELD", {"Length", cmd::ScriptItem::pick(line, {50, 0}), "0,10"}).finished);
    const Entity* e = f.doc.find(f.last());
    REQUIRE(e);
    const auto* t = entityAs<TextData>(*e);
    REQUIRE(t);
    CHECK(t->utf8 == "%<length:#" + std::to_string(line.value) + ":2>%");

    auto width = [&]() {
        DisplayList d;
        geom::buildDisplay(f.doc, *f.doc.find(f.last()), geom::DisplayContext{}, d);
        double lo = 1e30, hi = -1e30;
        auto take = [&](Vec2 v) {
            lo = std::min(lo, v.x);
            hi = std::max(hi, v.x);
        };
        for (const auto& item : d.items) {
            if (const auto* p = std::get_if<DisplayPolyline>(&item))
                for (Vec2 v : p->pts)
                    take(v);
            if (const auto* fl = std::get_if<DisplayFill>(&item))  // filled glyphs
                for (const auto& ring : fl->rings)
                    for (Vec2 v : ring)
                        take(v);
        }
        return hi - lo;
    };
    const double before = width();
    {
        auto tx = f.doc.begin("longer");
        (void)tx.modify(line, [](Entity& l) {
            if (auto* ld = entityAs<LineData>(l))
                ld->b = {100000, 0};
        });
        tx.commit();
    }
    CHECK(width() > before);  // "100.00" became "100000.00": more glyphs
}

TEST_CASE("FIELD SetSheet stores the sheet number") {
    Fixture f;
    f.host.run("FIELD", {"SetSheet", "B-7"});
    CHECK(f.doc.header().custom.at("sheet.number") == "B-7");
    f.host.undo();
    CHECK(f.doc.header().custom.count("sheet.number") == 0);
}

TEST_CASE("DXF export writes the shown value of a field") {
    Fixture f;
    f.host.run("FIELD", {"SetSheet", "C-3"});
    f.host.run("FIELD", {"Sheet", "0,0"});
    // A name of its own per run: a leftover file would stop EXPORTDXF at its overwrite question.
    const std::string file =
        (std::filesystem::temp_directory_path() /
         ("tbcad_field_export_" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".dxf"))
            .string();
    REQUIRE(f.host.run("EXPORTDXF", {file, "R2000"}).finished);
    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }  // closed before the file is removed (Windows cannot delete an open file)
    CHECK(text.find("C-3") != std::string::npos);
    CHECK(text.find("%<sheet>%") == std::string::npos);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

namespace {
HatchLoop squareLoop(double x0, double y0, double x1, double y1, bool outer) {
    HatchLoop l;
    l.outer = outer;
    l.edges = {HatchLineEdge{{x0, y0}, {x1, y0}}, HatchLineEdge{{x1, y0}, {x1, y1}},
               HatchLineEdge{{x1, y1}, {x0, y1}}, HatchLineEdge{{x0, y1}, {x0, y0}}};
    return l;
}
}  // namespace

TEST_CASE("fields: hatch and ellipse area, radius, position, layer, block, attribute, scale") {
    Fixture f;
    EntityId hatch{}, ell{}, circle{}, ins{};
    {
        auto tx = f.doc.begin("objects");
        HatchData h;
        h.loops = {squareLoop(0, 0, 100, 100, true), squareLoop(20, 20, 40, 40, false)};
        hatch = tx.add(Entity{{}, std::move(h)}).value();
        EllipseData e;
        e.center = {5, 6};
        e.majorAxis = {10, 0};
        e.ratio = 0.5;
        ell = tx.add(Entity{{}, e}).value();
        circle = tx.add(Entity{{}, CircleData{{7, 8}, 25}}).value();
        auto blk = tx.addBlock(BlockDef{.name = "Door"});
        REQUIRE(blk.ok());
        InsertData d{blk.value(), {30, 40}, {1, 1}, 0};
        d.attribs = {{"TAG1", "value one"}};
        ins = tx.add(Entity{{}, std::move(d)}).value();
        tx.commit();
    }
    auto x = [&](const std::string& s) { return text::expandFields(f.doc, s, "2026-10-13"); };
    auto id = [](EntityId e) { return std::to_string(e.value); };
    CHECK(x("%<area:#" + id(hatch) + ":0>%") == "9600");  // 100x100 minus the 20x20 island
    CHECK(x("%<area:#" + id(ell) + ":1>%") == "157.1");   // pi * 10 * 5
    CHECK(x("%<radius:#" + id(circle) + ">%") == "25.00");
    CHECK(x("%<diameter:#" + id(circle) + ":0>%") == "50");
    CHECK(x("%<x:#" + id(circle) + ">% %<y:#" + id(circle) + ">%") == "7.00 8.00");
    CHECK(x("%<x:#" + id(ins) + ":1>%") == "30.0");
    CHECK(x("%<layer:#" + id(circle) + ">%") == "0");
    CHECK(x("%<block:#" + id(ins) + ">%") == "Door");
    CHECK(x("%<attr:#" + id(ins) + ":tag1>%") == "value one");
    CHECK(x("%<attr:#" + id(ins) + ":nope>%") == "####");
    CHECK(x("%<block:#" + id(circle) + ">%") == "####");
    CHECK(x("%<perimeter:#" + id(circle) + ":0>%") == "157");
    CHECK(x("%<scale>%") == "1:1");
}

TEST_CASE("FIELD offers layer, block, attribute and radius") {
    Fixture f;
    f.host.run("CIRCLE", {"0,0", "10"});
    const EntityId circle = f.last();
    REQUIRE(
        f.host.run("FIELD", {"Radius", cmd::ScriptItem::pick(circle, {10, 0}), "0,20"}).finished);
    const auto* t = entityAs<TextData>(*f.doc.find(f.last()));
    REQUIRE(t);
    CHECK(t->utf8 == "%<radius:#" + std::to_string(circle.value) + ":2>%");
    REQUIRE(
        f.host.run("FIELD", {"Layer", cmd::ScriptItem::pick(circle, {10, 0}), "0,30"}).finished);
    CHECK(entityAs<TextData>(*f.doc.find(f.last()))->utf8 ==
          "%<layer:#" + std::to_string(circle.value) + ">%");
    REQUIRE(f.host.run("FIELD", {"Scale", "0,40"}).finished);
    CHECK(entityAs<TextData>(*f.doc.find(f.last()))->utf8 == "%<scale>%");
}

TEST_CASE("FONTALT and FONTMAP are kept as settings and given to the font code") {
    Fixture f;
    f.host.run("FONTALT", {"simplex.shx"});
    CHECK(f.host.setting("text.fontalt").value_or("") == "simplex.shx");
    f.host.run("FONTALT", {"."});
    CHECK(f.host.setting("text.fontalt").value_or("x").empty());

    const auto file = std::filesystem::temp_directory_path() / "tbcad_fontmap_test.fmp";
    std::ofstream(file) << "romans;Arial\n";
    f.host.run("FONTMAP", {file.string()});
    CHECK(f.host.setting("text.fontmap.file").value_or("") == file.string());
    TextStyle st;
    st.shxFile = "romans.shx";
    st.fontFamily = "TBCAD Stroke";
    CHECK(text::resolveStyleFont(st).wanted == "Arial");
    f.host.run("FONTMAP", {"."});
    std::filesystem::remove(file);
    CHECK(text::resolveStyleFont(st).wanted == "romans.shx");
}
