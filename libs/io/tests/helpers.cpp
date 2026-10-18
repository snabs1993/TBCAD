#include "helpers.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <random>

#include "tbcad/core/dump.h"

namespace tbcad::testing {

namespace {

template <class T>
T must(Result<T> r) {
    TB_REQUIRE_OK(r);
    return std::move(r).value();
}
void must(const Result<void>& r) {
    TB_REQUIRE_OK(r);
}

Entity make(auto data, LayerId layer) {
    Entity e;
    e.common.layer = layer;
    e.data = std::move(data);
    return e;
}

}  // namespace

std::unique_ptr<Document> makeRichDocument() {
    auto doc = std::make_unique<Document>();
    {
        Transaction tx = doc->begin("tables");
        // Linetypes (one erased again to leave an id gap).
        LinetypeId dashed = must(
            tx.addLinetype({.name = "DASHED", .description = "Dashed __ __", .pattern = {12, -6}}));
        LinetypeId tmp = must(tx.addLinetype({.name = "TMP", .pattern = {1, -1}}));
        LinetypeId center = must(tx.addLinetype({.name = "CENTER", .pattern = {32, -6, 6, -6}}));
        must(tx.eraseLinetype(tmp));
        // Text styles and dimension styles.
        TextStyleId thai = must(tx.addTextStyle({.name = "Thai",
                                                 .fontFamily = "Sarabun",
                                                 .fontFile = "Sarabun-Regular.ttf",
                                                 .widthFactor = 0.9,
                                                 .oblique = 0.1,
                                                 .height = 3.5}));
        must(tx.modifyTextStyle(kTextStyleStandard, [](TextStyle& t) { t.fontFamily = "Lato"; }));
        DimStyle arch;
        arch.name = "Arch";
        arch.textStyle = thai;
        arch.arrowType = ArrowType::ArchTick;
        arch.units = LengthUnit::Meter;
        arch.precision = 3;
        arch.prefix = "<";
        arch.suffix = ">";
        arch.suppressTrailingZeros = true;
        arch.dimLineColor = Color::rgb(1, 2, 3);
        DimStyleId archId = must(tx.addDimStyle(arch));
        DimStyle junk;
        junk.name = "junk";
        DimStyleId junkId = must(tx.addDimStyle(junk));
        must(tx.eraseDimStyle(junkId));
        (void)archId;

        // Layers: a tree with ByParent children, same child name under two parents, a Thai name,
        // a parent whose id is higher than its child's, and an erased layer for an id gap.
        LayerId floor =
            must(tx.addLayer({.name = "S-FLOOR", .color = Color::index(1), .linetype = dashed}));
        LayerId edge = must(tx.addLayer({.name = "edge",
                                         .parent = floor,
                                         .color = Color::byParent(),
                                         .linetype = kLinetypeByParent,
                                         .lineweight = LineWeight::byParent()}));
        LayerId hidden = must(tx.addLayer({.name = "Floor hidden",
                                           .parent = floor,
                                           .color = Color::rgb(10, 20, 30),
                                           .on = false}));
        LayerId wall = must(tx.addLayer({.name = "A-WALL",
                                         .color = Color::index(3),
                                         .lineweight = LineWeight::mm(0.35),
                                         .locked = true}));
        must(tx.addLayer({.name = "edge", .parent = wall, .color = Color::index(5)}));
        must(tx.addLayer({.name = "ห้องนอน", .color = Color::index(40), .plot = false}));
        LayerId junkLayer = must(tx.addLayer({.name = "junk"}));
        must(tx.eraseLayer(junkLayer));
        LayerId late = must(tx.addLayer({.name = "late child"}));
        LayerId lateParent = must(tx.addLayer({.name = "late parent", .frozen = true}));
        must(tx.modifyLayer(late, [&](Layer& l) {
            l.parent = lateParent;
            l.color = Color::byParent();
        }));
        (void)edge;
        (void)hidden;
        (void)center;

        // Blocks: Door, Door2 (nested insert of Door), an anonymous one and an erased one.
        BlockId door = must(tx.addBlock({.name = "Door", .base = {5, 5}}));
        BlockId junkBlock = must(tx.addBlock({.name = "junk"}));
        must(tx.eraseBlock(junkBlock));
        BlockId door2 = must(tx.addBlock({.name = "Door2", .base = {-1.5, 2.25}}));
        must(tx.addBlock({.name = "*U1", .anonymous = true}));
        must(tx.add(make(LineData{{0, 0}, {900, 0}}, kLayerZero), door));
        must(tx.add(make(CircleData{{450, 0}, 450}, kLayerZero), door));
        must(tx.add(make(InsertData{door, {10, 10}, {1, 1}, 0}, kLayerZero), door2));
        must(tx.add(make(TextData{.position = {0, 0}, .height = 20, .utf8 = "ประตู"}, kLayerZero),
                    door2));

        must(tx.modifyHeader([&](Header& h) {
            h.units = LengthUnit::Meter;
            h.workArea = 2e9;
            h.ltScale = 0.5;
            h.currentLayer = wall;
            h.currentTextStyle = thai;
            h.currentDimStyle = archId;
            h.createdBy = "TBCAD test ทดสอบ";
            h.custom["ui.grid"] = "on";
            h.custom["note"] = std::string("binary\0safe", 11);
        }));
        tx.commit();
    }
    {
        Transaction tx = doc->begin("entities");
        LayerId l0 = kLayerZero;
        LayerId floor = doc->layerByName("S-FLOOR")->id;
        LayerId wall = doc->layerByName("A-WALL")->id;
        LayerId edge = doc->layerByName(floor, "edge")->id;
        LinetypeId dashed = doc->linetypeByName("DASHED")->id;
        BlockId door = doc->blockByName("Door")->id;
        BlockId door2 = doc->blockByName("Door2")->id;
        TextStyleId thai = doc->textStyleByName("Thai")->id;
        DimStyleId arch = doc->dimStyleByName("Arch")->id;

        std::vector<EntityId> ids;
        auto add = [&](Entity e) { ids.push_back(must(tx.add(std::move(e)))); };

        add(make(LineData{{0, 0}, {1000, 500}}, l0));
        Entity styled = make(LineData{{1e8, 1e8}, {-1e8, 1e8 + 0.001}}, floor);  // far coordinates
        styled.common.color = Color::rgb(255, 128, 0);
        styled.common.linetype = dashed;
        styled.common.lineweight = LineWeight::hundredths(50);
        styled.common.linetypeScale = 2.5;
        add(std::move(styled));
        add(make(CircleData{{5e9, -5e9}, 12.5}, wall));  // beyond the work area, below the cap
        add(make(ArcData{{0, 0}, 100, 0.25, 4.5}, edge));
        add(make(
            PolylineData{
                {{{0, 0}, 0.5, 1, 2}, {{10, 0}, 0, 0, 0}, {{10, 10}, -1, 3, 4}}, true, false, 0.2},
            wall));
        add(make(PointData{{3, 4}}, l0));
        add(make(EllipseData{{1, 2}, {30, 0}, 0.5, 0.1, 3}, l0));
        add(make(SplineData{3,
                            {0, 0, 0, 0, 1, 1, 1, 1},
                            {{0, 0}, {1, 2}, {3, 2}, {4, 0}},
                            {1, 2, 3, 1},
                            {},
                            false,
                            false},
                 l0));
        add(make(TextData{{1, 1},
                          {2, 2},
                          3.5,
                          0.3,
                          0.8,
                          0.1,
                          thai,
                          HAlign::Center,
                          VAlign::Middle,
                          "น้ำ ปั่น ห้องนอน \"quoted\"\nline2"},
                 l0));
        add(make(
            MTextData{{5, 5}, 4, 100, 0.5, Attachment::MiddleCenter, thai, "{\\fSarabun;ทดสอบ}"},
            l0));
        HatchData hatch;
        hatch.solid = false;
        hatch.pattern = "ANSI31";
        hatch.patternScale = 2;
        hatch.patternAngle = 0.7;
        hatch.patternOrigin = {1, 1};
        hatch.fill = Color::index(9);
        HatchLoop loop;
        loop.edges.push_back(HatchLineEdge{{0, 0}, {10, 0}});
        loop.edges.push_back(HatchArcEdge{{10, 5}, 5, 4.71, 1.57, true});
        loop.edges.push_back(HatchEllipseEdge{{5, 5}, {3, 0}, 0.5, 0, 6.28, false});
        loop.edges.push_back(HatchSplineEdge{
            SplineData{2, {0, 0, 0, 1, 1, 1}, {{0, 0}, {1, 1}, {2, 0}}, {}, {}, false, false}});
        loop.edges.push_back(HatchPolylineEdge{{{{0, 0}, 0, 0, 0}, {{1, 1}, 0.3, 0, 0}}, false});
        hatch.loops.push_back(loop);
        HatchLoop hole;
        hole.outer = false;
        hole.edges.push_back(HatchLineEdge{{2, 2}, {3, 3}});
        hatch.loops.push_back(hole);
        add(make(std::move(hatch), l0));
        add(make(InsertData{door, {100, 100}, {2, -1}, 0.5}, l0));
        add(make(InsertData{door2, {-100, 100}, {1, 1}, 1.5}, floor));
        DimensionData dim;
        dim.kind = DimKind::Aligned;
        dim.rotation = 0.5;
        dim.defPts = {Vec2{0, 0}, Vec2{10, 0}, Vec2{10, 5}, Vec2{0, 5}};
        dim.textPos = {5, 6};
        dim.textPosUser = true;
        dim.textOverride = "<> mm";
        dim.style = arch;
        dim.measurement = 10;
        dim.breaks = {DimBreak{{1, 0}, {2, 0}}, DimBreak{{6, 0}, {7, 0}}};
        dim.oblique = 0.4;
        dim.textRotated = true;
        dim.textRotation = 0.25;
        dim.jogs = {Vec2{3, 0}, Vec2{4, 0}};
        add(make(std::move(dim), l0));
        add(make(LeaderData{{{0, 0}, {5, 5}, {8, 5}}, arch, "หมายเหตุ", {8.5, 5}}, l0));
        {
            MLeaderData ml;
            ml.lines = {{{0, 0}, {5, 5}}, {{0, 10}, {5, 5}}};
            ml.landing = {5, 5};
            ml.contentPos = {8, 5};
            ml.style = arch;
            ml.utf8 = "หมายเหตุ";
            ml.textHeight = 3;
            add(make(std::move(ml), l0));
        }
        {
            RayData ray;
            ray.origin = {1, 2};
            ray.dir = {0.6, 0.8};
            add(make(ray, l0));
            ray.infinite = true;
            add(make(ray, floor));
        }
        {
            MlineData ml;
            ml.style = "WALLS";
            ml.scale = 2.5;
            ml.justification = 2;
            ml.closed = true;
            ml.v = {{0, 0}, {10, 0}, {10, 8}, {0, 8}};
            add(make(std::move(ml), l0));
        }
        {
            TableData t;
            t.origin = {-20, 30};
            t.rotation = 0.3;
            t.colWidths = {10, 20, 15};
            t.rowHeights = {5, 6};
            t.cells.resize(6);
            t.cells[0].utf8 = "หัวตาราง";
            t.cells[0].colSpan = 2;
            t.cells[1].covered = true;
            t.cells[2].utf8 = "x";
            t.cells[3].utf8 = "a\\Pb";
            t.cells[3].rowSpan = 1;
            t.textHeight = 3;
            t.margin = 0.7;
            t.titleRow = true;
            add(make(std::move(t), l0));
        }
        ProxyData proxy;
        proxy.originalType = "ACAD_PROXY_ENTITY";
        proxy.raw = {0, 1, 2, 3, 254, 255};
        DisplayPolyline dp;
        dp.pts = {{0, 0}, {1, 1}};
        dp.closed = true;
        dp.style.color = {1, 2, 3, 4};
        dp.style.lineweight = 0.25;
        dp.style.layer = floor;
        proxy.graphics.items.emplace_back(dp);
        DisplayFill df;
        df.rings = {{{0, 0}, {1, 0}, {1, 1}}, {{0.2, 0.2}, {0.4, 0.2}, {0.4, 0.4}}};
        proxy.graphics.items.emplace_back(df);
        proxy.graphics.items.emplace_back(DisplayPoints{{{3, 3}}, {}});
        add(make(std::move(proxy), l0));
        Entity hiddenLine = make(LineData{{0, 0}, {1, 1}}, edge);
        hiddenLine.common.visible = false;
        add(std::move(hiddenLine));
        MTextData columns{{9, 9}, 3, 40, 0, Attachment::TopLeft, thai, "a\\Pb\\Pc\\Pd"};
        columns.columns = 2;
        columns.columnGap = 6;
        add(make(columns, l0));

        // Erase two entities (id gaps in the middle) and reorder the drawing.
        // (ids 1-4 belong to the block definitions, so these are entities 5 and 16.)
        must(tx.erase(ids[0]));
        must(tx.erase(ids[11]));
        must(tx.bringToFront(ids[1]));
        must(tx.sendToBack(ids[3]));
        must(tx.bringToFront(ids[7]));
        tx.commit();
    }
    return doc;
}

std::vector<std::uint8_t> writeOrFail(const Document& doc, const io::WriteOptions& o) {
    auto r = io::writeTbcadToMemory(doc, o);
    TB_REQUIRE_OK(r);
    return std::move(r).value();
}

std::unique_ptr<Document> readOrFail(const std::vector<std::uint8_t>& bytes, io::IoReport* report) {
    io::IoReport local;
    auto r = io::readTbcad(bytes, report ? *report : local);
    TB_REQUIRE_OK(r);
    return std::move(r).value();
}

std::vector<std::uint8_t> spliceChunkBeforeEnd(const std::vector<std::uint8_t>& file,
                                               const char (&type)[5], std::uint32_t flags,
                                               const std::vector<std::uint8_t>& payload) {
    // The END chunk is the last 20 bytes: fourcc, flags, u64 length 0, crc of nothing.
    constexpr std::size_t kEndSize = 20;
    REQUIRE(file.size() > kEndSize);
    std::vector<std::uint8_t> out(file.begin(), file.end() - kEndSize);
    auto le = [&](std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i)
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(type[i]));
    le(flags, 4);
    le(payload.size(), 8);
    out.insert(out.end(), payload.begin(), payload.end());
    // CRC-32 (IEEE) computed bitwise here so the test does not depend on the library's own.
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::uint8_t b : payload) {
        c ^= b;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    le(c ^ 0xFFFFFFFFu, 4);
    out.insert(out.end(), file.end() - kEndSize, file.end());
    return out;
}

TempDir::TempDir() {
    static std::atomic<int> counter{0};
    std::random_device rd;
    path_ = std::filesystem::temp_directory_path() /
            ("tbcad-io-test-" + std::to_string(rd()) + "-" + std::to_string(counter++));
    std::filesystem::create_directories(path_);
}
TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

}  // namespace tbcad::testing
