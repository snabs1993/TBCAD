#include <doctest/doctest.h>

#include <filesystem>

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

TEST_CASE("TABLEEXPORT writes a CSV that TABLEIMPORT turns back into the same table") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tbcad_table_csv";
    fs::create_directories(dir);
    const std::string file = (dir / "t.csv").string();
    Fx f;
    REQUIRE(f.host->run("TABLE", {"2", "2", "30", "8", "", "0,0"}).finished);
    const EntityId id = f.tableId();
    {
        auto tx = f.doc.begin("fill");
        REQUIRE(tx.modify(id, [](Entity& e) {
            auto* t = entityAs<TableData>(e);
            table::setText(*t, 0, 0, "Door");
            table::setText(*t, 0, 1, "W, mm");
            table::setText(*t, 1, 0, "D1");
            table::setText(*t, 1, 1, "900");
        }));
        tx.commit();
    }
    REQUIRE(f.host->run("TABLEEXPORT", {ScriptItem::pick(id, {1, -1}), file}).finished);
    REQUIRE(fs::exists(file));
    REQUIRE(f.host->run("TABLEIMPORT", {file, "100,0"}).finished);
    int tables = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (const auto* t = entityAs<TableData>(e); t && t->origin.x == 100) {
            ++tables;
            CHECK(table::rowCount(*t) == 2);
            CHECK(table::cellAt(*t, 0, 1).utf8 == "W, mm");
            CHECK(table::cellAt(*t, 1, 1).utf8 == "900");
        }
    });
    CHECK(tables == 1);
    fs::remove_all(dir);
}

TEST_CASE("TABLESTYLE defines styles, TABLE starts from the current one") {
    Fx f;
    f.host->run("TABLESTYLE", {"New", "Big", "50", "12", "4", "2", "", "Done"});
    auto s = table::findTableStyle(f.doc.header(), "big");
    REQUIRE(s);
    CHECK(s->colWidth == doctest::Approx(50));
    CHECK(s->margin == doctest::Approx(2));
    f.host->run("TABLESTYLE", {"New", "Big"});  // exists: refused
    CHECK(f.host->logContains("exists"));
    f.host->run("TABLESTYLE", {"Set", "Big"});
    CHECK(table::currentTableStyleName(f.doc.header()) == "Big");
    // The prompts offer the style's sizes; Enter takes them.
    f.host->run("TABLE", {"2", "3", "", "", "", "0,0"});
    const Entity* e = f.doc.find(f.tableId());
    REQUIRE(e);
    const auto* t = entityAs<TableData>(*e);
    REQUIRE(t);
    CHECK(t->colWidths[0] == doctest::Approx(50));
    CHECK(t->rowHeights[0] == doctest::Approx(12));
    CHECK(t->textHeight == doctest::Approx(4));
    CHECK(t->margin == doctest::Approx(2));
    // Modify, then Apply to the table already drawn.
    f.host->run("TABLESTYLE", {"Modify", "Big", "20", "6", "2", "0.5", "", "Done"});
    f.host->setSelection({f.tableId()});
    f.host->run("TABLESTYLE", {"Apply", "Big"});
    t = entityAs<TableData>(*f.doc.find(f.tableId()));
    CHECK(t->colWidths[1] == doctest::Approx(20));
    CHECK(t->rowHeights[1] == doctest::Approx(6));
    CHECK(t->margin == doctest::Approx(0.5));
    // Deleting the current style falls back to Standard; Standard stays.
    f.host->run("TABLESTYLE", {"Delete", "Big"});
    CHECK(table::currentTableStyleName(f.doc.header()) == "Standard");
    f.host->run("TABLESTYLE", {"Delete", "Standard"});
    CHECK(f.host->logContains("cannot be deleted"));
    CHECK(f.host->undo());
}

TEST_CASE("TABLESTYLE cell styles, set per role, stored with the style, used by TABLE and Apply") {
    Fx f;
    // Title: height 5, colour 1, fill 8, centre; then Header row on; then Data: right aligned.
    f.host->run("TABLESTYLE",
                {"New", "Plan",   "30",        "8",   "2.8",  "1", "",  "Title", "5",     "1",
                 "8",   "Center", "HeaderRow", "Yes", "Data", "0", "0", "0",     "Right", "Done"});
    auto s = table::findTableStyle(f.doc.header(), "Plan");
    REQUIRE(s);
    CHECK(s->headerRow);
    CHECK(s->title.textHeight == doctest::Approx(5));
    CHECK(s->title.color == 1);
    CHECK(s->title.fill == 8);
    CHECK(s->title.align == 1);
    CHECK(s->data.align == 2);
    CHECK(s->header.isDefault());
    f.host->run("TABLESTYLE", {"Set", "Plan"});
    f.host->run("TABLE", {"2", "3", "", "", "", "0,0"});
    const auto* t = entityAs<TableData>(*f.doc.find(f.tableId()));
    REQUIRE(t);
    CHECK(t->headerRow);
    CHECK(t->titleStyle.fill == 8);
    CHECK(t->dataStyle.align == 2);
    // Restyle: a later change of the style reaches the table through Apply.
    f.host->run("TABLESTYLE", {"Modify", "Plan", "30", "8", "2.8", "1", "", "Header", "3", "2", "0",
                               "Left", "Done"});
    f.host->setSelection({f.tableId()});
    f.host->run("TABLESTYLE", {"Apply", "Plan"});
    t = entityAs<TableData>(*f.doc.find(f.tableId()));
    CHECK(t->headerStyle.textHeight == doctest::Approx(3));
    CHECK(t->headerStyle.color == 2);
    CHECK(f.host->undo());
}

TEST_CASE("TABLEEDIT aligns a cell or a column, editing the text keeps the alignment") {
    Fx f;
    f.host->run("TABLE", {"2", "2", "40", "10", ScriptItem::enter(), "0,0"});
    const EntityId id = f.tableId();
    REQUIRE(id);
    // Pick the cell at row 0, column 1 (x 40..80, y -0..-10 below the top edge).
    f.host->run("TABLEEDIT", {ScriptItem::pick(id, {60, -5}), "Align", "Right", "Text", "42", ""});
    const auto* t = entityAs<TableData>(*f.doc.find(id));
    REQUIRE(t);
    CHECK(table::cellAlign(table::cellAt(*t, 0, 1)) == table::CellAlign::Right);
    CHECK(table::cellBody(table::cellAt(*t, 0, 1).utf8) == "42");
    f.host->run("TABLEEDIT", {ScriptItem::pick(id, {20, -5}), "AlignColumn", "Center", ""});
    t = entityAs<TableData>(*f.doc.find(id));
    CHECK(table::cellAlign(table::cellAt(*t, 0, 0)) == table::CellAlign::Center);
    CHECK(table::cellAlign(table::cellAt(*t, 1, 0)) == table::CellAlign::Center);
    CHECK(table::cellAlign(table::cellAt(*t, 0, 1)) ==
          table::CellAlign::Right);  // other column kept
    // The aligned text lands at the right edge, with its right edge as the anchor.
    bool right = false;
    for (const auto& item : table::layout(*t).texts)
        right = right || (item.utf8 == "42" && item.attach == Attachment::MiddleRight);
    CHECK(right);
}
