#include "support.h"

using namespace tbcad;
using namespace tbcad::blocks;
using namespace tbcad::blocks::test;

namespace {

AsmParam number(const char* name, double def, double lo = 0, double hi = 0) {
    AsmParam p;
    p.name = name;
    p.defaultNumber = def;
    p.min = lo;
    p.max = hi;
    return p;
}

AsmBinding bind(AsmBinding::Kind kind, const char* target, const char* formula, EntityId e = {}) {
    AsmBinding b;
    b.kind = kind;
    b.target = target;
    b.formula = formula;
    b.entity = e;
    return b;
}

// "Bar": a line that stretches with the dynamic parameter Length; assembly parameter L feeds it.
struct Bar {
    Document doc;
    BlockId block{};
    EntityId line{};

    Bar() {
        Transaction tx = doc.begin("bar");
        block = tx.addBlock(BlockDef{.name = "Bar"}).value();
        line = tx.add(test::line(0, 0, 10, 0), block).value();
        DynamicDef dyn;
        DynParam len;
        len.name = "Length";
        len.base = {0, 0};
        len.end = {10, 0};
        dyn.params.push_back(len);
        DynAction stretch;
        stretch.kind = DynAction::Kind::Stretch;
        stretch.frameMin = {9, -1};
        stretch.frameMax = {11, 1};
        stretch.objects = {line};
        dyn.actions.push_back(stretch);
        AssemblyDef as;
        as.params.push_back(number("L", 10, 1, 100));
        AsmParam label;
        label.kind = AsmParam::Kind::Text;
        label.name = "Label";
        label.defaultText = "bar";
        as.params.push_back(label);
        AsmParam mat;
        mat.kind = AsmParam::Kind::Choice;
        mat.name = "Material";
        mat.choices = {"Steel", "Oak"};
        as.params.push_back(mat);
        as.bindings.push_back(bind(AsmBinding::Kind::DynParam, "Length", "L * 2"));
        AsmPreset big;
        big.name = "Big";
        big.values = {AsmValue{"L", false, 50, ""}, AsmValue{"Material", true, 0, "Oak"}};
        as.presets.push_back(big);
        REQUIRE(tx.modifyBlock(block, [&](BlockDef& d) {
                      d.dynamic = std::move(dyn);
                      d.assembly = std::move(as);
                  }).ok());
        tx.commit();
    }

    EntityId insert(std::vector<AsmValue> values = {}, Vec2 at = {}) {
        Entity e = insertOf(block, at);
        entityAs<InsertData>(e)->asmValues = std::move(values);
        return addEntity(doc, std::move(e));
    }
};

}  // namespace

TEST_CASE("assembly: defaults, clamping and effective values") {
    Bar b;
    const AssemblyDef& as = *b.doc.block(b.block)->assembly;
    InsertData none;
    auto v = asmEffectiveValues(as, none);
    REQUIRE(v.size() == 3);
    CHECK(v[0].number == 10);
    CHECK(v[1].text == "bar");
    CHECK(v[2].text == "Steel");  // first choice
    InsertData big;
    big.asmValues = {AsmValue{"l", false, 500, ""}, AsmValue{"Material", true, 0, "Glass"}};
    v = asmEffectiveValues(as, big);
    CHECK(v[0].number == 100);    // clamped, name matched without case
    CHECK(v[2].text == "Steel");  // not offered
    CHECK(asmFind(as, "LABEL") == 1);
    CHECK(asmFind(as, "nope") == -1);
}

