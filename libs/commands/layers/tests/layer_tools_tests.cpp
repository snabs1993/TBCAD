#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/layers/layer_ops.h"

using namespace tbcad;

namespace {

struct Fixture {
    Document doc;
    cmd::Registry reg;
    Fixture() { registerAllCommands(reg); }
    cmd::ScriptHost host{doc, reg};
    LayerId a, b;
    EntityId ea, eb;

    // Two layers "A" and "B" with one line each; layer 0 stays current.
    void setup() {
        auto tx = doc.begin("setup");
        a = layers::createLayer(doc, tx, "A").value();
        b = layers::createLayer(doc, tx, "B").value();
        Entity x{{}, LineData{{0, 0}, {10, 0}}};
        x.common.layer = a;
        ea = tx.add(x).value();
        Entity y{{}, LineData{{0, 5}, {10, 5}}};
        y.common.layer = b;
        eb = tx.add(y).value();
        tx.commit();
    }
    cmd::ScriptResult run(const char* c, std::vector<cmd::ScriptItem> items) {
        return host.run(c, items);
    }
};

}  // namespace

TEST_CASE("LAYOFF, LAYFRZ and LAYLCK act on the layers of the selected objects") {
    Fixture f;
    f.setup();
    CHECK(f.run("LAYOFF", {cmd::ScriptItem::select({f.ea}), cmd::ScriptItem::enter()}).finished);
    CHECK_FALSE(f.doc.layer(f.a)->on);
    CHECK(f.doc.layer(f.b)->on);
    CHECK(f.run("LAYFRZ", {cmd::ScriptItem::select({f.eb}), cmd::ScriptItem::enter()}).finished);
    CHECK(f.doc.layer(f.b)->frozen);
    CHECK(f.run("LAYLCK", {cmd::ScriptItem::select({f.ea}), cmd::ScriptItem::enter()}).finished);
    CHECK(f.doc.layer(f.a)->locked);
    CHECK(f.run("LAYULK", {cmd::ScriptItem::select({f.ea}), cmd::ScriptItem::enter()}).finished);
    CHECK_FALSE(f.doc.layer(f.a)->locked);
    CHECK(f.run("LAYON", {}).finished);
    CHECK(f.doc.layer(f.a)->on);
    CHECK(f.run("LAYTHW", {}).finished);
    CHECK_FALSE(f.doc.layer(f.b)->frozen);
}

TEST_CASE("LAYFRZ refuses the current layer but still freezes the others") {
    Fixture f;
    f.setup();
    {
        auto tx = f.doc.begin("cur");
        REQUIRE(layers::setCurrentLayer(f.doc, tx, f.a).ok());
        tx.commit();
    }
    CHECK(f.run("LAYFRZ", {cmd::ScriptItem::select({f.ea, f.eb}), cmd::ScriptItem::enter()})
              .finished);
    CHECK_FALSE(f.doc.layer(f.a)->frozen);
    CHECK(f.doc.layer(f.b)->frozen);
}

TEST_CASE("LAYISO hides the other layers and LAYUNISO brings back the earlier state") {
    Fixture f;
    f.setup();
    {
        auto tx = f.doc.begin("off");
        REQUIRE(layers::setOn(f.doc, tx, f.b, false).ok());
        tx.commit();
    }
    CHECK(f.run("LAYISO", {cmd::ScriptItem::select({f.ea}), cmd::ScriptItem::enter()}).finished);
    CHECK(f.doc.layer(f.a)->on);
    CHECK_FALSE(f.doc.layer(f.b)->on);
    // A second LAYISO keeps the original snapshot.
    CHECK(f.run("LAYISO", {cmd::ScriptItem::select({f.eb}), cmd::ScriptItem::enter()}).finished);
    CHECK(f.doc.layer(f.b)->on);
    CHECK_FALSE(f.doc.layer(f.a)->on);
    CHECK(f.run("LAYUNISO", {}).finished);
    CHECK(f.doc.layer(f.a)->on);
    CHECK_FALSE(f.doc.layer(f.b)->on);  // it was off before the first LAYISO
    CHECK(f.doc.header().custom.count("tbcad.layiso") == 0);
}

TEST_CASE("LAYMCH moves objects to the layer of the picked object") {
    Fixture f;
    f.setup();
    CHECK(f.run("LAYMCH", {cmd::ScriptItem::select({f.ea}), cmd::ScriptItem::enter(),
                           cmd::ScriptItem::pick(f.eb)})
              .finished);
    CHECK(f.doc.find(f.ea)->common.layer == f.b);
    CHECK(f.doc.layer(f.a) != nullptr);
}

