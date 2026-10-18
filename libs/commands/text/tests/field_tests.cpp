#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/geom/entity.h"
#include "tbcad/text/field.h"

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