TEST_CASE("assembly: a formula drives the dynamic parameter") {
    Bar b;
    EntityId id = b.insert({AsmValue{"L", false, 7, ""}}, {100, 0});
    const BlockDef& def = *b.doc.block(b.block);
    auto res = resolveAssembly(b.doc, def, *entityAs<InsertData>(*b.doc.find(id)));
    CHECK(res.errors.empty());
    CHECK(res.insert.dynValues == std::vector<double>{14});
    // bounds and explode show the stretched line, the default insert the plain one
    BBox2 box = insertBounds(b.doc, *b.doc.find(id));
    CHECK(near(box.max, {114, 0}));
    CHECK(near(insertBounds(b.doc, *b.doc.find(b.insert())).max, {20, 0}));  // L = 10 -> 20
    auto ex = explode(b.doc, *b.doc.find(id));
    REQUIRE(ex.ok());
    REQUIRE(ex.value().entities.size() == 1);
    CHECK(near(entityAs<LineData>(ex.value().entities[0])->b, {114, 0}));
}

TEST_CASE("assembly: a formula that fails is reported and skipped") {
    Bar b;
    {
        Transaction tx = b.doc.begin("bad");
        REQUIRE(tx.modifyBlock(b.block, [](BlockDef& d) {
                      d.assembly->bindings.push_back(
                          bind(AsmBinding::Kind::DynParam, "Length", "L / 0"));
                  }).ok());
        tx.commit();
    }
    const BlockDef& def = *b.doc.block(b.block);
    auto res = resolveAssembly(b.doc, def, InsertData{});
    CHECK(res.errors.size() == 1);
    CHECK(res.insert.dynValues == std::vector<double>{20});  // the first binding still applied
    auto problems = asmValidate(b.doc, def);
    CHECK(problems.empty());  // division by zero is a value error, not a definition error
}

TEST_CASE("assembly: validate finds bad names, targets and formulas") {
    Bar b;
    {
        Transaction tx = b.doc.begin("bad");
        REQUIRE(tx.modifyBlock(b.block, [](BlockDef& d) {
                      d.assembly->bindings.push_back(bind(AsmBinding::Kind::DynParam, "Nope", "L"));
                      d.assembly->bindings.push_back(
                          bind(AsmBinding::Kind::DynParam, "Length", "Q + 1"));
                      d.assembly->bindings.push_back(
                          bind(AsmBinding::Kind::Attribute, "TAG", "L +"));
                      d.assembly->params.push_back(number("l", 1));
                  }).ok());
        tx.commit();
    }
    auto problems = asmValidate(b.doc, *b.doc.block(b.block));
    CHECK(problems.size() == 4);
}

TEST_CASE("assembly: asmSet validates, presets apply") {
    Bar b;
    EntityId id = b.insert();
    const Entity& e = *b.doc.find(id);
    auto r = asmSet(b.doc, e, "L", expr::Value::ofNumber(30));
    REQUIRE(r.ok());
    CHECK(asmEffectiveValues(*b.doc.block(b.block)->assembly, *entityAs<InsertData>(r.value()))[0]
              .number == 30);
    CHECK_FALSE(asmSet(b.doc, e, "L", expr::Value::ofNumber(500)).ok());  // above the limit
    CHECK_FALSE(asmSet(b.doc, e, "Material", expr::Value::ofText("Glass")).ok());
    CHECK_FALSE(asmSet(b.doc, e, "Nope", expr::Value::ofNumber(1)).ok());
    CHECK_FALSE(asmSet(b.doc, e, "L", expr::Value::ofText("abc")).ok());
    auto t = asmSet(b.doc, e, "Label", expr::Value::ofNumber(12));
    REQUIRE(t.ok());
    CHECK(asmEffectiveValues(*b.doc.block(b.block)->assembly, *entityAs<InsertData>(t.value()))[1]
              .text == "12");
    auto preset = asmApplyPreset(b.doc, e, "big");
    REQUIRE(preset.ok());
    auto v =
        asmEffectiveValues(*b.doc.block(b.block)->assembly, *entityAs<InsertData>(preset.value()));
    CHECK(v[0].number == 50);
    CHECK(v[2].text == "Oak");
    CHECK_FALSE(asmApplyPreset(b.doc, e, "none").ok());
}