TEST_CASE("LAYCHANGE moves objects to a named layer and asks again for an unknown name") {
    Fixture f;
    f.setup();
    CHECK(
        f.run("LAYCHANGE", {cmd::ScriptItem::select({f.ea}), cmd::ScriptItem::enter(), "Nope", "B"})
            .finished);
    CHECK(f.host.logContains("Nope"));
    CHECK(f.doc.find(f.ea)->common.layer == f.b);
    CHECK(f.host.undo());
    CHECK(f.doc.find(f.ea)->common.layer == f.a);
}

TEST_CASE("LAYMRG moves everything over and removes the source layer, one undo step") {
    Fixture f;
    f.setup();
    CHECK(f.run("LAYMRG", {cmd::ScriptItem::select({f.ea}), cmd::ScriptItem::enter(),
                           cmd::ScriptItem::pick(f.eb)})
              .finished);
    CHECK(f.doc.layer(f.a) == nullptr);
    CHECK(f.doc.find(f.ea)->common.layer == f.b);
    f.host.undo();
    CHECK(f.doc.layer(f.a) != nullptr);
    CHECK(f.doc.find(f.ea)->common.layer == f.a);
}

TEST_CASE("LAYDEL erases the layer with its objects only after confirmation") {
    Fixture f;
    f.setup();
    CHECK(f.run("LAYDEL", {cmd::ScriptItem::pick(f.ea), "No"}).finished);
    CHECK(f.doc.layer(f.a) != nullptr);
    CHECK(f.run("LAYDEL", {cmd::ScriptItem::pick(f.ea), "Yes"}).finished);
    CHECK(f.doc.layer(f.a) == nullptr);
    CHECK(f.doc.find(f.ea) == nullptr);
    CHECK(f.doc.find(f.eb) != nullptr);
}

TEST_CASE("DIMLAYER and TEXTLAYER send new dimensions and text to their layer, '.' undoes it") {
    Fixture f;
    f.setup();
    f.host.run("DIMLAYER", {"A"});
    f.host.run("TEXTLAYER", {"B"});
    f.host.run("DIMLINEAR", {"0,0", "100,0", "50,10"});
    f.host.run("TEXT", {"0,20", "5", "0", "hi"});
    LayerId dimLayer, textLayer;
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            dimLayer = e.common.layer;
        if (entityAs<TextData>(e))
            textLayer = e.common.layer;
    });
    CHECK(dimLayer == f.a);
    CHECK(textLayer == f.b);
    f.host.run("DIMLAYER", {"nope"});
    CHECK(f.host.logContains("There is no layer named"));
    f.host.run("DIMLAYER", {"."});
    f.host.run("DIMLINEAR", {"0,0", "50,0", "25,10"});
    int onCurrent = 0;
    f.doc.forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e) && e.common.layer == f.doc.header().currentLayer)
            ++onCurrent;
    });
    CHECK(onCurrent == 1);
}

TEST_CASE("LAYTRANS renames, merges and restyles layers from a CSV standard; Export writes one") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tbcad_laytrans";
    fs::create_directories(dir);
    Fixture f;
    f.setup();  // layers A (with ea) and B (with eb)
    const std::string std_ = (dir / "std.csv").string();
    {
        std::ofstream o(std_, std::ios::binary);
        o << "\xEF\xBB\xBF" << "Old,New,Color,Linetype,Lineweight\r\n"
          << "A,A-WALL,red,,\r\n"
          << "B,A-WALL,,,\r\n"
          << "Missing,Whatever,,,\r\n";
    }
    CHECK(f.run("LAYTRANS", {"Load", std_}).finished);
    const Layer* wall = f.doc.layerByName("A-WALL");
    REQUIRE(wall);
    CHECK(f.doc.layerByName("A") == nullptr);
    CHECK(f.doc.layerByName("B") == nullptr);
    CHECK(f.doc.find(f.ea)->common.layer == wall->id);
    CHECK(f.doc.find(f.eb)->common.layer == wall->id);
    CHECK(wall->color == Color::index(1));
    CHECK(f.host.logContains("1 renamed, 1 merged"));
    CHECK(f.host.undo());
    CHECK(f.doc.layerByName("A") != nullptr);
    CHECK(f.doc.layerByName("B") != nullptr);

    const std::string out = (dir / "out.csv").string();
    CHECK(f.run("LAYTRANS", {"Export", out}).finished);
    std::ifstream in(out, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    CHECK(buf.str().find("A,A,") != std::string::npos);
    fs::remove_all(dir);
}
