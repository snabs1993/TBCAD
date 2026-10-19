#include <doctest/doctest.h>

#include "tbcad/core/core.h"

using namespace tbcad;

TEST_CASE("Boxed holds an optional value on the heap and copies it deeply") {
    static_assert(sizeof(Boxed<AttDefInfo>) == sizeof(void*));
    static_assert(sizeof(TextData) < 140);
    static_assert(sizeof(InsertData) < 200);

    Boxed<AttDefInfo> a;
    CHECK(!a);
    CHECK(!a.has_value());
    CHECK(a == std::nullopt);
    CHECK_THROWS_AS((void)a.value(), std::bad_optional_access);

    a = AttDefInfo{.tag = "ROOM", .prompt = "Name?", .defaultValue = "101"};
    REQUIRE(a);
    CHECK(a->tag == "ROOM");
    CHECK((*a).defaultValue == "101");

    Boxed<AttDefInfo> b = a;  // a copy of its own, not a share
    b->tag = "OTHER";
    CHECK(a->tag == "ROOM");
    CHECK(!(a == b));
    b->tag = "ROOM";
    CHECK(a == b);

    Boxed<AttDefInfo> c = std::move(b);
    CHECK(c->tag == "ROOM");

    std::optional<AttDefInfo> plain = a;  // reads like an optional where one is expected
    REQUIRE(plain);
    CHECK(plain->prompt == "Name?");
    a = plain;
    CHECK(a->prompt == "Name?");

    a.reset();
    CHECK(!a);
    a.emplace().tag = "NEW";
    CHECK(a->tag == "NEW");
    a = std::nullopt;
    CHECK(!a);
    CHECK(a.value_or(AttDefInfo{.tag = "X"}).tag == "X");
}

TEST_CASE("a text and an insert keep their attribute definition and array through a copy") {
    TextData t;
    t.attdef = AttDefInfo{.tag = "T", .prompt = "p", .defaultValue = "d", .constant = true};
    TextData u = t;
    u.attdef->tag = "U";
    CHECK(t.attdef->tag == "T");
    CHECK(u.attdef->constant);

    InsertData ins;
    ArrayInfo info;
    info.rows = 3;
    info.columns = 4;
    ins.array = info;
    InsertData copy = ins;
    copy.array->rows = 9;
    CHECK(ins.array->rows == 3);
    CHECK(copy.array->columns == 4);
    ins.array.reset();
    CHECK(!ins.array);
    CHECK(copy.array);
}
