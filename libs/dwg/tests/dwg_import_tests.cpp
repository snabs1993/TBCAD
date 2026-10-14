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
    o.cmc(color);
    o.handles.handle(4, 0x02);  // layer control
    o.xdic();
    o.handles.handle(5, 0);  // external reference block
    o.handles.handle(5, 0);  // plot style
    if (gR2007)
        o.handles.handle(5, 0);  // material
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
    o.xdic();
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
    o.xdic();
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
    if (gR2004)
        o.main.bl(0);  // owned object count
    o.main.bd(bx);
    o.main.bd(by);
    o.main.bd(0.0);
    o.main.text("");
    o.handles.handle(4, 0x01);
    o.xdic();
    return o.finish();
}

void commonHandles(ObjectWriter& o, int entmode, std::uint64_t owner, std::uint64_t layer) {
    if (entmode == 0)
        o.handles.handle(4, owner);
    o.xdic();
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
    if (gR2004)
        o.main.bl(0);  // owned object count
    commonHandles(o, 2, 0, kLayer0);
    o.handles.handle(5, kDoor);
    return o.finish();
}

std::vector<std::uint8_t> ellipseObj(std::uint64_t h) {
    ObjectWriter o(0x23, h);
    o.entityCommon(2, 256, 0);
    o.main.bd(10.0);  // centre
    o.main.bd(20.0);
    o.main.bd(0.0);
    o.main.bd(4.0);  // semi-major vector
    o.main.bd(0.0);
    o.main.bd(0.0);
    o.main.bd(0.0);  // extrusion
    o.main.bd(0.0);
    o.main.bd(1.0);
    o.main.bd(0.5);  // ratio
    o.main.bd(0.0);
    o.main.bd(6.283185307179586);
    commonHandles(o, 2, 0, kLayer0);
    return o.finish();
}

std::vector<std::uint8_t> splineObj(std::uint64_t h) {
    ObjectWriter o(0x24, h);
    o.entityCommon(2, 256, 0);
    o.main.bl(1);  // control points and knots
    o.main.bl(2);  // degree
    o.main.b(false);
    o.main.b(false);
    o.main.b(false);
    o.main.bd(0.0);
    o.main.bd(0.0);
    o.main.bl(6);  // knots
    o.main.bl(3);  // control points
    o.main.b(false);
    for (double k : {0.0, 0.0, 0.0, 1.0, 1.0, 1.0})
        o.main.bd(k);
    for (double c : {0.0, 0.0, 0.0, 5.0, 8.0, 0.0, 10.0, 0.0, 0.0})
        o.main.bd(c);
    commonHandles(o, 2, 0, kLayer0);
    return o.finish();
}

std::vector<std::uint8_t> solidObj(std::uint64_t h) {
    ObjectWriter o(0x1F, h);
    o.entityCommon(2, 256, 0);
    o.main.b(true);  // thickness 0
    o.main.bd(0.0);  // elevation
    for (double v : {0.0, 0.0, 10.0, 0.0, 0.0, 10.0, 10.0, 10.0})
        o.main.rd(v);
    o.main.b(true);  // extrusion default
    commonHandles(o, 2, 0, kLayer0);
    return o.finish();
}

std::vector<std::uint8_t> polylineObj(std::uint64_t h, std::uint64_t first, std::uint64_t last) {
    ObjectWriter o(0x0F, h);
    o.entityCommon(2, 256, 0);
    o.main.bs(1);  // closed
    o.main.bs(0);
    o.main.bd(0.0);
    o.main.bd(0.0);
    o.main.b(true);  // thickness 0
    o.main.bd(0.0);
    o.main.b(true);  // extrusion default
    commonHandles(o, 2, 0, kLayer0);
    o.handles.handle(4, first);
    o.handles.handle(4, last);
    o.handles.handle(4, last + 1);  // SEQEND
    return o.finish();
}

