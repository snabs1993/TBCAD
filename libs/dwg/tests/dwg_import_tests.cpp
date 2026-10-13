#include <doctest/doctest.h>

#include <algorithm>

#include "dwg_builder.h"
#include "tbcad/dwg/dwg.h"

using namespace tbcad;
using namespace tbcad::dwg;
using namespace tbcad::dwg::test;

namespace {

std::vector<std::byte> asBytes(const std::vector<std::uint8_t>& v) {
    std::vector<std::byte> out(v.size());
    std::memcpy(out.data(), v.data(), v.size());
    return out;
}

constexpr std::uint64_t kLayer0 = 0x10, kWalls = 0x11, kLtype = 0x12, kStyle = 0x13, kModel = 0x14,
                        kPaper = 0x15, kDoor = 0x16;

std::vector<std::uint8_t> layer(std::uint64_t h, const char* name, int color, std::uint64_t lt) {
    ObjectWriter o(0x33, h);
    o.objectCommon();
    o.main.text(name);
    o.main.b(false);
    o.main.bs(0);
    o.main.b(false);
    o.main.bs(16);  // plot flag only
    o.main.bs(color);
    o.handles.handle(4, 0x02);  // layer control
    o.handles.handle(3, 0);     // xdictionary
    o.handles.handle(5, 0);     // external reference block
    o.handles.handle(5, 0);     // plot style
    o.handles.handle(5, lt);
    return o.finish();
}

std::vector<std::uint8_t> linetype(std::uint64_t h, const char* name) {
    ObjectWriter o(0x39, h);
    o.objectCommon();
    o.main.text(name);
    o.main.b(false);
    o.main.bs(0);
    o.main.b(false);
    o.main.text("Solid line");
    o.main.bd(0.0);
    o.main.rc('A');
    o.main.rc(0);
    o.handles.handle(4, 0x05);
    o.handles.handle(3, 0);
    return o.finish();
}

std::vector<std::uint8_t> style(std::uint64_t h, const char* name) {
    ObjectWriter o(0x35, h);
    o.objectCommon();
    o.main.text(name);
    o.main.b(false);
    o.main.bs(0);
    o.main.b(false);
    o.main.b(false);
    o.main.b(false);
    o.main.bd(0.0);
    o.main.bd(1.0);
    o.main.bd(0.0);
    o.main.rc(0);
    o.main.bd(2.5);
    o.main.text("txt");
    o.main.text("");
    o.handles.handle(4, 0x03);
    o.handles.handle(3, 0);
    return o.finish();
}

std::vector<std::uint8_t> blockHeader(std::uint64_t h, const char* name, double bx, double by) {
    ObjectWriter o(0x31, h);
    o.objectCommon();
    o.main.text(name);
    o.main.b(false);
    o.main.bs(0);
    for (int i = 0; i < 6; ++i)
        o.main.b(false);  // xdep, anonymous, has attributes, xref, overlay, loaded
    o.main.bd(bx);
    o.main.bd(by);
    o.main.bd(0.0);
    o.main.text("");
    o.handles.handle(4, 0x01);
    o.handles.handle(3, 0);
    return o.finish();
}

void commonHandles(ObjectWriter& o, int entmode, std::uint64_t owner, std::uint64_t layer) {
    if (entmode == 0)
        o.handles.handle(4, owner);
    o.handles.handle(3, 0);  // xdictionary
    o.handles.handle(5, layer);
}

std::vector<std::uint8_t> line(std::uint64_t h, int entmode, std::uint64_t owner, double x0,
                               double y0, double x1, double y1) {
    ObjectWriter o(0x13, h);
    o.entityCommon(entmode, 256, 0);
    o.main.b(true);  // z's are zero
    o.main.rd(x0);
    o.main.dd(x1, x0);
    o.main.rd(y0);
    o.main.dd(y1, y0);
    o.main.b(true);  // thickness 0
    o.main.b(true);  // extrusion default
    commonHandles(o, entmode, owner, kLayer0);
    return o.finish();
}

std::vector<std::uint8_t> circle(std::uint64_t h, double cx, double cy, double r,
                                 std::uint64_t layer) {
    ObjectWriter o(0x12, h);
    o.entityCommon(2, 1, 0);
    o.main.bd(cx);
    o.main.bd(cy);
    o.main.bd(0.0);
    o.main.bd(r);
    o.main.b(true);
    o.main.b(true);
    commonHandles(o, 2, 0, layer);
    return o.finish();
}

std::vector<std::uint8_t> arc(std::uint64_t h) {
    ObjectWriter o(0x11, h);
    o.entityCommon(2, 256, 0);
    o.main.bd(5.0);
    o.main.bd(5.0);
    o.main.bd(0.0);
    o.main.bd(2.0);
    o.main.b(true);
    o.main.b(true);
    o.main.bd(0.0);
    o.main.bd(3.14159265358979323846 / 2);
    commonHandles(o, 2, 0, kLayer0);
    return o.finish();
}

std::vector<std::uint8_t> lwpolyline(std::uint64_t h, bool closed) {
    ObjectWriter o(0x4D, h);
    o.entityCommon(2, 256, 0);
    o.main.bs(16 | (closed ? 512 : 0));
    o.main.bl(3);
    o.main.bl(3);  // bulges
    o.main.rd(0);
    o.main.rd(0);
    o.main.dd(10, 0);
    o.main.dd(0, 0);
    o.main.dd(10, 10);
    o.main.dd(10, 0);
    o.main.bd(0.0);
    o.main.bd(0.5);
    o.main.bd(0.0);
    commonHandles(o, 2, 0, kWalls);
    return o.finish();
}

std::vector<std::uint8_t> text(std::uint64_t h) {
    ObjectWriter o(0x01, h);
    o.entityCommon(2, 256, 0);
    // data flags: no elevation, no alignment point, no oblique, no rotation, width factor 1,
    // no generation / alignment flags
    o.main.rc(0xFF);
    o.main.rd(3.0);
    o.main.rd(4.0);
    o.main.b(true);  // extrusion default
    o.main.b(true);  // thickness 0
    o.main.rd(2.5);  // height
    o.main.text("Hello");
    commonHandles(o, 2, 0, kLayer0);
    o.handles.handle(5, kStyle);
    return o.finish();
}

std::vector<std::uint8_t> insert(std::uint64_t h, double x, double y, double sx, double rot) {
    ObjectWriter o(0x07, h);
    o.entityCommon(2, 256, 0);
    o.main.bd(x);
    o.main.bd(y);
    o.main.bd(0.0);
    if (sx == 1.0) {
        o.main.bb(3);
    } else {
        o.main.bb(2);
        o.main.rd(sx);
    }
    o.main.bd(rot);
    o.main.bd(0.0);
    o.main.bd(0.0);
    o.main.bd(1.0);
    o.main.b(false);  // no attributes
    commonHandles(o, 2, 0, kLayer0);
    o.handles.handle(5, kDoor);
    return o.finish();
}

std::vector<std::uint8_t> sampleDrawing() {
    DwgFile f;
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kWalls, layer(kWalls, "WALLS", 3, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    f.add(kDoor, blockHeader(kDoor, "DOOR", 1, 2));
    f.add(0x20, line(0x20, 2, 0, 0, 0, 100, 50));
    f.add(0x21, circle(0x21, 10, 20, 5, kWalls));
    f.add(0x22, arc(0x22));
    f.add(0x23, lwpolyline(0x23, true));
    f.add(0x24, text(0x24));
    f.add(0x25, insert(0x25, 30, 40, 2.0, 0.0));
    f.add(0x30, line(0x30, 0, kDoor, 0, 0, 0, 10));
    return f.build();
}

}  // namespace

TEST_CASE("an R2000 file with tables, entities and a block imports") {
    auto file = asBytes(sampleDrawing());
    dxf::Report report;
    auto r = importDwg(file, dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const Document& doc = *r.value().document;
    CHECK(r.value().summary.version == "R2000");
    CHECK(report.count(dxf::Severity::Error) == 0);

    CHECK(doc.layerByName("WALLS") != nullptr);
    CHECK(doc.blockByName("DOOR") != nullptr);
    CHECK(doc.block(doc.blockByName("DOOR")->id)->entities.size() == 1);

    int lines = 0, circles = 0, arcs = 0, polys = 0, texts = 0, inserts = 0;
    for (EntityId id : doc.modelSpace()) {
        const Entity& e = *doc.find(id);
        switch (kind(e)) {
            case EntityKind::Line: {
                ++lines;
                const auto* l = entityAs<LineData>(e);
                CHECK(l->a == Vec2{0, 0});
                CHECK(l->b == Vec2{100, 50});
                break;
            }
            case EntityKind::Circle: {
                ++circles;
                const auto* c = entityAs<CircleData>(e);
                CHECK(c->center == Vec2{10, 20});
                CHECK(c->radius == 5);
                CHECK(doc.layer(e.common.layer)->name == "WALLS");
                break;
            }
            case EntityKind::Arc:
                ++arcs;
                break;
            case EntityKind::Polyline: {
                ++polys;
                const auto* p = entityAs<PolylineData>(e);
                CHECK(p->closed);
                CHECK(p->v.size() == 3);
                CHECK(p->v[1].bulge == doctest::Approx(0.5));
                break;
            }
            case EntityKind::Text: {
                ++texts;
                const auto* t = entityAs<TextData>(e);
                CHECK(t->utf8 == "Hello");
                CHECK(t->height == 2.5);
                CHECK(t->position == Vec2{3, 4});
                break;
            }
            case EntityKind::Insert: {
                ++inserts;
                const auto* i = entityAs<InsertData>(e);
                CHECK(i->position == Vec2{30, 40});
                CHECK(i->scale == Vec2{2, 2});
                break;
            }
            default:
                break;
        }
    }
    CHECK(lines == 1);
    CHECK(circles == 1);
    CHECK(arcs == 1);
    CHECK(polys == 1);
    CHECK(texts == 1);
    CHECK(inserts == 1);
}

TEST_CASE("damaged or truncated files give a result or a clean error, never a crash") {
    auto good = sampleDrawing();
    for (std::size_t cut : {std::size_t{10}, std::size_t{0x30}, good.size() / 2, good.size() - 3}) {
        std::vector<std::uint8_t> bad(good.begin(),
                                      good.begin() + static_cast<std::ptrdiff_t>(cut));
        dxf::Report report;
        auto r = importDwg(asBytes(bad), dxf::ImportOptions{}, report);
        (void)r;  // either outcome is fine
    }
    // Bit flips across the whole file.
    for (std::size_t i = 0x19; i < good.size(); i += 7) {
        auto bad = good;
        bad[i] = static_cast<std::uint8_t>(bad[i] ^ 0x5A);
        dxf::Report report;
        auto r = importDwg(asBytes(bad), dxf::ImportOptions{}, report);
        (void)r;
    }
    CHECK(true);
}
