#include <doctest/doctest.h>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/geom/geom.h"
#include "tbcad/table/table.h"
#include "tbcad/tablecmd/tablecmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

struct Fx {
    Document doc;
    Registry reg;
    std::unique_ptr<ScriptHost> host;
    Fx() {
        registerBuiltinCommands(reg);
        tablecmd::registerCommands(reg);
        host = std::make_unique<ScriptHost>(doc, reg);
    }
    EntityId tableId() const {
        EntityId id{};
        doc.forEachEntity([&](const Entity& e) {
            if (kind(e) == EntityKind::Table)
                id = e.common.id;
        });
        return id;
    }
    const TableData* table() const {
        const Entity* e = doc.find(tableId());
        return e ? entityAs<TableData>(*e) : nullptr;
    }
};

}  // namespace

TEST_CASE("TABLE draws a titled table at the picked corner and undoes in one step") {
    Fx f;
    CHECK(f.reg.find("TB") == f.reg.find("TABLE"));
    auto r = f.host->run("TABLE", {"3", "4", "40", "10", "Parts list", "100,200"});
    CHECK(r.finished);
    const TableData* t = f.table();
    REQUIRE(t);
    CHECK(table::colCount(*t) == 3);
    CHECK(table::rowCount(*t) == 5);  // 4 rows plus the title
    CHECK(t->origin.x == doctest::Approx(100));
    CHECK(t->origin.y == doctest::Approx(200));
    CHECK(t->titleRow);
    CHECK(table::cellAt(*t, 0, 0).utf8 == "Parts list");
    CHECK(f.host->undo());
    CHECK_FALSE(f.tableId());
}

TEST_CASE("TABLE without a title has exactly the requested rows") {
    Fx f;
    REQUIRE(f.host->run("TABLE", {"2", "2", "20", "6", ScriptItem::enter(), "0,0"}).finished);
    const TableData* t = f.table();
    REQUIRE(t);
    CHECK(table::rowCount(*t) == 2);
    CHECK_FALSE(t->titleRow);
}

TEST_CASE("TABLEEDIT: click a cell to type its text, then rows, columns and merges") {
    Fx f;
    REQUIRE(f.host->run("TABLE", {"3", "3", "40", "10", ScriptItem::enter(), "0,0"}).finished);
    const EntityId id = f.tableId();
    REQUIRE(id);
    // cell (0,0) is x 0..40, y 0..-10; cell (1,1) is x 40..80, y -10..-20
    auto r = f.host->run("TABLEEDIT", {ScriptItem::pick(id, {20, -5}), "20,-5", "Alpha", "60,-15",
                                       "Beta", "RowBelow", "ColRight", ScriptItem::enter()});
    CHECK(r.finished);
    const TableData* t = f.table();
    REQUIRE(t);
    CHECK(table::rowCount(*t) == 4);
    CHECK(table::colCount(*t) == 4);
    CHECK(table::cellAt(*t, 0, 0).utf8 == "Alpha");
    CHECK(table::cellAt(*t, 1, 1).utf8 == "Beta");
    CHECK(f.host->undo());
    const TableData* back = f.table();
    REQUIRE(back);
    CHECK(table::rowCount(*back) == 3);
    CHECK(table::cellAt(*back, 0, 0).utf8.empty());
}

TEST_CASE("TABLEEDIT: merge, unmerge, width and delete") {
    Fx f;
    REQUIRE(f.host->run("TABLE", {"3", "3", "40", "10", ScriptItem::enter(), "0,0"}).finished);
    const EntityId id = f.tableId();
    auto r =
        f.host->run("TABLEEDIT", {ScriptItem::pick(id, {20, -5}), "Merge", "60,-15", "Unmerge",
                                  "DeleteRow", "DeleteCol", "Width", "55", ScriptItem::enter()});
    CHECK(r.finished);
    const TableData* t = f.table();
    REQUIRE(t);
    CHECK(table::rowCount(*t) == 2);
    CHECK(table::colCount(*t) == 2);
    CHECK(t->colWidths[0] == doctest::Approx(55));
    CHECK(table::cellAt(*t, 0, 0).colSpan == 1);
}

TEST_CASE("a table has an explode hook that gives lines and text") {
    Fx f;
    CHECK(blocks::ExplodeRegistry::has(EntityKind::Table));
    CHECK(geom::DisplayRegistry::has(EntityKind::Table));
}
