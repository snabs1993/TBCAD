#include "support.h"

using namespace tbcad;
using namespace tbcad::blocks;
using namespace tbcad::blocks::test;

namespace {

struct Frame {
    Document doc;
    BlockId block{};
    EntityId bottom{}, right{}, top{}, left{}, hole{};

    Frame() {
        Transaction tx = doc.begin("frame");
        block = tx.addBlock(BlockDef{.name = "Frame"}).value();
        bottom = tx.add(line(0, 0, 10, 0), block).value();
        right = tx.add(line(10, 0, 10, 5), block).value();
        top = tx.add(line(10, 5, 0, 5), block).value();
        left = tx.add(line(0, 5, 0, 0), block).value();
        hole = tx.add(circle(5, 2.5, 1), block).value();
        tx.commit();
    }

    // "Width" stretches the right side and moves the hole half way; "Open" hides the hole.
    void makeWidth() {
        Transaction tx = doc.begin("dyn");
        REQUIRE(tx.modifyBlock(block, [&](BlockDef& d) {
                      DynamicDef def;
                      DynParam w;
                      w.name = "Width";
                      w.base = {0, 0};
                      w.end = {10, 0};
                      def.params.push_back(w);
                      DynAction stretch;
                      stretch.kind = DynAction::Kind::Stretch;
                      stretch.frameMin = {9, -1};
                      stretch.frameMax = {11, 6};
                      stretch.objects = {bottom, right, top};
                      def.actions.push_back(stretch);
                      DynAction move;
                      move.kind = DynAction::Kind::Move;
                      move.objects = {hole};
                      def.actions.push_back(move);
                      d.dynamic = std::move(def);
                  }).ok());
        tx.commit();
    }

    EntityId insert(std::vector<double> values = {}, Vec2 at = {}) {
        Entity e = insertOf(block, at);
        entityAs<InsertData>(e)->dynValues = std::move(values);
        return addEntity(doc, std::move(e));
    }
};

const LineData& lineOf(const std::vector<Entity>& v, std::size_t i) {
    return *entityAs<LineData>(v.at(i));
}

}  // namespace

TEST_CASE("dynamic: default values reproduce the block") {
    Frame f;
    f.makeWidth();
    const BlockDef& def = *f.doc.block(f.block);
    CHECK(dynValues(def, InsertData{}) == std::vector<double>{10});
    auto items = evaluateDynamic(f.doc, def, {10});
    REQUIRE(items.size() == 5);
    CHECK(near(lineOf(items, 1).b, {10, 5}));
}

TEST_CASE("dynamic: a linear parameter stretches the points in the frame and moves objects") {
    Frame f;
    f.makeWidth();
    auto items = evaluateDynamic(f.doc, *f.doc.block(f.block), {14});
    REQUIRE(items.size() == 5);
    CHECK(near(lineOf(items, 0).a, {0, 0}));
    CHECK(near(lineOf(items, 0).b, {14, 0}));
    CHECK(near(lineOf(items, 1).a, {14, 0}));
    CHECK(near(lineOf(items, 1).b, {14, 5}));
    CHECK(near(lineOf(items, 2).a, {14, 5}));
    CHECK(near(lineOf(items, 2).b, {0, 5}));
    CHECK(near(lineOf(items, 3).a, {0, 5}));  // not in the action
    CHECK(near(entityAs<CircleData>(items[4])->center, {9, 2.5}));
}

TEST_CASE("dynamic: limits clamp the value") {
    Frame f;
    f.makeWidth();
    {
        Transaction tx = f.doc.begin("limits");
        REQUIRE(tx.modifyBlock(f.block, [](BlockDef& d) {
                      d.dynamic->params[0].min = 5;
                      d.dynamic->params[0].max = 20;
                  }).ok());
        tx.commit();
    }
    InsertData ins;
    ins.dynValues = {100};
    CHECK(dynValues(*f.doc.block(f.block), ins) == std::vector<double>{20});
    ins.dynValues = {1};
    CHECK(dynValues(*f.doc.block(f.block), ins) == std::vector<double>{5});
}

TEST_CASE("dynamic: rotation and flip parameters") {
    Frame f;
    {
        Transaction tx = f.doc.begin("dyn");
        REQUIRE(tx.modifyBlock(f.block, [&](BlockDef& d) {
                      DynamicDef def;
                      DynParam r;
                      r.kind = DynParam::Kind::Rotation;
                      r.name = "Angle";
                      r.base = {0, 0};
                      r.end = {10, 0};
                      def.params.push_back(r);
                      DynParam m;
                      m.kind = DynParam::Kind::Flip;
                      m.name = "Flip";
                      m.base = {0, 0};
                      m.end = {0, 1};  // mirror in the Y axis
                      def.params.push_back(m);
                      DynAction rot;
                      rot.kind = DynAction::Kind::Rotate;
                      rot.param = 0;
                      rot.objects = {f.hole};
                      def.actions.push_back(rot);
                      DynAction flip;
                      flip.kind = DynAction::Kind::Flip;
                      flip.param = 1;
                      flip.objects = {f.right};
                      def.actions.push_back(flip);
                      d.dynamic = std::move(def);
                  }).ok());
        tx.commit();
    }
    auto items = evaluateDynamic(f.doc, *f.doc.block(f.block), {kPi, 1});
    REQUIRE(items.size() == 5);
    CHECK(near(entityAs<CircleData>(items[4])->center, {-5, -2.5}));
    CHECK(near(lineOf(items, 1).a, {-10, 0}));
    CHECK(near(lineOf(items, 1).b, {-10, 5}));
    // flip 0 leaves it alone
    items = evaluateDynamic(f.doc, *f.doc.block(f.block), {0, 0});
    CHECK(near(lineOf(items, 1).a, {10, 0}));
}