std::vector<std::uint8_t> vertexObj(std::uint64_t h, double x, double y, double bulge) {
    ObjectWriter o(0x0A, h);
    o.entityCommon(2, 256, 0);
    o.main.rc(0);
    o.main.bd(x);
    o.main.bd(y);
    o.main.bd(0.0);
    o.main.bd(0.0);  // start width (0, so an end width follows)
    o.main.bd(0.0);
    o.main.bd(bulge);
    o.main.bd(0.0);
    commonHandles(o, 2, 0, kLayer0);
    return o.finish();
}

std::vector<std::uint8_t> mtextObj(std::uint64_t h) {
    ObjectWriter o(0x2C, h);
    o.entityCommon(2, 256, 0);
    for (double v : {5.0, 6.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0})
        o.main.bd(v);
    o.main.bd(40.0);  // width
    o.main.bd(0.0);   // height of the box
    o.main.bs(1);     // attachment top left
    o.main.bs(1);
    o.main.bd(0.0);
    o.main.bd(0.0);
    o.main.text("Line one\\Pline two");
    o.main.bs(1);
    o.main.bd(1.0);
    o.main.b(false);
    commonHandles(o, 2, 0, kLayer0);
    o.handles.handle(5, kStyle);
    return o.finish();
}

std::vector<std::uint8_t> hatchObj(std::uint64_t h, int type) {
    ObjectWriter o(type, h);
    o.entityCommon(2, 256, 0);
    o.main.bd(0.0);  // elevation
    o.main.bd(0.0);  // extrusion
    o.main.bd(0.0);
    o.main.bd(1.0);
    o.main.text("SOLID");
    o.main.b(true);   // solid
    o.main.b(false);  // not associative
    o.main.bl(2);     // two paths
    // path 1: a polyline path, a closed square
    o.main.bl(2);
    o.main.b(false);
    o.main.b(true);
    o.main.bl(4);
    for (double v : {0.0, 0.0, 10.0, 0.0, 10.0, 10.0, 0.0, 10.0})
        o.main.rd(v);
    o.main.bl(0);
    // path 2: edges, a line then a circular arc
    o.main.bl(0);
    o.main.bl(2);
    o.main.rc(1);
    for (double v : {2.0, 2.0, 4.0, 2.0})
        o.main.rd(v);
    o.main.rc(2);
    o.main.rd(3.0);
    o.main.rd(2.0);
    o.main.bd(1.0);
    o.main.bd(0.0);
    o.main.bd(3.14159265358979323846);
    o.main.b(true);
    o.main.bl(0);
    o.main.bs(0);  // style
    o.main.bs(1);  // pattern type
    o.main.bl(0);  // no seed points
    commonHandles(o, 2, 0, kLayer0);
    return o.finish();
}

constexpr std::uint64_t kDimStyle = 0x17, kDimBlock = 0x18;

std::vector<std::uint8_t> dimStyleObj(std::uint64_t h, const char* name, double textHeight) {
    ObjectWriter o(0x45, h);
    o.objectCommon();
    o.main.text(name);
    o.main.b(false);  // 64 flag
    o.main.bs(0);
    o.main.b(false);
    o.main.text("");  // DIMPOST
    o.main.text("");  // DIMAPOST
    for (int code : {40, 41, 42, 43, 44, 45, 46, 47, 48})
        o.main.bd(code == 40 ? 1.0 : code == 41 ? 2.5 : 0.0);
    for (int i = 0; i < 6; ++i)
        o.main.b(false);  // 71..76
    for (int i = 0; i < 3; ++i)
        o.main.bs(0);  // 77..79
    for (int code : {140, 141, 142, 143, 144, 145, 146, 147, 148})
        o.main.bd(code == 140 ? textHeight : code == 147 ? 0.625 : 0.0);
    o.main.b(false);  // 170
    o.main.bs(0);     // 171
    for (int i = 0; i < 4; ++i)
        o.main.b(false);  // 172..175
    for (int i = 0; i < 14; ++i)
        o.main.bs(i == 5 ? 2 : 0);  // 176..280 ... (271 DIMDEC is the 6th entry)
    o.main.b(false);
    o.main.b(false);
    for (int i = 0; i < 4; ++i)
        o.main.bs(0);  // 283..286
    o.main.b(false);   // 288
    o.main.bs(0);      // 287
    o.main.bs(0);      // 371
    o.main.bs(0);      // 372
    o.main.b(false);   // unknown bit
    o.handles.handle(4, 0x04);
    o.xdic();
    for (int i = 0; i < 6; ++i)
        o.handles.handle(5, 0);
    return o.finish();
}

