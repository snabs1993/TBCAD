#include <doctest/doctest.h>

#include "tbcad/layers/layer_ops.h"

using namespace tbcad;
using namespace tbcad::layers;

namespace {

LayerId mk(Document& doc, const std::string& name, LayerId parent = {}) {
    auto tx = doc.begin("make layer");
    auto r = createLayer(doc, tx, name, parent);
    REQUIRE(r.ok());
    tx.commit();
    return r.value();
}

EntityId lineOn(Document& doc, LayerId layer) {
    auto tx = doc.begin("line");
    Entity e{{}, LineData{{0, 0}, {1, 0}}};
    e.common.layer = layer;
    auto id = tx.add(std::move(e));
    REQUIRE(id.ok());
    tx.commit();
    return id.value();
}

}  // namespace

TEST_CASE("sub-layers get ByParent defaults and paths") {
    Document doc;
    LayerId floor = mk(doc, "S-FLOOR");
    LayerId hidden = mk(doc, "Floor hidden", floor);
    LayerId edge = mk(doc, "edge", floor);
    CHECK(doc.layer(floor)->linetype == kLinetypeContinuous);
    CHECK(doc.layer(hidden)->color.kind == ColorKind::ByParent);
    CHECK(doc.layer(hidden)->linetype == kLinetypeByParent);
    CHECK(doc.layer(hidden)->lineweight.isByParent());
    CHECK(layerPath(doc, edge) == "S-FLOOR~edge");
    CHECK(layerDepth(doc, edge) == 1);
    CHECK(layerDepth(doc, floor) == 0);

    CHECK(resolveLayer(doc, "S-FLOOR~edge").value() == edge);
    CHECK(resolveLayer(doc, "s-floor~EDGE").value() == edge);
    CHECK(resolveLayer(doc, "edge").value() == edge);  // unique sub-layer by bare name
    CHECK(resolveLayer(doc, "#" + std::to_string(edge.value)).value() == edge);
    CHECK_FALSE(resolveLayer(doc, "nope").ok());
    CHECK_FALSE(resolveLayer(doc, "S-FLOOR~nope").ok());
}

TEST_CASE("sibling names are unique, layer 0 takes no children, depth is limited to 8") {
    Document doc;
    LayerId a = mk(doc, "A");
    mk(doc, "x", a);
    {
        auto tx = doc.begin("dup");
        CHECK_FALSE(createLayer(doc, tx, "X", a).ok());  // same name, same parent
        CHECK(createLayer(doc, tx, "x", {}).ok());       // same name elsewhere is fine
        tx.rollback();
    }
    {
        auto tx = doc.begin("zero");
        CHECK_FALSE(createLayer(doc, tx, "child", kLayerZero).ok());
        tx.rollback();
    }
    // A top-level layer plus seven sub-layers is eight levels; the ninth is refused.
    LayerId p = a;
    for (int depth = 1; depth <= 7; ++depth)
        p = mk(doc, "d" + std::to_string(depth), p);
    CHECK(layerDepth(doc, p) == 7);
    auto tx = doc.begin("too deep");
    CHECK_FALSE(createLayer(doc, tx, "d8", p).ok());
    tx.rollback();
}

TEST_CASE("effective state: on/off, freeze, lock, no-plot are inherited along the branch") {
    Document doc;
    LayerId parent = mk(doc, "S-FLOOR");
    LayerId child = mk(doc, "edge", parent);
    LayerId grand = mk(doc, "fine", child);

    {
        auto tx = doc.begin("off");
        REQUIRE(setOn(doc, tx, parent, false).ok());
        tx.commit();
    }
    CHECK_FALSE(doc.effective(child).on);
    CHECK_FALSE(doc.effective(grand).visible());
    CHECK(doc.layer(child)->on);  // the child keeps its own switch
    {
        auto tx = doc.begin("child off too");
        REQUIRE(setOn(doc, tx, child, false).ok());
        REQUIRE(setOn(doc, tx, parent, true).ok());
        tx.commit();
    }
    CHECK_FALSE(doc.effective(child).on);  // parent back on, child's own switch still off
    CHECK_FALSE(doc.effective(grand).on);
    {
        auto tx = doc.begin("child on");
        REQUIRE(setOn(doc, tx, child, true).ok());
        tx.commit();
    }
    CHECK(doc.effective(grand).visible());

    {
        auto tx = doc.begin("flags");
        REQUIRE(setFrozen(doc, tx, parent, true).ok());
        REQUIRE(setLocked(doc, tx, child, true).ok());
        REQUIRE(setPlot(doc, tx, parent, false).ok());
        tx.commit();
    }
    CHECK(doc.effective(grand).frozen);
    CHECK_FALSE(doc.effective(grand).visible());
    CHECK(doc.effective(grand).locked);
    CHECK_FALSE(doc.effective(grand).plot);
    CHECK_FALSE(doc.effective(parent).locked);
    CHECK(doc.effective(parent).frozen);
}

