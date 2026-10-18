#include <doctest/doctest.h>

#include <string>

#include "map/emit.h"
#include "objects/model.h"

namespace tbcad::dwg::test {
using namespace detail;

namespace {
EntityRec point(std::uint64_t handle, std::uint64_t owner, double x) {
    EntityRec e;
    e.handle = handle;
    e.dxfType = "POINT";
    e.entmode = 0;
    e.owner = owner;
    e.groups.push_back({10, Value{x}});
    e.groups.push_back({20, Value{0.0}});
    return e;
}
}  // namespace

TEST_CASE("draw order: a sort table puts a block's entities in the order of its sort handles") {
    detail::Drawing d;
    detail::BlockRec model;
    model.handle = 1;
    model.name = "*Model_Space";
    d.blocks[1] = model;
    detail::BlockRec blk;
    blk.handle = 2;
    blk.name = "B";
    d.blocks[2] = blk;
    d.entities.push_back(point(0x10, 2, 111.0));
    d.entities.push_back(point(0x11, 2, 222.0));
    d.entities.push_back(point(0x12, 2, 333.0));
    const std::string plain = detail::emitDxf(d);
    CHECK(plain.find("111") < plain.find("222"));
    d.sortKeys[2][0x10] = 0x50;  // the first entity goes to the front of the others
    const std::string sorted = detail::emitDxf(d);
    CHECK(sorted.find("222") < sorted.find("333"));
    CHECK(sorted.find("333") < sorted.find("111"));
}

}  // namespace tbcad::dwg::test