std::vector<std::uint8_t> linearDimObj(std::uint64_t h) {
    ObjectWriter o(0x15, h);
    o.entityCommon(2, 256, 0);
    for (double v : {0.0, 0.0, 1.0})
        o.main.bd(v);  // extrusion
    o.main.rd(5.0);    // text midpoint
    o.main.rd(2.0);
    o.main.bd(0.0);  // elevation
    o.main.rc(1);    // flags 1
    o.main.text("");
    o.main.bd(0.0);
    o.main.bd(0.0);
    o.main.bd(1.0);
    o.main.bd(1.0);
    o.main.bd(1.0);
    o.main.bd(0.0);
    o.main.bs(5);
    o.main.bs(1);
    o.main.bd(1.0);
    o.main.bd(10.0);  // measurement
    o.main.rd(0.0);   // 12-pt
    o.main.rd(0.0);
    for (double v : {0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 0.0, 2.0, 0.0})
        o.main.bd(v);  // 13, 14, 10 points
    o.main.bd(0.0);    // extension line rotation
    o.main.bd(0.0);    // dimension rotation
    commonHandles(o, 2, 0, kLayer0);
    o.handles.handle(5, kDimStyle);
    o.handles.handle(5, kDimBlock);
    return o.finish();
}

DwgFile sampleFile() {
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
    return f;
}

std::vector<std::uint8_t> sampleDrawing() {
    return sampleFile().build();
}