TEST_CASE("effective look: ByParent follows the chain, own values win") {
    Document doc;
    LayerId parent = mk(doc, "P");
    LayerId child = mk(doc, "C", parent);
    LayerId grand = mk(doc, "G", child);
    {
        auto tx = doc.begin("look");
        REQUIRE(setLayerColor(doc, tx, parent, Color::index(1)).ok());
        REQUIRE(setLayerLineweight(doc, tx, parent, LineWeight::mm(0.5)).ok());
        tx.commit();
    }
    CHECK(doc.effective(grand).color == Color::index(1));
    CHECK(doc.effective(grand).lineweight == LineWeight::mm(0.5));
    CHECK(doc.effective(grand).linetype == kLinetypeContinuous);
    {
        auto tx = doc.begin("own");
        REQUIRE(setLayerColor(doc, tx, child, Color::rgb(1, 2, 3)).ok());
        tx.commit();
    }
    CHECK(doc.effective(child).color == Color::rgb(1, 2, 3));
    CHECK(doc.effective(grand).color == Color::rgb(1, 2, 3));
    CHECK(doc.effective(parent).color == Color::index(1));
    {
        auto tx = doc.begin("bad");
        CHECK_FALSE(setLayerColor(doc, tx, parent, Color::byLayer()).ok());
        CHECK_FALSE(setLayerColor(doc, tx, parent, Color::byParent()).ok());  // top level
        CHECK(setLayerColor(doc, tx, child, Color::byParent()).ok());
        tx.rollback();
    }
}

TEST_CASE("re-parenting refuses cycles, layer 0, name clashes and depth overflow") {
    Document doc;
    LayerId a = mk(doc, "A");
    LayerId b = mk(doc, "B", a);
    LayerId c = mk(doc, "C", b);
    LayerId other = mk(doc, "B");  // top-level B next to A
    {
        auto tx = doc.begin("cycle");
        CHECK_FALSE(reparentLayer(doc, tx, a, c).ok());
        CHECK_FALSE(reparentLayer(doc, tx, a, a).ok());
        CHECK_FALSE(reparentLayer(doc, tx, kLayerZero, a).ok());
        CHECK_FALSE(reparentLayer(doc, tx, b, kLayerZero).ok());
        CHECK_FALSE(reparentLayer(doc, tx, other, a).ok());  // A already has a child called B
        tx.rollback();
    }
    CHECK(layerPath(doc, c) == "A~B~C");
    {
        auto tx = doc.begin("move");
        CHECK(reparentLayer(doc, tx, c, a).ok());
        tx.commit();
    }
    CHECK(layerPath(doc, c) == "A~C");
    {
        auto tx = doc.begin("top");
        CHECK(reparentLayer(doc, tx, c, {}).ok());
        tx.commit();
    }
    CHECK(layerPath(doc, c) == "C");

    // A chain 8 levels deep: moving a deeper subtree under its bottom must fail.
    LayerId bottom = mk(doc, "L0");
    for (int i = 1; i < 8; ++i)
        bottom = mk(doc, "L" + std::to_string(i), bottom);
    LayerId extra = mk(doc, "extra");
    mk(doc, "extra-child", extra);
    auto tx = doc.begin("deep");
    CHECK_FALSE(reparentLayer(doc, tx, extra, bottom).ok());  // would make 8 + 2 levels
    tx.rollback();
}

TEST_CASE("rename a parent: children keep their names, their paths change") {
    Document doc;
    LayerId p = mk(doc, "OLD");
    LayerId c = mk(doc, "child", p);
    {
        auto tx = doc.begin("rename");
        REQUIRE(renameLayer(doc, tx, p, "NEW").ok());
        tx.commit();
    }
    CHECK(doc.layer(c)->name == "child");
    CHECK(layerPath(doc, c) == "NEW~child");
    CHECK(resolveLayer(doc, "NEW~child").value() == c);
    CHECK_FALSE(resolveLayer(doc, "OLD~child").ok());
    auto tx = doc.begin("rename0");
    CHECK_FALSE(renameLayer(doc, tx, kLayerZero, "Zero").ok());
    tx.rollback();
}