TEST_CASE("assembly: nested inserts take parameters and array sizes from the outer formulas") {
    Bar b;
    BlockId room, fence, post;
    EntityId inner, arr;
    {
        Transaction tx = b.doc.begin("room");
        room = tx.addBlock(BlockDef{.name = "Room"}).value();
        Entity ins = insertOf(b.block, {0, 0});
        inner = tx.add(std::move(ins), room).value();
        post = tx.addBlock(BlockDef{.name = "Post"}).value();
        REQUIRE(tx.add(circle(0, 0, 1), post).ok());
        fence = tx.addBlock(BlockDef{.name = "Fence"}).value();
        Entity row = insertOf(post, {0, 0});
        ArrayInfo a;
        a.columns = 2;
        a.columnSpacing = 5;
        entityAs<InsertData>(row)->array = a;
        arr = tx.add(std::move(row), fence).value();
        AssemblyDef r;
        r.params.push_back(number("W", 20, 2, 200));
        r.bindings.push_back(bind(AsmBinding::Kind::Nested, "L", "W / 2", inner));
        REQUIRE(tx.modifyBlock(room, [&](BlockDef& d) { d.assembly = std::move(r); }).ok());
        AssemblyDef f;
        AsmParam n;
        n.kind = AsmParam::Kind::Integer;
        n.name = "Posts";
        n.defaultNumber = 2;
        n.min = 1;
        n.max = 50;
        f.params.push_back(n);
        f.params.push_back(number("Gap", 5));
        f.bindings.push_back(bind(AsmBinding::Kind::ArrayField, "columns", "Posts", arr));
        f.bindings.push_back(bind(AsmBinding::Kind::ArrayField, "columnSpacing", "Gap", arr));
        REQUIRE(tx.modifyBlock(fence, [&](BlockDef& d) { d.assembly = std::move(f); }).ok());
        tx.commit();
    }
    CHECK(asmValidate(b.doc, *b.doc.block(room)).empty());
    CHECK(asmValidate(b.doc, *b.doc.block(fence)).empty());

    Entity r = insertOf(room, {0, 0});
    entityAs<InsertData>(r)->asmValues = {AsmValue{"W", false, 40, ""}};
    EntityId rid = addEntity(b.doc, r);
    // W = 40 -> the Bar gets L = 20 -> its Length is 40
    CHECK(near(insertBounds(b.doc, *b.doc.find(rid)).max, {40, 0}));
    // default W = 20 -> L = 10 -> Length 20
    CHECK(near(insertBounds(b.doc, *b.doc.find(addEntity(b.doc, insertOf(room, {0, 100})))).max,
               {20, 100}));

    Entity f = insertOf(fence, {0, 0});
    entityAs<InsertData>(f)->asmValues = {AsmValue{"Posts", false, 4, ""},
                                          AsmValue{"Gap", false, 3, ""}};
    EntityId fid = addEntity(b.doc, f);
    auto ex = explode(b.doc, *b.doc.find(fid));
    REQUIRE(ex.ok());
    REQUIRE(ex.value().entities.size() == 1);
    const auto* nestedIns = entityAs<InsertData>(ex.value().entities[0]);
    REQUIRE(nestedIns);
    REQUIRE(nestedIns->array);
    CHECK(nestedIns->array->columns == 4);
    CHECK(nestedIns->array->columnSpacing == doctest::Approx(3));
    auto ex2 = explode(b.doc, ex.value().entities[0]);
    REQUIRE(ex2.ok());
    REQUIRE(ex2.value().entities.size() == 4);
    CHECK(near(entityAs<CircleData>(ex2.value().entities[3])->center, {9, 0}));
}

