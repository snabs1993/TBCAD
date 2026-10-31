// SHEETLAYER: the layers of the current layout sheet only.
#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layout/layout.h"

using namespace tbcad;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    LayerId grid;
    Fixture() {
        registerAllCommands(reg);
        auto tx = doc.begin("setup");
        Layer l;
        l.name = "Grid";
        grid = tx.addLayer(l).value();
        REQUIRE(layout::createLayout(tx, "Plan").ok());
        REQUIRE(layout::createLayout(tx, "Section").ok());
        tx.commit();
        host.setSetting("layout.current", "Plan");
    }
    cmd::ScriptHost host{doc, reg};
    const layout::LayerOverride* plan() {
        plan_ = layout::findLayout(doc, "Plan");
        return plan_ ? plan_->findOverride(grid) : nullptr;
    }
    std::optional<layout::Layout> plan_;
};

}  // namespace

TEST_CASE("SHEETLAYER hides and restyles a layer on the current sheet only") {
    Fixture f;
    auto r = f.host.run("SHEETLAYER", {"Off", "Grid", "Color", "5", "Grid", "LW", "0.5", "Grid",
                                       cmd::ScriptItem::enter()});
    CHECK(r.finished);
    REQUIRE(f.plan() != nullptr);
    CHECK(f.plan()->on == std::optional<bool>(false));
    CHECK(f.plan()->color == std::optional<Color>(Color::index(5)));
    CHECK(f.plan()->lineweight == std::optional<LineWeight>(LineWeight::hundredths(50)));
    CHECK(layout::findLayout(f.doc, "Section")->layerOverrides.empty());
    CHECK(f.doc.layer(f.grid)->on);
    CHECK(f.doc.layer(f.grid)->color == Color::index(7));

    // Drawing takes one value back; On again equals the drawing, so it is not kept either.
    r = f.host.run("SHEETLAYER",
                   {"Color", "Drawing", "Grid", "On", "Grid", cmd::ScriptItem::enter()});
    CHECK(r.finished);
    REQUIRE(f.plan() != nullptr);
    CHECK_FALSE(f.plan()->color);
    CHECK_FALSE(f.plan()->on);
    CHECK(f.plan()->lineweight);

    // Undo puts the sheet back as it was before the command.
    f.host.undo();
    CHECK(f.plan()->color == std::optional<Color>(Color::index(5)));

    // Sheet picks another layout; Reset clears a layer, ResetAll the sheet.
    r = f.host.run("SHEETLAYER", {"Sheet", "Section", "NoPlot", "Grid", cmd::ScriptItem::enter()});
    CHECK(r.finished);
    CHECK(layout::findLayout(f.doc, "Section")->layerOverrides.size() == 1);
    r = f.host.run("SHEETLAYER", {"Reset", "Grid", cmd::ScriptItem::enter()});
    CHECK(f.plan() == nullptr);
    r = f.host.run("SHEETLAYER", {"Sheet", "Section", "ResetAll", cmd::ScriptItem::enter()});
    CHECK(layout::findLayout(f.doc, "Section")->layerOverrides.empty());
}

TEST_CASE("SHEETLAYER refuses ByLayer colours and unknown layers") {
    Fixture f;
    f.host.run("SHEETLAYER", {"Color", "ByLayer", "Grid", cmd::ScriptItem::enter()});
    CHECK(f.plan() == nullptr);
    f.host.run("SHEETLAYER", {"Off", "Nope", cmd::ScriptItem::enter()});
    CHECK(f.plan() == nullptr);
}
