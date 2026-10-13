#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/geom/entity.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/linetype_file.h"

using namespace tbcad;
using namespace tbcad::layers;

TEST_CASE("the shipped linetype file parses without problems") {
    LinetypeFile f = parseLinetypes(builtinLinetypeText());
    CHECK(f.problems.empty());
    auto has = [&](const char* n) {
        for (const Linetype& lt : f.linetypes)
            if (lt.name == n)
                return true;
        return false;
    };
    for (const char* n : {"Dashed", "Hidden", "Center", "Phantom", "DashDot", "Dot", "Border"})
        CHECK(has(n));
    for (const Linetype& lt : f.linetypes) {
        CHECK_FALSE(lt.pattern.empty());
        CHECK_FALSE(lt.description.empty());
    }
    // The pattern values are exactly what the file says.
    for (const Linetype& lt : f.linetypes)
        if (lt.name == "Dashed")
            CHECK(lt.pattern == std::vector<double>{12, -6});
}

TEST_CASE("the linetype file on disk matches the compiled-in copy") {
    auto onDisk = readLinetypeFile(std::filesystem::path(TBCAD_SOURCE_ASSETS_DIR) / "linetypes" /
                                   "tbcad.lintypes");
    REQUIRE(onDisk.ok());
    LinetypeFile embedded = parseLinetypes(builtinLinetypeText());
    REQUIRE(onDisk.value().linetypes.size() == embedded.linetypes.size());
    for (std::size_t i = 0; i < embedded.linetypes.size(); ++i)
        CHECK(onDisk.value().linetypes[i].pattern == embedded.linetypes[i].pattern);
}

TEST_CASE("parser reports bad lines and keeps the good ones") {
    LinetypeFile f = parseLinetypes(
        "# comment\n"
        "\n"
        "Good ; two dashes ; 4 -2\n"
        "NoPattern ; nothing ;\n"
        "Words ; text ; a b\n"
        "missing semicolons\n"
        "Continuous ; reserved ; 1 -1\n"
        "Zero ; no length ; 0 0\n"
        "Comma ; commas work too ; 3,-1.5,0,-1.5\n"
        "good ; duplicate ; 1 -1\n");
    REQUIRE(f.linetypes.size() == 2);
    CHECK(f.linetypes[0].name == "Good");
    CHECK(f.linetypes[1].pattern == std::vector<double>{3, -1.5, 0, -1.5});
    CHECK(f.problems.size() == 6);
    CHECK(f.problems[0].rfind("line 4", 0) == 0);
}

TEST_CASE("loading adds missing linetypes and skips existing ones") {
    Document doc;
    LinetypeFile f = parseLinetypes(builtinLinetypeText());
    {
        auto tx = doc.begin("load");
        auto r = loadLinetypes(doc, tx, f, {"Dashed", "hidden"});
        REQUIRE(r.ok());
        CHECK(r.value().size() == 2);
        CHECK_FALSE(loadLinetypes(doc, tx, f, {"Nope"}).ok());
        tx.commit();
    }
    REQUIRE(doc.linetypeByName("Dashed") != nullptr);
    CHECK(doc.linetypeByName("HIDDEN") != nullptr);
    CHECK(doc.linetypeByName("Center") == nullptr);
    {
        auto tx = doc.begin("again");
        auto r = loadLinetypes(doc, tx, f);
        REQUIRE(r.ok());
        CHECK(r.value().size() == f.linetypes.size() - 2);
        tx.commit();
    }
    CHECK(doc.linetypes().size() == f.linetypes.size() + 1);  // plus Continuous
    CHECK(doc.linetypeByName("Continuous") != nullptr);
}

TEST_CASE("a dashed line's display list carries the dash pattern; sub-layers inherit it") {
    Document doc;
    LinetypeFile f = parseLinetypes(builtinLinetypeText());
    LayerId parent, child;
    {
        auto tx = doc.begin("setup");
        REQUIRE(loadLinetypes(doc, tx, f, {"Dashed"}).ok());
        const Linetype* dashed = doc.linetypeByName("Dashed");
        REQUIRE(dashed != nullptr);
        parent = createLayer(doc, tx, "P").value();
        child = createLayer(doc, tx, "C", parent).value();
        REQUIRE(setLayerLinetype(doc, tx, parent, dashed->id).ok());
        tx.commit();
    }
    for (LayerId layer : {parent, child}) {
        auto tx = doc.begin("line");
        Entity e{{}, LineData{{0, 0}, {100, 0}}};
        e.common.layer = layer;
        e.common.linetypeScale = 2;
        auto id = tx.add(std::move(e));
        REQUIRE(id.ok());
        tx.commit();
        DisplayList out;
        geom::buildDisplay(doc, *doc.find(id.value()), geom::DisplayContext{}, out);
        REQUIRE(out.items.size() == 1);
        const auto& pl = std::get<DisplayPolyline>(out.items[0]);
        REQUIRE(pl.style.pattern != nullptr);
        CHECK(*pl.style.pattern == std::vector<double>{12, -6});
        CHECK(pl.style.patternScale == doctest::Approx(2.0));
    }
    // A line set to Continuous on a dashed layer overrides it.
    auto tx = doc.begin("override");
    Entity e{{}, LineData{{0, 0}, {100, 0}}};
    e.common.layer = parent;
    e.common.linetype = kLinetypeContinuous;
    auto id = tx.add(std::move(e));
    tx.commit();
    DisplayList out;
    geom::buildDisplay(doc, *doc.find(id.value()), geom::DisplayContext{}, out);
    CHECK(std::get<DisplayPolyline>(out.items[0]).style.pattern == nullptr);
}

TEST_CASE("-LINETYPE Load / File / List through the script host") {
    Document doc;
    cmd::Registry reg;
    registerAllCommands(reg);
    cmd::ScriptHost host(doc, reg);
    host.run("-LINETYPE", {"Load", "Center,Dot", "List", cmd::ScriptItem::enter()});
    CHECK(doc.linetypeByName("Center") != nullptr);
    CHECK(doc.linetypeByName("Dot") != nullptr);
    CHECK(host.logContains("Loaded 2"));
    CHECK(host.logContains("Continuous"));
    host.clearLog();
    host.run("-LINETYPE", {"Load", "*", cmd::ScriptItem::enter()});
    CHECK(host.logContains("Loaded"));
    host.clearLog();
    host.run("-LINETYPE", {"Load", "*", cmd::ScriptItem::enter()});
    CHECK(host.logContains("already loaded"));
    host.clearLog();
    host.run("-LINETYPE", {"File", "/definitely/not/here.lintypes", cmd::ScriptItem::enter()});
    CHECK(host.logContains("Cannot open"));
}