std::vector<std::uint8_t> sampleDrawing2004(const char* version) {
    const std::string v = version;
    gR2004 = true;
    gR2007 = v >= "AC1024";
    gR2013 = v >= "AC1027";
    const DwgFile f = sampleFile();
    gR2004 = gR2007 = gR2013 = false;
    Dwg2004File c;
    std::vector<std::uint8_t> objects, handles;
    f.sections(objects, handles);
    c.section("AcDb:AcDbObjects", objects);
    c.section("AcDb:Handles", handles, 0x80);
    return c.build(version);
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

TEST_CASE("an R2004 file with compressed sections imports like the R2000 one") {
    for (const char* version : {"AC1018", "AC1024", "AC1027", "AC1032"}) {
        CAPTURE(version);
        auto file = asBytes(sampleDrawing2004(version));
        dxf::Report report;
        auto r = importDwg(file, dxf::ImportOptions{}, report);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
        const Document& doc = *r.value().document;
        CHECK(report.count(dxf::Severity::Error) == 0);
        int texts = 0;
        for (EntityId id : doc.modelSpace()) {
            if (const auto* tx = entityAs<TextData>(*doc.find(id))) {
                ++texts;
                CHECK(tx->utf8 == "Hello");
            }
        }
        CHECK(texts == 1);
        CHECK(doc.layerByName("WALLS") != nullptr);
        REQUIRE(doc.blockByName("DOOR") != nullptr);
        CHECK(doc.modelSpace().size() == 6);
    }
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

TEST_CASE("an object map of many sections reads in full") {
    for (bool r2004 : {false, true}) {
        CAPTURE(r2004);
        gR2004 = r2004;
        DwgFile f;
        f.add(kLtype, linetype(kLtype, "Continuous"));
        f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
        f.add(kStyle, style(kStyle, "Standard"));
        f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
        f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
        for (std::uint64_t i = 0; i < 1500; ++i)
            f.add(0x100 + i * 3, line(0x100 + i * 3, 2, 0, 0, 0, double(i), 1));
        std::vector<std::uint8_t> bytes;
        if (r2004) {
            std::vector<std::uint8_t> objects, handles;
            f.sections(objects, handles);
            gR2004 = false;
            Dwg2004File c;
            c.section("AcDb:AcDbObjects", objects);
            c.section("AcDb:Handles", handles, 0x7400);
            bytes = c.build("AC1018");
        } else {
            bytes = f.build();
        }
        gR2004 = false;
        dxf::Report report;
        auto r = importDwg(asBytes(bytes), dxf::ImportOptions{}, report);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
        CHECK(r.value().document->modelSpace().size() == 1500);
    }
}

TEST_CASE("an object map that restarts its running values in each section reads in full") {
    gMapRestarts = true;
    gR2004 = true;
    DwgFile f;
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    for (std::uint64_t i = 0; i < 1500; ++i)
        f.add(0x100 + i * 3, line(0x100 + i * 3, 2, 0, 0, 0, double(i), 1));
    std::vector<std::uint8_t> objects, handles;
    f.sections(objects, handles);
    gR2004 = false;
    gMapRestarts = false;
    Dwg2004File c;
    c.section("AcDb:AcDbObjects", objects);
    c.section("AcDb:Handles", handles);
    dxf::Report report;
    auto r = importDwg(asBytes(c.build("AC1018")), dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    CHECK(r.value().document->modelSpace().size() == 1500);
}

TEST_CASE("describe lists the container steps and sections") {
    const std::string text = describe(asBytes(sampleDrawing2004("AC1032")));
    CHECK(text.find("AC1032") != std::string::npos);
    CHECK(text.find("section \"AcDb:AcDbObjects\"") != std::string::npos);
    CHECK(text.find("decoded section AcDb:Handles") != std::string::npos);
    CHECK(text.find("object types (type:count):") != std::string::npos);
    CHECK(text.find("import: ok") != std::string::npos);
    CHECK(describe(asBytes(std::vector<std::uint8_t>{1, 2, 3})).find("not a DWG") !=
          std::string::npos);
}

TEST_CASE("damaged R2004 and R2010 files give a result or a clean error, never a crash") {
    for (const char* version : {"AC1018", "AC1024"}) {
        const auto good = sampleDrawing2004(version);
        for (std::size_t cut :
             {std::size_t{0x90}, std::size_t{0x100}, good.size() / 2, good.size() - 5}) {
            std::vector<std::uint8_t> bad(good.begin(),
                                          good.begin() + static_cast<std::ptrdiff_t>(cut));
            dxf::Report report;
            (void)importDwg(asBytes(bad), dxf::ImportOptions{}, report);
        }
        for (std::size_t i = 0x80; i < good.size(); i += 13) {
            auto bad = good;
            bad[i] = static_cast<std::uint8_t>(bad[i] ^ 0xA5);
            dxf::Report report;
            (void)importDwg(asBytes(bad), dxf::ImportOptions{}, report);
        }
    }
    CHECK(true);
}

TEST_CASE("ellipse, spline and solid import") {
    DwgFile f;
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    f.add(0x40, ellipseObj(0x40));
    f.add(0x41, splineObj(0x41));
    f.add(0x42, solidObj(0x42));
    dxf::Report report;
    auto r = importDwg(asBytes(f.build()), dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const Document& doc = *r.value().document;
    int ellipses = 0, splines = 0, others = 0;
    for (EntityId id : doc.modelSpace()) {
        const Entity& e = *doc.find(id);
        if (kind(e) == EntityKind::Ellipse) {
            ++ellipses;
            const auto* d = entityAs<EllipseData>(e);
            CHECK(d->center == Vec2{10, 20});
            CHECK(d->majorAxis == Vec2{4, 0});
            CHECK(d->ratio == doctest::Approx(0.5));
        } else if (kind(e) == EntityKind::Spline) {
            ++splines;
            const auto* d = entityAs<SplineData>(e);
            CHECK(d->degree == 2);
            CHECK(d->control.size() == 3);
            CHECK(d->control[1] == Vec2{5, 8});
            CHECK(d->knots.size() == 6);
        } else {
            ++others;  // the solid
        }
    }
    CHECK(ellipses == 1);
    CHECK(splines == 1);
    CHECK(others == 1);
}

TEST_CASE("a 2D polyline with its vertices and an mtext import") {
    DwgFile f;
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    f.add(0x50, polylineObj(0x50, 0x51, 0x53));
    f.add(0x51, vertexObj(0x51, 0, 0, 0));
    f.add(0x52, vertexObj(0x52, 10, 0, 0.5));
    f.add(0x53, vertexObj(0x53, 10, 10, 0));
    f.add(0x60, mtextObj(0x60));
    dxf::Report report;
    auto r = importDwg(asBytes(f.build()), dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const Document& doc = *r.value().document;
    int polys = 0, mtexts = 0;
    for (EntityId id : doc.modelSpace()) {
        const Entity& e = *doc.find(id);
        if (kind(e) == EntityKind::Polyline) {
            ++polys;
            const auto* p = entityAs<PolylineData>(e);
            CHECK(p->closed);
            REQUIRE(p->v.size() == 3);
            CHECK(p->v[1].bulge == doctest::Approx(0.5));
            CHECK(p->v[2].p == Vec2{10, 10});
        } else if (kind(e) == EntityKind::MText) {
            ++mtexts;
            const auto* m = entityAs<MTextData>(e);
            CHECK(m->position == Vec2{5, 6});
            CHECK(m->width == 40);
        }
    }
    CHECK(polys == 1);
    CHECK(mtexts == 1);
}

TEST_CASE("a solid hatch with two kinds of boundary imports") {
    DwgFile f;
    const int hatchType = f.addClass("HATCH");
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    f.add(0x70, hatchObj(0x70, hatchType));
    dxf::Report report;
    auto r = importDwg(asBytes(f.build()), dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const Document& doc = *r.value().document;
    int hatches = 0;
    for (EntityId id : doc.modelSpace()) {
        const Entity& e = *doc.find(id);
        if (kind(e) == EntityKind::Hatch) {
            ++hatches;
            const auto* d = entityAs<HatchData>(e);
            CHECK(d->solid);
            CHECK(d->loops.size() == 2);
        }
    }
    CHECK(hatches == 1);
}

TEST_CASE("a linear dimension with its style and drawn block imports") {
    DwgFile f;
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    f.add(kDimStyle, dimStyleObj(kDimStyle, "ARCH", 3.5));
    f.add(kDimBlock, blockHeader(kDimBlock, "*D1", 0, 0));
    f.add(0x80, linearDimObj(0x80));
    f.add(0x81, line(0x81, 0, kDimBlock, 0, 0, 10, 0));
    dxf::Report report;
    auto r = importDwg(asBytes(f.build()), dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const Document& doc = *r.value().document;
    REQUIRE(doc.dimStyleByName("ARCH") != nullptr);
    CHECK(doc.dimStyleByName("ARCH")->textHeight == doctest::Approx(3.5));
    int dims = 0;
    for (EntityId id : doc.modelSpace()) {
        const Entity& e = *doc.find(id);
        if (kind(e) == EntityKind::Dimension) {
            ++dims;
            const auto* d = entityAs<DimensionData>(e);
            CHECK(d->kind == DimKind::Linear);
            CHECK(d->measurement == doctest::Approx(10.0));
        }
    }
    CHECK(dims == 1);
}