TEST_CASE("delete: layer 0, current layer, layers in use and layers with children are refused") {
    Document doc;
    LayerId p = mk(doc, "P");
    LayerId c = mk(doc, "C", p);
    lineOn(doc, c);

    auto tx = doc.begin("delete");
    auto zero = deleteLayer(doc, tx, kLayerZero, DeleteMode::LeafOnly);
    REQUIRE_FALSE(zero.ok());
    CHECK(zero.error().message.find("Layer 0") != std::string::npos);

    auto used = deleteLayer(doc, tx, c, DeleteMode::LeafOnly);
    REQUIRE_FALSE(used.ok());
    CHECK(used.error().message.find("1 object") != std::string::npos);

    auto parent = deleteLayer(doc, tx, p, DeleteMode::LeafOnly);
    REQUIRE_FALSE(parent.ok());
    CHECK(parent.error().message.find("sub-layers") != std::string::npos);

    // Branch delete needs every layer in the branch to be empty.
    CHECK_FALSE(deleteLayer(doc, tx, p, DeleteMode::Branch).ok());
    CHECK(doc.layer(p) != nullptr);
    CHECK(doc.layer(c) != nullptr);
    tx.rollback();
}

TEST_CASE("delete the current layer is refused, even inside a branch") {
    Document doc;
    LayerId p = mk(doc, "P");
    LayerId c = mk(doc, "C", p);
    {
        auto tx = doc.begin("current");
        REQUIRE(setCurrentLayer(doc, tx, c).ok());
        tx.commit();
    }
    auto tx = doc.begin("delete");
    CHECK_FALSE(deleteLayer(doc, tx, c, DeleteMode::LeafOnly).ok());
    CHECK_FALSE(deleteLayer(doc, tx, p, DeleteMode::Branch).ok());
    CHECK_FALSE(setFrozen(doc, tx, p, true).ok());  // the current layer sits inside the branch
    CHECK(setFrozen(doc, tx, p, false).ok());
    tx.rollback();
}

TEST_CASE("delete a whole branch of empty layers") {
    Document doc;
    LayerId p = mk(doc, "P");
    LayerId c = mk(doc, "C", p);
    mk(doc, "G", c);
    mk(doc, "Sibling");
    const std::size_t before = doc.layers().size();
    {
        auto tx = doc.begin("branch");
        REQUIRE(deleteLayer(doc, tx, p, DeleteMode::Branch).ok());
        tx.commit();
    }
    CHECK(doc.layers().size() == before - 3);
    CHECK(doc.layerByName("Sibling") != nullptr);
}

TEST_CASE("delete a layer and move its children up; they keep their look") {
    Document doc;
    LayerId top = mk(doc, "Top");
    LayerId mid = mk(doc, "Mid", top);
    LayerId inherit = mk(doc, "inherits", mid);
    LayerId own = mk(doc, "own", mid);
    {
        auto tx = doc.begin("look");
        REQUIRE(setLayerColor(doc, tx, top, Color::index(1)).ok());
        REQUIRE(setLayerColor(doc, tx, mid, Color::index(3)).ok());
        REQUIRE(setLayerColor(doc, tx, own, Color::index(5)).ok());
        tx.commit();
    }
    REQUIRE(doc.effective(inherit).color == Color::index(3));
    {
        auto tx = doc.begin("move up");
        REQUIRE(deleteLayer(doc, tx, mid, DeleteMode::MoveChildrenUp).ok());
        tx.commit();
    }
    CHECK(doc.layer(mid) == nullptr);
    CHECK(layerPath(doc, inherit) == "Top~inherits");
    CHECK(layerPath(doc, own) == "Top~own");
    CHECK(doc.effective(inherit).color == Color::index(3));  // baked from the deleted parent
    CHECK(doc.effective(own).color == Color::index(5));
}

TEST_CASE("move children up refuses a name clash and changes nothing") {
    Document doc;
    LayerId top = mk(doc, "Top");
    LayerId mid = mk(doc, "Mid", top);
    mk(doc, "dup", top);
    mk(doc, "dup", mid);
    auto tx = doc.begin("move up");
    CHECK_FALSE(deleteLayer(doc, tx, mid, DeleteMode::MoveChildrenUp).ok());
    tx.rollback();
    CHECK(doc.layer(mid) != nullptr);
}

