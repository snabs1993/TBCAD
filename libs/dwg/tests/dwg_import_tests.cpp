#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

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

std::vector<std::uint8_t> layer(std::uint64_t h, const char* name, int color, std::uint64_t lt,
                                int flags = 16, bool described = false) {
    ObjectWriter o(0x33, h);
    if (described) {
        // Extended data of the AcAecLayerStandard application: two strings, the second is the
        // description ("Walls", plus a Thai letter from R2007 where strings are UTF-16).
        std::vector<std::uint8_t> d;
        for (int i = 0; i < 2; ++i) {
            d.push_back(0);
            const std::u16string text = i == 0 ? u"" : (gR2007 ? u"Walls\u0e1c" : u"Walls");
            if (gR2007) {
                d.push_back(static_cast<std::uint8_t>(text.size()));
                d.push_back(0);
                for (char16_t c : text) {
                    d.push_back(static_cast<std::uint8_t>(c & 0xff));
                    d.push_back(static_cast<std::uint8_t>(c >> 8));
                }
            } else {
                d.push_back(static_cast<std::uint8_t>(text.size()));
                d.push_back(0);
                d.push_back(0);
                for (char16_t c : text)
                    d.push_back(static_cast<std::uint8_t>(c));
            }
        }
        o.main.bs(static_cast<int>(d.size()));
        o.main.handle(5, 0x99);
        for (std::uint8_t b : d)
            o.main.rc(b);
        o.main.bs(0);
        o.main.bl(0);  // reactors
        if (gR2004)
            o.main.b(true);
        if (gR2013)
            o.main.b(false);
    } else {
        o.objectCommon();
    }
    o.main.text(name);
    o.main.b(false);
    o.main.bs(0);
    o.main.b(false);
    o.main.bs(flags);  // plot flag only unless given
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
    if (gR2007) {
        o.main.rc(0);     // insert count terminator
        o.main.text("");  // description
        o.main.bl(0);     // preview size
        o.main.bs(0);     // insert units
        o.main.b(true);   // explodable
        o.main.rc(1);     // block scaling
    }
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

std::vector<std::uint8_t> lwpolyline(std::uint64_t h, bool closed, bool plinegen = false) {
    ObjectWriter o(0x4D, h);
    o.entityCommon(2, 256, 0);
    o.main.bs(16 | (closed ? 512 : 0) | (plinegen ? 128 : 0));
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

// An insert of the door block whose extension dictionary is `xdic`.
std::vector<std::uint8_t> insertWithXdic(std::uint64_t h, std::uint64_t xdic) {
    ObjectWriter o(0x07, h);
    o.entityCommon(2, 256, 0);
    o.main.bd(30.0);
    o.main.bd(40.0);
    o.main.bd(0.0);
    o.main.bb(3);  // scale 1
    o.main.bd(0.0);
    o.main.bd(0.0);
    o.main.bd(0.0);
    o.main.bd(1.0);
    o.main.b(false);  // no attributes
    o.handles.handle(3, xdic);
    o.handles.handle(5, kLayer0);
    o.handles.handle(5, kDoor);
    return o.finish();
}

// A DICTIONARY with the given (name, handle) items, owned by `owner`.
std::vector<std::uint8_t> dictionaryObj(
    std::uint64_t h, std::uint64_t owner,
    const std::vector<std::pair<const char*, std::uint64_t>>& items) {
    ObjectWriter o(0x2A, h);
    o.objectCommon();
    o.main.rl(static_cast<std::uint32_t>(items.size()));
    o.main.bs(0);  // cloning flag
    o.main.rc(1);  // hard owner
    for (const auto& it : items)
        o.main.text(it.first);
    o.handles.handle(4, owner);
    o.xdic();
    for (const auto& it : items)
        o.handles.handle(2, it.second);
    return o.finish();
}

// The SPATIAL_FILTER of an XCLIP: a rectangle, no front/back clip, identity transforms.
std::vector<std::uint8_t> spatialFilterObj(std::uint64_t h, int type, std::uint64_t owner,
                                           const std::vector<std::pair<double, double>>& pts) {
    ObjectWriter o(type, h);
    o.objectCommon();
    o.main.bs(static_cast<int>(pts.size()));
    for (const auto& p : pts) {
        o.main.rd(p.first);
        o.main.rd(p.second);
    }
    for (double v : {0.0, 0.0, 1.0})  // extrusion
        o.main.bd(v);
    for (double v : {0.0, 0.0, 0.0})  // clip boundary origin
        o.main.bd(v);
    o.main.bs(1);  // display the boundary
    o.main.bs(0);  // no front clip
    o.main.bs(0);  // no back clip
    for (int m = 0; m < 2; ++m)
        for (double v : {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0})
            o.main.bd(v);
    o.handles.handle(4, owner);
    o.xdic();
    return o.finish();
}

// SORTENTSTABLE: the count and the sort handles in the main data, then the owner, the block and
// the entity handles in the handle stream.
std::vector<std::uint8_t> sortEntsObj(
    std::uint64_t h, int type, std::uint64_t owner, std::uint64_t block,
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& pairs) {
    ObjectWriter o(type, h);
    o.objectCommon();
    o.main.bl(static_cast<int>(pairs.size()));
    for (const auto& [entity, sort] : pairs)
        o.main.handle(0, sort);
    o.handles.handle(4, owner);
    o.xdic();
    o.handles.handle(4, block);
    for (const auto& [entity, sort] : pairs)
        o.handles.handle(4, entity);
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

std::vector<std::uint8_t> polylineObj(std::uint64_t h, std::uint64_t first, std::uint64_t last,
                                      bool plinegen = false) {
    ObjectWriter o(0x0F, h);
    o.entityCommon(2, 256, 0);
    o.main.bs(1 | (plinegen ? 128 : 0));  // closed, linetype generation
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
    f.add(kWalls, layer(kWalls, "WALLS", 3, kLtype, 16, true));
    f.add(0x17, layer(0x17, "HIDDENLAYER", 5, kLtype, 16 | 2 | 1));  // off and frozen
    f.add(0x18, layer(0x18, "NOPLOT", 6, kLtype, 0));
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

TEST_CASE("an import names the time of each step for the open timings") {
    auto file = asBytes(sampleDrawing());
    dxf::Report report;
    std::vector<std::pair<std::string, double>> timings;
    auto r = importDwg(file, dxf::ImportOptions{}, report, &timings);
    REQUIRE(r.ok());
    std::vector<std::string> names;
    for (const auto& [name, ms] : timings) {
        names.push_back(name);
        CHECK(ms >= 0);
    }
    CHECK(std::find(names.begin(), names.end(), "convert to DXF groups") != names.end());
    CHECK(std::find(names.begin(), names.end(), "import the groups") != names.end());
    CHECK(names.size() >= 4);
}

TEST_CASE("an R2000 file with tables, entities and a block imports") {
    auto file = asBytes(sampleDrawing());
    dxf::Report report;
    auto r = importDwg(file, dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const Document& doc = *r.value().document;
    CHECK(r.value().summary.version == "R2000");
    CHECK(report.count(dxf::Severity::Error) == 0);

    CHECK(doc.layerByName("WALLS") != nullptr);
    CHECK(doc.layerByName("WALLS")->on);
    CHECK(doc.layerByName("WALLS")->plot);
    CHECK(doc.layerByName("WALLS")->description == "Walls");
    CHECK(doc.layerByName("NOPLOT")->description.empty());
    REQUIRE(doc.layerByName("HIDDENLAYER") != nullptr);
    CHECK_FALSE(doc.layerByName("HIDDENLAYER")->on);
    CHECK(doc.layerByName("HIDDENLAYER")->frozen);
    REQUIRE(doc.layerByName("NOPLOT") != nullptr);
    CHECK_FALSE(doc.layerByName("NOPLOT")->plot);
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
        REQUIRE(doc.layerByName("WALLS") != nullptr);
        CHECK(doc.layerByName("WALLS")->description ==
              (std::string(version) >= "AC1024" ? "Walls\xE0\xB8\x9C" : "Walls"));
        REQUIRE(doc.blockByName("DOOR") != nullptr);
        CHECK(doc.modelSpace().size() == 6);
    }
}

TEST_CASE("layer and block names in a Thai or accented code page show as UTF-8") {
    struct Case {
        int codepage;
        const char* layerBytes;
        const char* layerUtf8;
    };
    const Case cases[] = {
        {37, "\xCA\xD1 \xB5 A", "\xE0\xB8\xAA\xE0\xB8\xB1 \xE0\xB8\x95 A"},  // ANSI_874
        {30, "Caf\xE9 \x96 long layer name with spaces 12345",
         "Caf\xC3\xA9 \xE2\x80\x93 long layer name with spaces 12345"},
    };
    for (const Case& c : cases) {
        for (bool r2004 : {false, true}) {
            CAPTURE(c.codepage);
            CAPTURE(r2004);
            gCodepage = c.codepage;
            DwgFile f = sampleFile();
            f.add(0x19, layer(0x19, c.layerBytes, 3, kLtype));
            f.add(0x1a, blockHeader(0x1a, c.layerBytes, 1, 2));
            std::vector<std::uint8_t> bytes;
            if (r2004) {
                gR2004 = true;
                DwgFile g = sampleFile();
                g.add(0x19, layer(0x19, c.layerBytes, 3, kLtype));
                g.add(0x1a, blockHeader(0x1a, c.layerBytes, 1, 2));
                gR2004 = false;
                Dwg2004File cf;
                std::vector<std::uint8_t> objects, handles;
                g.sections(objects, handles);
                cf.section("AcDb:AcDbObjects", objects);
                cf.section("AcDb:Handles", handles, 0x80);
                bytes = cf.build("AC1018");
            } else {
                bytes = f.build();
            }
            gCodepage = 30;
            auto file = asBytes(bytes);
            dxf::Report report;
            auto r = importDwg(file, dxf::ImportOptions{}, report);
            REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
            const Document& doc = *r.value().document;
            CHECK(doc.layerByName(c.layerUtf8) != nullptr);
            CHECK(doc.blockByName(c.layerUtf8) != nullptr);
            CHECK(doc.layerByName("WALLS") != nullptr);
        }
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

TEST_CASE("random damage to every container version: no crash, no hang, an error or a result") {
    // Random byte, run and size-field damage on files of each layout. Each import must also
    // finish quickly (a pathological file may not keep the program busy).
    std::mt19937 rng(20261019);
    std::size_t imported = 0, rejected = 0;
    double slowest = 0;
    auto run = [&](const std::vector<std::uint8_t>& good) {
        for (int iter = 0; iter < 600; ++iter) {
            auto bad = good;
            for (int k = 0, n = 1 + static_cast<int>(rng() % 4); k < n; ++k) {
                const std::size_t at = 0x19 + rng() % (bad.size() - 0x19);
                switch (rng() % 6) {
                    case 0:
                        bad[at] = static_cast<std::uint8_t>(rng());
                        break;
                    case 1:
                        bad[at] = 0xFF;
                        break;
                    case 2:
                        bad[at] = 0;
                        break;
                    case 3: {  // a run of all-ones: huge sizes and counts
                        const std::size_t len =
                            std::min<std::size_t>(1 + rng() % 8, bad.size() - at);
                        std::fill_n(bad.begin() + static_cast<std::ptrdiff_t>(at), len, 0xFF);
                        break;
                    }
                    case 4:
                        bad.resize(at);
                        break;
                    default:
                        bad.insert(bad.begin() + static_cast<std::ptrdiff_t>(at),
                                   static_cast<std::uint8_t>(rng()));
                }
                if (bad.size() < 0x40)
                    break;
            }
            const auto t0 = std::chrono::steady_clock::now();
            dxf::Report report;
            auto r = importDwg(asBytes(bad), dxf::ImportOptions{}, report);
            const double sec =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            slowest = std::max(slowest, sec);
            if (r.ok()) {
                ++imported;
                for (EntityId id : r.value().document->modelSpace())
                    REQUIRE(r.value().document->find(id) != nullptr);
            } else {
                ++rejected;
                CHECK_FALSE(r.error().message.empty());
            }
        }
    };
    run(sampleDrawing());
    run(sampleDrawing2004("AC1018"));
    run(sampleDrawing2004("AC1024"));
    INFO("imported " << imported << " rejected " << rejected << " slowest " << slowest << " s");
    CHECK(slowest < 2.0);
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

// Real R2018 files store scenario 1 for a spline made of fit points; the method flag decides.
TEST_CASE("an R2013 fit-point spline is read by its method flag, not the stored scenario") {
    gR2004 = gR2007 = gR2013 = true;
    DwgFile f;
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    {
        ObjectWriter o(0x24, 0x41);
        o.entityCommon(2, 256, 0);
        o.main.bl(1);  // stored scenario (wrong for a fit spline)
        o.main.bl(9);  // method fit points + use knot parameter
        o.main.bl(0);  // chord
        o.main.bl(3);  // degree
        o.main.bd(1e-10);
        for (int i = 0; i < 6; ++i)
            o.main.bd(0.0);  // tangents
        o.main.bl(2);        // fit points
        for (double c : {0.0, 0.0, 0.0, 10.0, 5.0, 0.0})
            o.main.bd(c);
        commonHandles(o, 2, 0, kLayer0);
        f.add(0x41, o.finish());
    }
    gR2004 = gR2007 = gR2013 = false;
    Dwg2004File c;
    std::vector<std::uint8_t> objects, handles;
    f.sections(objects, handles);
    c.section("AcDb:AcDbObjects", objects);
    c.section("AcDb:Handles", handles, 0x80);
    dxf::Report report;
    auto r = importDwg(asBytes(c.build("AC1032")), dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const Document& doc = *r.value().document;
    int splines = 0;
    for (EntityId id : doc.modelSpace())
        if (const auto* d = entityAs<SplineData>(*doc.find(id))) {
            ++splines;
            CHECK(d->fit.size() == 2);
        }
    CHECK(splines == 1);
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

TEST_CASE("an XCLIP boundary on an insert comes through as the insert's clip") {
    DwgFile f;
    const int filterType = f.addClass("SPATIAL_FILTER");
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    f.add(kDoor, blockHeader(kDoor, "DOOR", 0, 0));
    f.add(0x30, line(0x30, 0, kDoor, 0, 0, 0, 10));
    f.add(0x90, dictionaryObj(0x90, 0x60, {{"ACAD_FILTER", 0x91}}));
    f.add(0x91, dictionaryObj(0x91, 0x90, {{"SPATIAL", 0x92}}));
    f.add(0x92, spatialFilterObj(0x92, filterType, 0x91, {{-5, -5}, {5, -5}, {5, 15}, {-5, 15}}));
    f.add(0x60, insertWithXdic(0x60, 0x90));
    dxf::Report report;
    auto r = importDwg(asBytes(f.build()), dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const Document& doc = *r.value().document;
    int inserts = 0;
    for (EntityId id : doc.modelSpace()) {
        const Entity& e = *doc.find(id);
        if (kind(e) != EntityKind::Insert)
            continue;
        ++inserts;
        const auto* d = entityAs<InsertData>(e);
        REQUIRE(d->clip.size() == 4);
        CHECK(d->clip[0] == Vec2{-5, -5});
        CHECK(d->clip[2] == Vec2{5, 15});
        CHECK(d->clipOn);
        CHECK_FALSE(d->clipInvert);
    }
    CHECK(inserts == 1);
}

TEST_CASE("a block's draw order table puts its entities in the order of the sort handles") {
    DwgFile f;
    const int sortType = f.addClass("SORTENTSTABLE");
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    f.add(kDoor, blockHeader(kDoor, "DOOR", 0, 0));
    f.add(0x30, line(0x30, 0, kDoor, 0, 0, 0, 10));  // the outline, first in the file
    f.add(0x31, line(0x31, 0, kDoor, 5, 0, 5, 10));  // the fill, sent behind it
    f.add(0x92, sortEntsObj(0x92, sortType, 0x91, kDoor, {{0x30, 0x200}, {0x31, 0x100}}));
    dxf::Report report;
    auto r = importDwg(asBytes(f.build()), dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const Document& doc = *r.value().document;
    const BlockDef* door = nullptr;
    for (const auto& [id, b] : doc.blocks())
        if (b.name == "DOOR")
            door = &b;
    REQUIRE(door);
    REQUIRE(door->entities.size() == 2);
    const auto* first = entityAs<LineData>(*doc.find(door->entities[0]));
    REQUIRE(first);
    CHECK(first->a.x == doctest::Approx(5));
    for (const auto& w : report.diagnostics())
        CHECK(w.message.find("sort table") == std::string::npos);
}

TEST_CASE("the linetype generation flag (128) of a lightweight and a heavy polyline is kept") {
    for (bool plinegen : {false, true}) {
        DwgFile f;
        f.add(kLtype, linetype(kLtype, "Continuous"));
        f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
        f.add(kStyle, style(kStyle, "Standard"));
        f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
        f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
        f.add(0x23, lwpolyline(0x23, true, plinegen));
        f.add(0x50, polylineObj(0x50, 0x51, 0x53, plinegen));
        f.add(0x51, vertexObj(0x51, 0, 0, 0));
        f.add(0x52, vertexObj(0x52, 10, 0, 0));
        f.add(0x53, vertexObj(0x53, 10, 10, 0));
        dxf::Report report;
        auto r = importDwg(asBytes(f.build()), dxf::ImportOptions{}, report);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
        int polys = 0;
        for (EntityId id : r.value().document->modelSpace()) {
            const Entity& e = *r.value().document->find(id);
            if (kind(e) != EntityKind::Polyline)
                continue;
            ++polys;
            CHECK(entityAs<PolylineData>(e)->restartLinetype == !plinegen);
        }
        CHECK(polys == 2);
    }
}

TEST_CASE("a hatch stored under its fixed type number 78 imports") {
    DwgFile f;
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    f.add(0x70, hatchObj(0x70, 78));
    dxf::Report report;
    auto r = importDwg(asBytes(f.build()), dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    int hatches = 0;
    for (EntityId id : r.value().document->modelSpace())
        if (kind(*r.value().document->find(id)) == EntityKind::Hatch)
            ++hatches;
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

// Open time of a big drawing: lines, lightweight polylines and circles, with the stage timings
// the reader reports. Size: TBCAD_BENCH_N objects of each kind (default 700, a quick check).
TEST_CASE("perf: open of a big DWG") {
    const char* env = std::getenv("TBCAD_BENCH_N");
    const int n = env ? std::atoi(env) : 700;
    DwgFile f;
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kWalls, layer(kWalls, "WALLS", 3, kLtype, 16, true));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    std::uint64_t h = 0x1000;
    for (int i = 0; i < n; ++i) {
        const double x = (i % 500) * 30.0, y = (i / 500) * 30.0;
        f.add(h, line(h, 2, 0, x, y, x + 20, y + 10));
        ++h;
        f.add(h, lwpolyline(h, i % 2 == 0));
        ++h;
        f.add(h, circle(h, x, y, 5, kWalls));
        ++h;
    }
    const std::vector<std::uint8_t> file = f.build();
    dxf::Report report;
    dxf::ImportOptions options;
    options.diagnostics = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto r = importDwg(asBytes(file), options, report);
    const double total =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    CHECK(r.value().document->modelSpaceCount() == static_cast<std::size_t>(3 * n));
    for (const dxf::Diagnostic& d : report.diagnostics())
        if (d.message.rfind("timing (ms)", 0) == 0 || d.message.rfind("read stages", 0) == 0)
            std::printf("  %s\n", d.message.c_str());
    std::printf("  open of %d objects (%zu KB file): %.0f ms\n", 3 * n, file.size() / 1024, total);
}

TEST_CASE("an entity of a class the reader does not know is reported as not shown") {
    DwgFile f;
    const int wipeType = f.addClass("WIPEOUT");
    f.add(kLtype, linetype(kLtype, "Continuous"));
    f.add(kLayer0, layer(kLayer0, "0", 7, kLtype));
    f.add(kStyle, style(kStyle, "Standard"));
    f.add(kModel, blockHeader(kModel, "*Model_Space", 0, 0));
    f.add(kPaper, blockHeader(kPaper, "*Paper_Space", 0, 0));
    ObjectWriter o(wipeType, 0x70);
    o.entityCommon(2, 256, 0);
    f.add(0x70, o.finish());
    dxf::Report report;
    auto r = importDwg(asBytes(f.build()), dxf::ImportOptions{}, report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    bool told = false;
    for (const auto& d : report.diagnostics())
        if (d.severity == dxf::Severity::Warning &&
            d.message.find("WIPEOUT x1") != std::string::npos)
            told = true;
    CHECK(told);
}