TEST_CASE("dynamic: a visibility parameter shows one state") {
    Frame f;
    {
        Transaction tx = f.doc.begin("dyn");
        REQUIRE(tx.modifyBlock(f.block, [&](BlockDef& d) {
                      DynamicDef def;
                      DynParam v;
                      v.kind = DynParam::Kind::Visibility;
                      v.name = "Style";
                      v.states = {"Open", "Closed"};
                      v.stateObjects = {{f.hole}, {f.bottom}};
                      def.params.push_back(v);
                      d.dynamic = std::move(def);
                  }).ok());
        tx.commit();
    }
    const BlockDef& def = *f.doc.block(f.block);
    auto open = evaluateDynamic(f.doc, def, {0});
    CHECK(open.size() == 4);  // bottom hidden
    auto closed = evaluateDynamic(f.doc, def, {1});
    CHECK(closed.size() == 4);  // hole hidden
    for (const Entity& e : closed)
        CHECK(kind(e) == EntityKind::Line);
}

TEST_CASE("dynamic: display, bounds and explode use the insert's values") {
    registerInsertDisplay();
    Frame f;
    f.makeWidth();
    EntityId plain = f.insert({}, {100, 0});
    EntityId wide = f.insert({14}, {100, 20});
    BBox2 a = insertBounds(f.doc, *f.doc.find(plain));
    BBox2 b = insertBounds(f.doc, *f.doc.find(wide));
    CHECK(near(a.max, {110, 5}));
    CHECK(near(b.max, {114, 25}));
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.1;
    DisplayList l;
    geom::buildDisplay(f.doc, *f.doc.find(wide), ctx, l);
    CHECK(l.items.size() == 5);
    auto ex = explode(f.doc, *f.doc.find(wide));
    REQUIRE(ex.ok());
    REQUIRE(ex.value().entities.size() == 5);
    CHECK(near(entityAs<LineData>(ex.value().entities[0])->b, {114, 20}));
}

TEST_CASE("dynamic: handles, dragging and setting values") {
    Frame f;
    f.makeWidth();
    EntityId id = f.insert({}, {100, 0});
    auto handles = dynHandles(f.doc, *f.doc.find(id));
    REQUIRE(handles.size() == 1);
    CHECK(near(handles[0].world, {110, 0}));
    auto dragged = dynDrag(f.doc, *f.doc.find(id), 0, {117, 3});
    REQUIRE(dragged.ok());
    CHECK(entityAs<InsertData>(dragged.value())->dynValues == std::vector<double>{17});
    auto set = dynSet(f.doc, *f.doc.find(id), 0, 12);
    REQUIRE(set.ok());
    CHECK(entityAs<InsertData>(set.value())->dynValues == std::vector<double>{12});
    CHECK_FALSE(dynSet(f.doc, *f.doc.find(id), 3, 1).ok());
    CHECK(dynFind(*f.doc.block(f.block)->dynamic, "width") == 0);
    CHECK(dynFind(*f.doc.block(f.block)->dynamic, "nope") == -1);
}

TEST_CASE("dynamic: an ordinary block is unaffected") {
    Frame f;
    EntityId id = f.insert();
    CHECK(dynHandles(f.doc, *f.doc.find(id)).empty());
    CHECK_FALSE(dynSet(f.doc, *f.doc.find(id), 0, 1).ok());
    std::vector<Entity> storage;
    auto items = blockItems(f.doc, *f.doc.block(f.block), nullptr, storage);
    CHECK(items.size() == 5);
    CHECK(storage.empty());
}

TEST_CASE("dynamic: flip and visibility parameters have click-only grips") {
    Frame f;
    {
        Transaction tx = f.doc.begin("dyn");
        REQUIRE(tx.modifyBlock(f.block, [&](BlockDef& d) {
                      DynamicDef def;
                      DynParam fl;
                      fl.kind = DynParam::Kind::Flip;
                      fl.name = "Flip";
                      fl.base = {5, 0};
                      fl.end = {5, 5};
                      def.params.push_back(fl);
                      DynParam v;
                      v.kind = DynParam::Kind::Visibility;
                      v.name = "Style";
                      v.base = {0, 8};
                      v.states = {"A", "B", "C"};
                      v.stateObjects = {{f.hole}, {f.bottom}, {f.top}};
                      def.params.push_back(v);
                      d.dynamic = std::move(def);
                  }).ok());
        tx.commit();
    }
    EntityId id = f.insert({}, {100, 0});
    auto handles = dynHandles(f.doc, *f.doc.find(id));
    REQUIRE(handles.size() == 2);
    CHECK(near(handles[0].world, {105, 2.5}));  // middle of the mirror axis
    CHECK(near(handles[1].world, {100, 8}));
    CHECK(dynGripIsClick(f.doc, *f.doc.find(id), 0));
    CHECK(dynGripIsClick(f.doc, *f.doc.find(id), 1));
    // Each use flips, or steps to the next state and wraps round after the last.
    auto a = dynDrag(f.doc, *f.doc.find(id), 0, {0, 0});
    REQUIRE(a.ok());
    CHECK(entityAs<InsertData>(a.value())->dynValues[0] == 1);
    auto b = dynDrag(f.doc, a.value(), 0, {0, 0});
    REQUIRE(b.ok());
    CHECK(entityAs<InsertData>(b.value())->dynValues[0] == 0);
    Entity cur = *f.doc.find(id);
    for (double want : {1.0, 2.0, 0.0}) {
        auto n = dynDrag(f.doc, cur, 1, {0, 0});
        REQUIRE(n.ok());
        CHECK(entityAs<InsertData>(n.value())->dynValues[1] == want);
        cur = n.value();
    }
}