TEST_CASE("assembly: attribute texts come from formulas") {
    Bar b;
    {
        Transaction tx = b.doc.begin("att");
        Entity t;
        TextData td;
        td.utf8 = "TAG";
        td.attdef = AttDefInfo{"SIZE", "", "x", false, false, false, false};
        t.data = Indirect<TextData>(std::move(td));
        REQUIRE(tx.add(std::move(t), b.block).ok());
        REQUIRE(tx.modifyBlock(b.block, [](BlockDef& d) {
                      d.assembly->bindings.push_back(bind(AsmBinding::Kind::Attribute, "SIZE",
                                                          "\"Bar \" & L & \" x \" & Label"));
                  }).ok());
        tx.commit();
    }
    EntityId id = b.insert({AsmValue{"L", false, 7, ""}});
    auto res =
        resolveAssembly(b.doc, *b.doc.block(b.block), *entityAs<InsertData>(*b.doc.find(id)));
    REQUIRE(res.insert.attribs.size() == 1);
    CHECK(res.insert.attribs[0].value == "Bar 7 x bar");
    CHECK(asmValidate(b.doc, *b.doc.block(b.block)).empty());
}

TEST_CASE("assemblyBom counts placed, nested and arrayed assemblies by parameter values") {
    Document doc;
    BlockId leaf, room;
    EntityId inner;
    {
        auto tx = doc.begin("blocks");
        leaf = tx.addBlock(BlockDef{.name = "Leg"}).value();
        REQUIRE(tx.add(Entity{{}, LineData{{0, 0}, {0, 10}}}, leaf).ok());
        REQUIRE(tx.modifyBlock(leaf, [](BlockDef& d) {
                      AssemblyDef a;
                      AsmParam p;
                      p.name = "Len";
                      p.kind = AsmParam::Kind::Number;
                      p.defaultNumber = 10;
                      a.params.push_back(p);
                      d.assembly = a;
                  }).ok());
        room = tx.addBlock(BlockDef{.name = "Table"}).value();
        InsertData li;
        li.block = leaf;
        li.array = ArrayInfo{};
        li.array->rows = 1;
        li.array->columns = 4;
        auto id = tx.add(Entity{{}, std::move(li)}, room);
        REQUIRE(id.ok());
        inner = id.value();
        REQUIRE(tx.modifyBlock(room, [&](BlockDef& d) {
                      AssemblyDef a;
                      AsmParam p;
                      p.name = "H";
                      p.kind = AsmParam::Kind::Number;
                      p.defaultNumber = 70;
                      a.params.push_back(p);
                      AsmBinding b;
                      b.kind = AsmBinding::Kind::Nested;
                      b.entity = inner;
                      b.target = "Len";
                      b.formula = "H";
                      a.bindings.push_back(b);
                      d.assembly = a;
                  }).ok());
        InsertData t1;
        t1.block = room;
        REQUIRE(tx.add(Entity{{}, t1}).ok());
        InsertData t2;
        t2.block = room;
        t2.asmValues.push_back(AsmValue{"H", false, 90, ""});
        REQUIRE(tx.add(Entity{{}, t2}).ok());
        InsertData loose;
        loose.block = leaf;
        REQUIRE(tx.add(Entity{{}, loose}).ok());
        tx.commit();
    }
    const auto rows = blocks::assemblyBom(doc);
    std::size_t legsDefault = 0, legs70 = 0, legs90 = 0, tables = 0;
    for (const auto& r : rows) {
        if (r.block == "Table")
            tables += r.count;
        else if (r.values[0].second == "10")
            legsDefault += r.count;
        else if (r.values[0].second == "70")
            legs70 += r.count;
        else if (r.values[0].second == "90")
            legs90 += r.count;
    }
    CHECK(tables == 2);
    CHECK(legsDefault == 1);  // the loose one
    CHECK(legs70 == 4);       // the first table, four in the array
    CHECK(legs90 == 4);       // the second table through its binding
    const std::string csv = blocks::assemblyBomCsv(rows);
    CHECK(csv.rfind("Assembly,Parameters,Quantity", 0) == 0);
}