TEST_CASE("isolate and show all") {
    Document doc;
    LayerId a = mk(doc, "A");
    LayerId a1 = mk(doc, "A1", a);
    LayerId b = mk(doc, "B");
    {
        auto tx = doc.begin("isolate");
        REQUIRE(isolateLayer(doc, tx, a1).ok());
        tx.commit();
    }
    CHECK(doc.effective(a1).visible());
    CHECK(doc.layer(a)->on);  // ancestors stay on
    CHECK_FALSE(doc.layer(b)->on);
    CHECK_FALSE(doc.layer(kLayerZero)->on);
    {
        auto tx = doc.begin("all");
        REQUIRE(showAllLayers(doc, tx).ok());
        tx.commit();
    }
    CHECK(doc.layer(b)->on);
    CHECK(doc.layer(kLayerZero)->on);
}

TEST_CASE("layer list references: commas, star, ids") {
    Document doc;
    LayerId a = mk(doc, "A");
    LayerId b = mk(doc, "B");
    auto two = resolveLayers(doc, "A, B");
    REQUIRE(two.ok());
    CHECK(two.value().size() == 2);
    CHECK(resolveLayers(doc, "*").value().size() == doc.layers().size());
    CHECK_FALSE(resolveLayers(doc, "A,nope").ok());
    CHECK(resolveLayers(doc, "A,A").value().size() == 1);
    (void)a;
    (void)b;
}

TEST_CASE("tree order lists parents before children, sorted by name") {
    Document doc;
    LayerId b = mk(doc, "b");
    LayerId a = mk(doc, "a");
    LayerId a2 = mk(doc, "z", a);
    LayerId a1 = mk(doc, "y", a);
    auto order = layerTreeOrder(doc);
    REQUIRE(order.size() == 5);
    CHECK(order[0] == kLayerZero);
    CHECK(order[1] == a);
    CHECK(order[2] == a1);
    CHECK(order[3] == a2);
    CHECK(order[4] == b);
}

TEST_CASE("text parsing helpers") {
    CHECK(parseColor("red").value() == Color::index(1));
    CHECK(parseColor("WHITE").value() == Color::index(7));
    CHECK(parseColor("40").value() == Color::index(40));
    CHECK(parseColor("10,20,30").value() == Color::rgb(10, 20, 30));
    CHECK(parseColor("ByParent").value().kind == ColorKind::ByParent);
    CHECK_FALSE(parseColor("256").ok());
    CHECK_FALSE(parseColor("1,2").ok());
    CHECK_FALSE(parseColor("1,2,300").ok());
    CHECK_FALSE(parseColor("mauve").ok());
    CHECK(colorName(Color::index(2)) == "yellow");
    CHECK(colorName(Color::rgb(1, 2, 3)) == "1,2,3");

    CHECK(parseLineweight("0.35").value() == LineWeight::mm(0.35));
    CHECK(parseLineweight("0.5mm").value() == LineWeight::mm(0.5));
    CHECK(parseLineweight("default").value().isDefault());
    CHECK(parseLineweight("ByParent").value().isByParent());
    CHECK_FALSE(parseLineweight("-1").ok());
    CHECK_FALSE(parseLineweight("9").ok());
    CHECK(lineweightName(LineWeight::mm(0.25)) == "0.25 mm");
}

TEST_CASE("whyCannotReparent explains each refusal and agrees with reparentLayer") {
    Document doc;
    LayerId a = mk(doc, "A");
    LayerId b = mk(doc, "B", a);
    LayerId c = mk(doc, "C", b);
    mk(doc, "B");
    LayerId d = mk(doc, "D");
    CHECK(whyCannotReparent(doc, a, c).find("cycle") != std::string::npos);
    CHECK(whyCannotReparent(doc, kLayerZero, a).find("Layer 0") != std::string::npos);
    CHECK(whyCannotReparent(doc, d, kLayerZero).find("Layer 0") != std::string::npos);
    CHECK(whyCannotReparent(doc, c, b).find("already") != std::string::npos);
    CHECK(whyCannotReparent(doc, doc.layerByName("B")->id, a).find("already exists") !=
          std::string::npos);
    CHECK(whyCannotReparent(doc, d, a).empty());
    CHECK(whyCannotReparent(doc, c, {}).empty());

    LayerId bottom = mk(doc, "L0");
    for (int i = 1; i < 8; ++i)
        bottom = mk(doc, "L" + std::to_string(i), bottom);
    CHECK(whyCannotReparent(doc, d, bottom).find("8 levels") != std::string::npos);
    LayerId deepParent = bottom;
    // One level above the bottom still has room for a leaf only if it is not the 9th level.
    auto tx = doc.begin("check");
    CHECK_FALSE(reparentLayer(doc, tx, d, deepParent).ok());
    tx.rollback();
}
