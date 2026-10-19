// Every entity kind through the exporter and back, plus patterns, dimensions and colours.
#include <algorithm>
#include <set>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "semantic_dump.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/gdt/gdt.h"
#include "tbcad/hatch/pattern.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/underlay/image.h"

namespace tbcad::dxf {
namespace {

template <class T>
Entity make(T data, LayerId layer = {}) {
    Entity e;
    e.data = std::move(data);
    e.common.layer = layer;
    return e;
}

PolyVertex pv(double x, double y, double bulge = 0, double sw = 0, double ew = 0) {
    PolyVertex v;
    v.p = {x, y};
    v.bulge = bulge;
    v.startWidth = sw;
    v.endWidth = ew;
    return v;
}

struct RoundTrip {
    std::string bytes{};
    std::unique_ptr<Document> back{};
    Report exportReport{};
    std::string diff{};
};

RoundTrip roundTrip(const Document& doc, ExportOptions options = {}) {
    RoundTrip rt;
    Result<ExportResult> out = exportDxf(doc, options, rt.exportReport);
    REQUIRE(out.ok());
    rt.bytes = out.value().bytes;
    Report importReport;
    Result<ImportResult> in = importDxf(testing::bytesOf(rt.bytes), ImportOptions{}, importReport);
    REQUIRE_MESSAGE(in.ok(), (in.ok() ? "" : in.error().message));
    rt.back = std::move(in.value().document);
    rt.diff =
        testing::SemanticDump::diff(testing::SemanticDump(doc), testing::SemanticDump(*rt.back));
    return rt;
}

}  // namespace

TEST_CASE("all geometry kinds, properties and tables survive an export and re-import") {
    Document doc;
    {
        auto tx = doc.begin("everything");
        Linetype dash;
        dash.name = "DASH";
        dash.description = "Dashed";
        dash.pattern = {10, -5};
        LinetypeId dashId = tx.addLinetype(dash).value();
        Linetype dots;
        dots.name = "DOTS";
        dots.pattern = {0, -4, 2, -3};
        REQUIRE(tx.addLinetype(dots).ok());
        TextStyle arial;
        arial.name = "Arial";
        arial.fontFile = "arial.ttf";
        arial.widthFactor = 0.8;
        arial.oblique = 0.2;
        arial.height = 4;
        TextStyleId arialId = tx.addTextStyle(arial).value();
        DimStyle arch;
        arch.name = "Arch";
        arch.textHeight = 3;
        arch.arrowSize = 2;
        arch.textGap = 1;
        arch.extLineOffset = 0.5;
        arch.extLineExtension = 1;
        arch.dimLineExtension = 0.25;
        arch.overallScale = 50;
        arch.precision = 1;
        DimStyleId archId = tx.addDimStyle(arch).value();

        Layer walls;
        walls.name = "WALLS";
        walls.color = Color::rgb(200, 30, 60);
        walls.linetype = dashId;
        walls.lineweight = LineWeight::hundredths(70);
        walls.locked = true;
        LayerId wallsId = tx.addLayer(walls).value();
        Layer notes;
        notes.name = "NOTES";
        notes.color = Color::index(3);
        notes.plot = false;
        LayerId notesId = tx.addLayer(notes).value();
        Layer off;
        off.name = "OFF";
        off.color = Color::index(40);
        off.on = false;
        off.frozen = true;
        LayerId offId = tx.addLayer(off).value();

        BlockDef chair;
        chair.name = "CHAIR";
        chair.base = {1, 1};
        BlockId chairId = tx.addBlock(chair).value();
        REQUIRE(tx.add(make(LineData{{0, 0}, {4, 0}}), chairId).ok());
        REQUIRE(tx.add(make(CircleData{{2, 2}, 1}), chairId).ok());
        BlockDef set;
        set.name = "SET";
        BlockId setId = tx.addBlock(set).value();
        InsertData nestedIns;
        nestedIns.block = chairId;
        nestedIns.position = {5, 5};
        nestedIns.scale = {0.5, 2};
        nestedIns.rotation = 1.2;
        REQUIRE(tx.add(make(Indirect<InsertData>(nestedIns)), setId).ok());

        auto addTo = [&](Entity e, LayerId layer) {
            e.common.layer = layer;
            REQUIRE(tx.add(std::move(e)).ok());
        };
        Entity line = make(LineData{{0, 0}, {100, 50}});
        line.common.color = Color::rgb(1, 2, 3);
        line.common.lineweight = LineWeight::hundredths(35);
        line.common.linetype = dashId;
        line.common.linetypeScale = 2.5;
        addTo(line, wallsId);
        Entity hidden = make(LineData{{0, 1}, {10, 1}});
        hidden.common.visible = false;
        hidden.common.color = Color::byBlock();
        hidden.common.linetype = kLinetypeByBlock;
        hidden.common.lineweight = LineWeight::defaultWeight();
        addTo(hidden, offId);
        addTo(make(CircleData{{50, 50}, 12.5}), notesId);
        addTo(make(ArcData{{50, 50}, 30, 0.3, 4.0}), notesId);
        PolylineData pl;
        pl.v = {pv(0, 0, 0.5, 1, 2), pv(10, 0, -0.3), pv(10, 10), pv(0, 10)};
        pl.closed = true;
        pl.constantWidth = 0.4;
        addTo(make(pl), wallsId);
        addTo(make(PointData{{3, 4}}), notesId);
        EllipseData el;
        el.center = {20, 20};
        el.majorAxis = {10, 5};
        el.ratio = 0.4;
        el.startParam = 0.5;
        el.endParam = 4.5;
        addTo(make(Indirect<EllipseData>(el)), notesId);
        EllipseData full;
        full.center = {40, 20};
        full.majorAxis = {0, 8};
        full.ratio = 0.75;
        addTo(make(Indirect<EllipseData>(full)), notesId);
        SplineData sp;
        sp.degree = 3;
        sp.control = {{0, 0}, {5, 8}, {10, -3}, {15, 6}, {20, 0}};
        sp.knots = {0, 0, 0, 0, 1, 2, 2, 2, 2};
        sp.weights = {1, 2, 1, 0.5, 1};
        addTo(make(Indirect<SplineData>(sp)), notesId);
        SplineData open;
        open.degree = 2;
        open.control = {{0, 20}, {5, 28}, {10, 20}, {15, 28}};
        open.knots = {0, 0, 0, 1, 2, 2, 2};
        addTo(make(Indirect<SplineData>(open)), notesId);

        TextData t;
        t.position = {10, 60};
        t.alignPoint = t.position;
        t.height = 3;
        t.rotation = 0.5;
        t.widthFactor = 0.9;
        t.oblique = 0.1;
        t.style = arialId;
        t.utf8 = "Hello \xC2\xB0 world";
        addTo(make(Indirect<TextData>(t)), notesId);
        for (HAlign h :
             {HAlign::Center, HAlign::Right, HAlign::Aligned, HAlign::Middle, HAlign::Fit}) {
            TextData a;
            a.position = {10, 70};
            a.alignPoint = {40, 72};
            a.height = 2.5;
            a.h = h;
            a.v = h == HAlign::Middle ? VAlign::Middle : VAlign::Baseline;
            a.utf8 = "aligned";
            addTo(make(Indirect<TextData>(a)), notesId);
        }
        TextData top;
        top.position = {10, 80};
        top.alignPoint = {12, 81};
        top.height = 2;
        top.h = HAlign::Left;
        top.v = VAlign::Top;
        top.utf8 = "top";
        addTo(make(Indirect<TextData>(top)), notesId);

        MTextData m;
        m.position = {100, 100};
        m.height = 2.5;
        m.width = 40;
        m.rotation = 0.7;
        m.attach = Attachment::BottomCenter;
        m.style = arialId;
        m.utf8 =
            "plain {\\b1;bold} {\\i1;italic} {\\b1;\\i1;both}\n\\H4;big\\H2x; \\C1;red \\\\ \\{ "
            "\\}";
        addTo(make(Indirect<MTextData>(m)), notesId);
        MTextData longText;
        longText.position = {100, 140};
        longText.height = 2;
        std::string body;
        for (int i = 0; i < 80; ++i)
            body += "สวัสดี word" + std::to_string(i) + " ";
        longText.utf8 = body;
        addTo(make(Indirect<MTextData>(longText)), notesId);

        InsertData ins;
        ins.block = setId;
        ins.position = {200, 10};
        ins.scale = {2, 3};
        ins.rotation = 0.3;
        addTo(make(Indirect<InsertData>(ins)), wallsId);
        InsertData mirrored;
        mirrored.block = chairId;
        mirrored.position = {220, 10};
        mirrored.scale = {-1, 1};
        addTo(make(Indirect<InsertData>(mirrored)), wallsId);

        LeaderData ld;
        ld.vertices = {{0, 0}, {10, 10}, {20, 10}};
        ld.style = archId;
        ld.textPos = ld.vertices.back();  // where the importer puts it
        addTo(make(Indirect<LeaderData>(ld)), notesId);

        // Dimensions of every kind (the exporter draws their *D blocks).
        auto addDim = [&](DimKind k, std::array<Vec2, 4> pts, double rot, double measurement) {
            DimensionData d;
            d.kind = k;
            d.rotation = rot;
            d.defPts = pts;
            d.textPos = {pts[2].x, pts[2].y + 2};
            d.textPosUser = k == DimKind::Aligned;
            d.textOverride = k == DimKind::Radius ? "R<> typ" : "";
            d.style = archId;
            d.measurement = measurement;
            addTo(make(Indirect<DimensionData>(d)), notesId);
        };
        addDim(DimKind::Linear, {Vec2{0, 0}, Vec2{100, 0}, Vec2{0, -10}, Vec2{}}, 0, 100);
        addDim(DimKind::Linear, {Vec2{0, 0}, Vec2{0, 60}, Vec2{-10, 0}, Vec2{}}, kPi / 2, 60);
        addDim(DimKind::Aligned, {Vec2{0, 0}, Vec2{30, 40}, Vec2{25, 15}, Vec2{}}, 0, 50);
        addDim(DimKind::Radius, {Vec2{50, 50}, Vec2{62, 50}, Vec2{}, Vec2{}}, 0, 12);
        addDim(DimKind::Diameter, {Vec2{50, 50}, Vec2{70, 50}, Vec2{}, Vec2{}}, 0, 40);
        addDim(DimKind::Angular2Line, {Vec2{0, 0}, Vec2{10, 0}, Vec2{0, 0}, Vec2{0, 10}}, 0,
               kPi / 2);
        addDim(DimKind::Angular3Point, {Vec2{10, 0}, Vec2{0, 10}, Vec2{0, 0}, Vec2{7, 7}}, 0,
               kPi / 2);

        // Hatches: solid with an island, a pattern, and every edge type.
        HatchData solid;
        solid.solid = true;
        for (int i = 0; i < 2; ++i) {
            HatchPolylineEdge e;
            e.closed = true;
            double s = i == 0 ? 50 : 10;
            e.v = {pv(100 + (i ? 20 : 0), 0 + (i ? 20 : 0)),
                   pv(100 + (i ? 20 : 0) + s, 0 + (i ? 20 : 0)),
                   pv(100 + (i ? 20 : 0) + s, 0 + (i ? 20 : 0) + s, 0.4),
                   pv(100 + (i ? 20 : 0), 0 + (i ? 20 : 0) + s)};
            HatchLoop loop;
            loop.outer = i == 0;
            loop.edges.emplace_back(e);
            solid.loops.push_back(loop);
        }
        addTo(make(Indirect<HatchData>(solid)), notesId);
        HatchData pattern;
        pattern.solid = false;
        pattern.pattern = "LINE45";
        pattern.patternScale = 2;
        pattern.patternAngle = 0.5;
        {
            HatchPolylineEdge e;
            e.closed = true;
            e.v = {pv(200, 100), pv(250, 100), pv(250, 150), pv(200, 150)};
            HatchLoop loop;
            loop.edges.emplace_back(e);
            pattern.loops.push_back(loop);
        }
        addTo(make(Indirect<HatchData>(pattern)), notesId);
        HatchData edges;
        edges.solid = true;
        {
            HatchLoop loop;
            loop.edges.emplace_back(HatchLineEdge{{300, 0}, {340, 0}});
            HatchArcEdge arc;
            arc.center = {340, 20};
            arc.radius = 20;
            arc.startAngle = normalizeAngle(-kPi / 2);
            arc.endAngle = kPi / 2;
            loop.edges.emplace_back(arc);
            loop.edges.emplace_back(HatchLineEdge{{340, 40}, {300, 40}});
            loop.edges.emplace_back(HatchLineEdge{{300, 40}, {300, 0}});
            edges.loops.push_back(loop);
            HatchLoop ellipse;
            HatchEllipseEdge ee;
            ee.center = {350, 100};
            ee.majorAxis = {20, 0};
            ee.ratio = 0.5;
            ee.startParam = 0;
            ee.endParam = kTwoPi;
            ellipse.edges.emplace_back(ee);
            edges.loops.push_back(ellipse);
            HatchLoop spline;
            HatchSplineEdge se;
            se.spline.degree = 3;
            se.spline.control = {{300, 60}, {310, 90}, {330, 50}, {340, 60}};
            se.spline.knots = {0, 0, 0, 0, 1, 1, 1, 1};
            spline.edges.emplace_back(se);
            spline.edges.emplace_back(HatchLineEdge{{340, 60}, {300, 60}});
            edges.loops.push_back(spline);
        }
        addTo(make(Indirect<HatchData>(edges)), notesId);

        tx.commit();
    }
    {
        // The text position of a dimension is derived from its definition points and style.
        std::vector<EntityId> dims;
        doc.forEachEntity([&](const Entity& e) {
            if (entityAs<DimensionData>(e))
                dims.push_back(e.common.id);
        });
        Transaction tx = doc.begin("refresh dimensions");
        for (EntityId id : dims)
            REQUIRE(
                tx.modify(id, [&](Entity& e) { dim::refresh(doc, *entityAs<DimensionData>(e)); }));
        tx.commit();
    }

    RoundTrip rt = roundTrip(doc);
    CHECK_MESSAGE(rt.diff.empty(), rt.diff);
    CHECK(rt.exportReport.countCode(wcode::kNoDimensionGraphics) == 0);
    CHECK(rt.exportReport.count(Severity::Error) == 0);
}

TEST_CASE("every palette index and RGB colour keeps its look") {
    Document doc;
    {
        auto tx = doc.begin("colors");
        for (int i = 1; i <= 255; ++i) {
            Entity e = make(LineData{{static_cast<double>(i), 0}, {static_cast<double>(i), 1}});
            e.common.color = Color::index(static_cast<std::uint8_t>(i));
            REQUIRE(tx.add(std::move(e)).ok());
        }
        Entity rgb = make(LineData{{0, 5}, {1, 5}});
        rgb.common.color = Color::rgb(12, 34, 56);
        REQUIRE(tx.add(std::move(rgb)).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    std::vector<EntityId> before = doc.modelSpace(), after = rt.back->modelSpace();
    REQUIRE(before.size() == after.size());
    auto look = [](const Color& c) {
        return c.kind == ColorKind::Rgb ? Rgba{c.r, c.g, c.b, 255} : paletteColor(c.indexValue());
    };
    // The palette is the ACI table: every index keeps its number (plot styles map pens by it).
    for (std::size_t i = 0; i < before.size(); ++i) {
        const Color a = doc.find(before[i])->common.color;
        const Color b = rt.back->find(after[i])->common.color;
        CHECK_MESSAGE(look(a) == look(b), "colour " << int(a.r) << " changed its look");
        if (a.kind == ColorKind::Index)
            CHECK_MESSAGE(a == b, "palette index " << int(a.r) << " was not kept");
    }
}

TEST_CASE("pattern hatches embed their definition lines - a pattern library can be supplied") {
    Document doc;
    {
        auto tx = doc.begin("hatches");
        for (const char* name : {"LINE45", "CROSS45", "BRICK", "ANSI99", "MYPAT"}) {
            HatchData h;
            h.solid = false;
            h.pattern = name;
            h.patternScale = 2;
            h.patternAngle = kPi / 6;
            HatchPolylineEdge e;
            e.closed = true;
            e.v = {pv(0, 0), pv(10, 0), pv(10, 10)};
            HatchLoop loop;
            loop.edges.emplace_back(e);
            h.loops.push_back(loop);
            REQUIRE(tx.add(make(Indirect<HatchData>(h))).ok());
        }
        tx.commit();
    }
    auto patternRecords = [](const std::string& text) {
        std::map<std::string, std::vector<std::pair<int, std::string>>> out;
        std::vector<std::pair<int, std::string>> groups;
        std::size_t pos = 0;
        auto line = [&] {
            std::size_t end = text.find('\n', pos);
            std::string l = text.substr(pos, end - pos);
            pos = end + 1;
            if (!l.empty() && l.back() == '\r')
                l.pop_back();
            return l;
        };
        std::string current;
        bool inHatch = false;
        while (pos < text.size()) {
            int code = std::atoi(line().c_str());
            std::string value = line();
            if (code == 0)
                inHatch = value == "HATCH";
            if (inHatch && code == 2)
                current = value;
            if (inHatch)
                out[current].push_back({code, value});
        }
        return out;
    };
    Report report;
    Result<ExportResult> out = exportDxf(doc, {}, report);
    REQUIRE(out.ok());
    auto records = patternRecords(out.value().bytes);
    auto count = [&](const std::string& name, int code) {
        int n = 0;
        for (auto& [c, v] : records[name])
            n += c == code;
        return n;
    };
    CHECK(count("LINE45", 53) == 1);
    CHECK(count("CROSS45", 53) == 2);
    CHECK(count("BRICK", 53) == 3);
    CHECK(count("ANSI99", 53) == 0);  // unknown here: the name only
    CHECK(count("MYPAT", 53) == 0);
    CHECK(report.countCode(wcode::kUnknownPattern) == 2);

    // LINE45 at scale 2, angle 30 degrees: the family runs at 75 degrees, 6 mm apart.
    double angle = 0, offX = 0, offY = 0;
    for (auto& [c, v] : records["LINE45"]) {
        if (c == 53)
            angle = std::atof(v.c_str());
        if (c == 45)
            offX = std::atof(v.c_str());
        if (c == 46)
            offY = std::atof(v.c_str());
    }
    CHECK(angle == doctest::Approx(75));
    CHECK(std::hypot(offX, offY) == doctest::Approx(6));
    CHECK(offX * std::cos(75 * kPi / 180) + offY * std::sin(75 * kPi / 180) ==
          doctest::Approx(0).epsilon(1e-9));

    ExportOptions options;
    options.patterns = [](std::string_view name) -> std::optional<std::vector<HatchPatternLine>> {
        if (name == "MYPAT")
            return std::vector<HatchPatternLine>{{10, 1, 2, 0, 5, {4, -1}}};
        return std::nullopt;
    };
    Report report2;
    Result<ExportResult> out2 = exportDxf(doc, options, report2);
    REQUIRE(out2.ok());
    auto records2 = patternRecords(out2.value().bytes);
    int mypat = 0;
    for (auto& [c, v] : records2["MYPAT"])
        mypat += c == 53;
    CHECK(mypat == 1);

    // The patterns read back as the same hatches.
    RoundTrip rt = roundTrip(doc);
    CHECK_MESSAGE(rt.diff.empty(), rt.diff);
}

TEST_CASE("imported dimensions are written with their graphics in a fresh anonymous block") {
    std::vector<std::byte> bytes =
        testing::readFile(testing::corpusPath("reader/r2000_dimensions.dxf"));
    Report importReport;
    Result<ImportResult> in = importDxf(bytes, ImportOptions{}, importReport);
    REQUIRE(in.ok());
    const Document& doc = *in.value().document;
    std::size_t dims = 0;
    doc.forEachEntity([&](const Entity& e) { dims += kind(e) == EntityKind::Dimension; });
    REQUIRE(dims > 0);

    Report report;
    Result<ExportResult> out = exportDxf(doc, {}, report);
    REQUIRE(out.ok());
    CHECK(report.countCode(wcode::kNoDimensionGraphics) == 0);
    CHECK(out.value().summary.entities["DIMENSION"] == dims);

    const std::string& text = out.value().bytes;
    // The dimension blocks written are *D1..*Dn, one per dimension, each referenced once.
    std::size_t blockDefs = 0, refs = 0;
    for (std::size_t i = 0; i + 1 < dims + 100; ++i) {
        std::string name = "*D" + std::to_string(i + 1);
        std::size_t first = text.find("\r\n2\r\n" + name + "\r\n");
        if (first == std::string::npos)
            continue;
        ++blockDefs;
        // once in BLOCK_RECORD? no: in BLOCK (group 2), in DIMENSION (group 2) and BLOCK_RECORD (2)
        std::size_t n = 0, pos = 0;
        while ((pos = text.find("\r\n2\r\n" + name + "\r\n", pos)) != std::string::npos) {
            ++n;
            pos += 4;
        }
        refs += n;
    }
    CHECK(blockDefs == dims);
    CHECK(refs == 3 * dims);  // BLOCK_RECORD + BLOCK + DIMENSION

    // Graphics, text and the dimension itself come back (the old *D blocks are not copied).
    Report importAgain;
    Result<ImportResult> back = importDxf(testing::bytesOf(text), ImportOptions{}, importAgain);
    REQUIRE(back.ok());
    std::size_t proxies = 0, backDims = 0;
    back.value().document->forEachEntity([&](const Entity& e) {
        proxies += entityAs<ProxyData>(e) != nullptr;
        backDims += kind(e) == EntityKind::Dimension;
    });
    std::size_t origProxies = 0;
    doc.forEachEntity([&](const Entity& e) { origProxies += entityAs<ProxyData>(e) != nullptr; });
    CHECK(backDims == dims);
    CHECK(proxies == origProxies);
}

TEST_CASE("unknown proxy objects are kept, graphics-only proxies become plain entities") {
    Document doc;
    {
        auto tx = doc.begin("proxies");
        ProxyData raw;
        raw.originalType = "ACAD_PROXY_ENTITY";
        for (int i = 0; i < 300; ++i)
            raw.raw.push_back(static_cast<std::uint8_t>(i * 7));
        REQUIRE(tx.add(make(Indirect<ProxyData>(raw))).ok());
        ProxyData other;
        other.originalType = "MYSTERY";
        REQUIRE(tx.add(make(Indirect<ProxyData>(other))).ok());
        ProxyData graphic;
        graphic.originalType = "DIMENSION";
        DisplayPolyline p;
        p.pts = {{0, 0}, {5, 5}};
        p.style.color = Rgba{10, 20, 30, 255};
        p.style.lineweight = 0.25;
        p.style.layer = kLayerZero;
        graphic.graphics.items.emplace_back(p);
        REQUIRE(tx.add(make(Indirect<ProxyData>(graphic))).ok());
        tx.commit();
    }
    Report report;
    Result<ExportResult> out = exportDxf(doc, {}, report);
    REQUIRE(out.ok());
    CHECK(report.countCode(wcode::kSkippedEntity) == 1);
    Report importAgain;
    Result<ImportResult> back =
        importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, importAgain);
    REQUIRE(back.ok());
    std::size_t proxies = 0, polylines = 0;
    std::size_t rawBytes = 0;
    back.value().document->forEachEntity([&](const Entity& e) {
        if (const auto* p = entityAs<ProxyData>(e)) {
            ++proxies;
            rawBytes = p->raw.size();
        }
        polylines += kind(e) == EntityKind::Polyline;
    });
    CHECK(proxies == 1);
    CHECK(rawBytes == 300);
    CHECK(polylines == 1);
}

TEST_CASE("non-finite coordinates are written as zero with a warning") {
    Document doc;
    {
        auto tx = doc.begin("nan");
        // The document rejects non-finite input, so use a huge but finite value that formats fine.
        REQUIRE(tx.add(make(LineData{{0, 0}, {1e9, -1e9}})).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    CHECK_MESSAGE(rt.diff.empty(), rt.diff);
}

TEST_CASE("patterns of the W5-4 library, including the user's own, are embedded") {
    hatch::Pattern mine;
    mine.name = "DXFTESTPAT";
    hatch::PatternFamily f;
    f.angle = kPi / 2;
    f.delta = {0, 7};
    mine.families.push_back(f);
    hatch::PatternRegistry::add(mine);

    Document doc;
    {
        auto tx = doc.begin("hatch");
        HatchData h;
        h.solid = false;
        h.pattern = "dxftestpat";
        HatchPolylineEdge e;
        e.closed = true;
        e.v = {pv(0, 0), pv(10, 0), pv(10, 10)};
        HatchLoop loop;
        loop.edges.emplace_back(e);
        h.loops.push_back(loop);
        REQUIRE(tx.add(make(Indirect<HatchData>(h))).ok());
        tx.commit();
    }
    Report report;
    auto out = exportDxf(doc, ExportOptions{}, report);
    hatch::PatternRegistry::clearUser();
    REQUIRE(out.ok());
    CHECK(report.countCode(wcode::kUnknownPattern) == 0);
    CHECK(out.value().bytes.find("\r\n78\r\n1\r\n") != std::string::npos);
}

TEST_CASE("attribute definitions and values survive an export and re-import") {
    Document doc;
    {
        auto tx = doc.begin("attributes");
        BlockId b = tx.addBlock(BlockDef{.name = "TAGGED", .base = {1, 1}}).value();
        TextData def;
        def.position = {2, 3};
        def.alignPoint = def.position;
        def.height = 2;
        def.utf8 = "ROOM";
        def.attdef = AttDefInfo{"ROOM", "Room name", "none", false, false, true, false};
        REQUIRE(tx.add(make(def), b).ok());
        TextData hidden;
        hidden.position = {2, 5};
        hidden.alignPoint = hidden.position;
        hidden.utf8 = "ID";
        hidden.attdef = AttDefInfo{"ID", "", "x", true, false, false, true};
        REQUIRE(tx.add(make(hidden), b).ok());
        TextData fixed;
        fixed.position = {2, 7};
        fixed.alignPoint = fixed.position;
        fixed.utf8 = "K";
        fixed.attdef = AttDefInfo{"K", "", "const", false, true, false, false};
        REQUIRE(tx.add(make(fixed), b).ok());
        InsertData ins{b, {10, 10}, {1, 1}, 0};
        ins.attribs = {{"ROOM", "Kitchen"}, {"ID", "42"}};
        REQUIRE(tx.add(make(ins)).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    CHECK_MESSAGE(rt.diff.empty(), rt.diff);
    // The exported INSERT is followed by one ATTRIB per non-constant attribute and a SEQEND.
    std::size_t attribs = 0;
    for (std::size_t at = rt.bytes.find("ATTRIB"); at != std::string::npos;
         at = rt.bytes.find("ATTRIB", at + 1))
        ++attribs;
    CHECK(attribs == 2);
}

TEST_CASE("a dynamic block is exported as its current geometry") {
    Document doc;
    {
        auto tx = doc.begin("dynamic");
        BlockId b = tx.addBlock(BlockDef{.name = "BAR"}).value();
        EntityId line = tx.add(make(LineData{{0, 0}, {10, 0}}), b).value();
        DynamicDef def;
        DynParam w;
        w.name = "Length";
        w.base = {0, 0};
        w.end = {10, 0};
        def.params.push_back(w);
        DynAction move;
        move.kind = DynAction::Kind::Move;
        move.objects = {line};
        def.actions.push_back(move);
        REQUIRE(tx.modifyBlock(b, [&](BlockDef& d) { d.dynamic = std::move(def); }).ok());
        InsertData ins{b, {100, 0}, {1, 1}, 0};
        ins.dynValues = {13};
        REQUIRE(tx.add(make(ins)).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    std::string text = rt.bytes;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    // no INSERT of the dynamic block; the line of the current state is in the model space
    CHECK(text.find("\nINSERT\n") == std::string::npos);
    CHECK(text.find("\n10\n103.0") != std::string::npos);
}

TEST_CASE("an assembly is exported as its current geometry") {
    Document doc;
    {
        auto tx = doc.begin("assembly");
        BlockId b = tx.addBlock(BlockDef{.name = "SHELF"}).value();
        EntityId c = tx.add(make(CircleData{{0, 0}, 1}), b).value();
        AssemblyDef def;
        AsmParam n;
        n.kind = AsmParam::Kind::Integer;
        n.name = "N";
        n.defaultNumber = 2;
        def.params.push_back(n);
        (void)c;
        REQUIRE(tx.modifyBlock(b, [&](BlockDef& d) { d.assembly = std::move(def); }).ok());
        REQUIRE(tx.add(make(InsertData{b, {100, 0}, {1, 1}, 0})).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    std::string text = rt.bytes;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    CHECK(text.find("\nINSERT\n") == std::string::npos);
    CHECK(text.find("\nCIRCLE\n") != std::string::npos);
}

TEST_CASE("an associative array is exported as one INSERT per item") {
    Document doc;
    {
        auto tx = doc.begin("array");
        BlockId b = tx.addBlock(BlockDef{.name = "*A1", .anonymous = true}).value();
        REQUIRE(tx.add(make(CircleData{{0, 0}, 1}), b).ok());
        InsertData ins{b, {0, 0}, {1, 1}, 0};
        ArrayInfo a;
        a.rows = 2;
        a.columns = 3;
        a.rowSpacing = 10;
        a.columnSpacing = 5;
        ins.array = a;
        REQUIRE(tx.add(make(ins)).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    std::string text = rt.bytes;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    std::size_t inserts = 0;
    for (std::size_t at = text.find("\nINSERT\n"); at != std::string::npos;
         at = text.find("\nINSERT\n", at + 1))
        ++inserts;
    CHECK(inserts == 6);
}

TEST_CASE("exported ATTRIB records sit at the inserted position, scaled and turned") {
    Document doc;
    BlockId b;
    {
        auto tx = doc.begin("attributes");
        b = tx.addBlock(BlockDef{.name = "B", .base = {0, 0}}).value();
        TextData def;
        def.position = {1, 0};
        def.height = 1;
        def.utf8 = "T";
        def.attdef = AttDefInfo{"T", "", "d", false, false, false, false};
        REQUIRE(tx.add(make(def), b).ok());
        InsertData ins{b, {10, 0}, {2, 2}, kPi / 2};
        ins.attribs = {{"T", "v"}};
        REQUIRE(tx.add(make(ins)).ok());
        tx.commit();
    }
    Report rep;
    Result<ExportResult> out = exportDxf(doc, ExportOptions{}, rep);
    REQUIRE(out.ok());
    Report back;
    Result<ImportResult> in = importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, back);
    REQUIRE(in.ok());
    // The definition (1,0) is scaled to (2,0), turned 90 degrees to (0,2) and moved to (10,2).
    // The attribute has a matching ATTDEF in the block, so it comes back as an insert value; check
    // the raw record through the text instead.
    std::string text = out.value().bytes;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    std::size_t at = text.find("\nATTRIB\n");
    REQUIRE(at != std::string::npos);
    std::size_t end = text.find("\nSEQEND\n", at);
    REQUIRE(end != std::string::npos);
    std::string rec = text.substr(at, end - at);
    CHECK(rec.find("\n10\n10") != std::string::npos);
    CHECK(rec.find("\n40\n2.0\n") != std::string::npos);
    CHECK(rec.find("\n50\n90") != std::string::npos);
}

TEST_CASE("a moved attribute keeps its offset through DXF") {
    Document doc;
    BlockId b;
    {
        auto tx = doc.begin("attributes");
        b = tx.addBlock(BlockDef{.name = "B", .base = {1, 1}}).value();
        TextData def;
        def.position = {1, 0};
        def.height = 1;
        def.utf8 = "T";
        def.attdef = AttDefInfo{"T", "", "d", false, false, false, false};
        REQUIRE(tx.add(make(def), b).ok());
        InsertData ins{b, {10, 0}, {2, 3}, kPi / 3};
        ins.attribs = {{"T", "v", {1.5, -2}}};
        REQUIRE(tx.add(make(ins)).ok());
        InsertData home{b, {30, 0}, {1, 1}, 0};
        home.attribs = {{"T", "w", {}}};
        REQUIRE(tx.add(make(home)).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    int seen = 0;
    rt.back->forEachEntity([&](const Entity& e) {
        const auto* i = entityAs<InsertData>(e);
        if (!i || i->attribs.size() != 1)
            return;
        ++seen;
        const Vec2 want = i->attribs[0].value == "v" ? Vec2{1.5, -2} : Vec2{};
        CHECK(i->attribs[0].offset.x == doctest::Approx(want.x).epsilon(1e-6));
        CHECK(i->attribs[0].offset.y == doctest::Approx(want.y).epsilon(1e-6));
    });
    CHECK(seen == 2);
}

TEST_CASE("a dimension is exported with a drawn *D block that other programs can show") {
    Document doc;
    {
        Transaction tx = doc.begin("dimensions");
        DimensionData d;
        d.kind = DimKind::Linear;
        d.defPts = {Vec2{0, 0}, Vec2{100, 0}, Vec2{50, 10}, Vec2{}};
        d.style = kDimStyleStandard;
        Entity e;
        e.data = Indirect<DimensionData>(d);
        REQUIRE(tx.add(std::move(e)));
        tx.commit();
    }
    Report exportReport;
    Result<ExportResult> out = exportDxf(doc, ExportOptions{}, exportReport);
    REQUIRE(out.ok());
    CHECK(exportReport.countCode(wcode::kNoDimensionGraphics) == 0);

    ImportOptions opt;
    opt.dimensionGraphics = true;
    Report report;
    Result<ImportResult> in = importDxf(testing::bytesOf(out.value().bytes), opt, report);
    REQUIRE(in.ok());
    int proxies = 0, texts = 0;
    in.value().document->forEachEntity([&](const Entity& e) {
        if (const auto* p = entityAs<ProxyData>(e)) {
            ++proxies;
            // two extension lines, the dimension line and two arrowheads
            CHECK(p->graphics.items.size() >= 5);
        } else if (const auto* t = entityAs<TextData>(e)) {
            texts += t->utf8 == "100.00" ? 1 : 0;
        } else if (const auto* m = entityAs<MTextData>(e)) {
            texts += m->utf8.find("100.00") != std::string::npos ? 1 : 0;
        }
    });
    CHECK(proxies == 1);
    CHECK(texts >= 1);
}

TEST_CASE("a multileader is exported as leaders, a dogleg line and its note") {
    Document doc;
    {
        Transaction tx = doc.begin("mleader");
        MLeaderData m;
        m.lines = {{{0, 0}, {20, 10}}, {{0, 30}, {20, 10}}};
        m.landing = {20, 10};
        m.contentPos = {22.5, 10};
        m.utf8 = "Steel beam";
        m.style = kDimStyleStandard;
        REQUIRE(tx.add(make(Indirect<MLeaderData>(m))));
        tx.commit();
    }
    Report exportReport;
    Result<ExportResult> out = exportDxf(doc, ExportOptions{}, exportReport);
    REQUIRE(out.ok());
    CHECK(exportReport.countCode(wcode::kDowngraded) >= 1);
    Report report;
    Result<ImportResult> in =
        importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, report);
    REQUIRE(in.ok());
    int leaders = 0, lines = 0, texts = 0;
    in.value().document->forEachEntity([&](const Entity& e) {
        leaders += kind(e) == EntityKind::Leader;
        lines += kind(e) == EntityKind::Line;
        if (const auto* t = entityAs<MTextData>(e))
            texts += t->utf8.find("Steel beam") != std::string::npos;
    });
    CHECK(leaders == 2);
    CHECK(lines == 1);
    CHECK(texts == 1);
}

TEST_CASE("an xref block and an insert clipping boundary survive the DXF round trip") {
    Document doc;
    BlockId b;
    {
        auto tx = doc.begin("xref");
        b = tx.addBlock(BlockDef{.name = "site", .base = {0, 0}}).value();
        REQUIRE(tx.add(make(LineData{{0, 0}, {10, 0}}), b).ok());
        REQUIRE(tx.modifyBlock(b, [](BlockDef& d) {
                      d.xrefPath = "../plans/site.tbcad";
                      d.xrefStamp = 123456789;
                      d.xrefOverlay = true;
                  }).ok());
        InsertData ins{b, {5, 5}, {1, 1}, 0};
        ins.clip = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
        ins.clipOn = false;
        ins.clipInvert = true;
        REQUIRE(tx.add(make(ins)).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    const BlockDef* back = rt.back->blockByName("site");
    REQUIRE(back);
    CHECK(back->xrefPath == "../plans/site.tbcad");
    CHECK(back->xrefStamp == 123456789);
    CHECK(back->xrefOverlay);
    CHECK(back->entities.size() == 1);
    REQUIRE(rt.back->modelSpace().size() == 1);
    const auto* ins = entityAs<InsertData>(*rt.back->find(rt.back->modelSpace()[0]));
    REQUIRE(ins);
    CHECK(ins->clip.size() == 4);
    CHECK_FALSE(ins->clipOn);
    CHECK(ins->clipInvert);
    CHECK(ins->clip[2] == Vec2{4, 4});
}

TEST_CASE("ordinate dimensions keep their kind and datum through the DXF round trip") {
    Document doc;
    {
        Transaction tx = doc.begin("dimensions");
        for (double rot : {0.0, kPi / 2}) {
            DimensionData d;
            d.kind = DimKind::Ordinate;
            d.rotation = rot;
            d.defPts = {Vec2{40, 25}, Vec2{40, 60}, Vec2{0, 0}, Vec2{}};
            if (rot > 0)
                d.defPts[1] = Vec2{80, 25};
            d.style = kDimStyleStandard;
            d.measurement = dim::measurement(d);
            Entity e;
            e.data = Indirect<DimensionData>(d);
            REQUIRE(tx.add(std::move(e)));
        }
        DimensionData arc;
        arc.kind = DimKind::ArcLength;
        arc.defPts = {Vec2{0, 0}, Vec2{50, 0}, Vec2{0, 50}, Vec2{70, 70}};
        arc.style = kDimStyleStandard;
        Entity e;
        e.data = Indirect<DimensionData>(arc);
        REQUIRE(tx.add(std::move(e)));
        tx.commit();
    }
    Report exportReport;
    Result<ExportResult> out = exportDxf(doc, ExportOptions{}, exportReport);
    REQUIRE(out.ok());
    Report report;
    Result<ImportResult> in =
        importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, report);
    REQUIRE(in.ok());
    int x = 0, y = 0, others = 0;
    in.value().document->forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<DimensionData>(e)) {
            if (d->kind != DimKind::Ordinate)
                return;
            if (d->rotation < kPi / 4) {
                ++x;
                CHECK(d->measurement == doctest::Approx(40));
            } else {
                ++y;
                CHECK(d->measurement == doctest::Approx(25));
            }
        } else {
            ++others;  // the arc length dimension comes back as plain geometry
        }
    });
    CHECK(x == 1);
    CHECK(y == 1);
    CHECK(others >= 3);
}

TEST_CASE("a picture block is exported as IMAGE and IMAGEDEF and comes back with its settings") {
    Document doc;
    {
        auto tx = doc.begin("picture");
        BlockId b = tx.addBlock(BlockDef{.name = "logo", .base = {0, 0}}).value();
        PolylineData frame;
        frame.closed = true;
        for (Vec2 v : {Vec2{0, 0}, Vec2{40, 0}, Vec2{40, 20}, Vec2{0, 20}})
            frame.v.push_back({v, 0, 0, 0});
        REQUIRE(tx.add(make(frame), b).ok());
        REQUIRE(tx.modifyBlock(b, [](BlockDef& d) { d.xrefPath = "pics/logo.png"; }).ok());
        underlay::ImageParams ip;
        ip.fade = 40;
        ip.u0 = 0.25;
        ip.u1 = 0.75;
        ip.v0 = 0.25;
        ip.v1 = 0.75;
        ip.transparent = true;
        REQUIRE(underlay::setImageParams(tx, "logo", ip));
        REQUIRE(tx.add(make(InsertData{b, {100, 200}, {2, 2}, 0.5})).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    CHECK(rt.bytes.find("IMAGEDEF") != std::string::npos);
    CHECK(rt.bytes.find("AcDbRasterImage") != std::string::npos);
    const InsertData* ins = nullptr;
    rt.back->forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            ins = i;
    });
    REQUIRE(ins);
    CHECK(ins->position == Vec2{100, 200});
    CHECK(ins->scale.x == doctest::Approx(2));
    CHECK(ins->rotation == doctest::Approx(0.5));
    const BlockDef* b = rt.back->block(ins->block);
    REQUIRE(b);
    CHECK(b->xrefPath == "pics/logo.png");
    const underlay::ImageParams p = underlay::imageParams(rt.back->header(), b->name);
    CHECK(p.fade == doctest::Approx(40));
    CHECK(p.u0 == doctest::Approx(0.25));
    CHECK(p.v1 == doctest::Approx(0.75));
    CHECK(p.transparent);
}

TEST_CASE("MTEXT paragraph settings, column breaks and line spacing survive DXF") {
    Document doc;
    {
        auto tx = doc.begin("mtext");
        MTextData m;
        m.position = {10, 20};
        m.height = 3;
        m.width = 80;
        m.utf8 = "\\pxi-3,l6,qj,t12,24;A\tB\\PNext\\Nand\\~more";
        m.lineSpacing = 1.5;
        Entity e;
        e.data = Indirect<MTextData>(m);
        REQUIRE(tx.add(std::move(e)).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    const MTextData* back = nullptr;
    rt.back->forEachEntity([&](const Entity& e) {
        if (const auto* m = entityAs<MTextData>(e))
            back = m;
    });
    REQUIRE(back);
    CHECK(back->lineSpacing == doctest::Approx(1.5));
    CHECK(back->utf8.find("\\pxi-3,l6,qj,t12,24;") != std::string::npos);
    CHECK(back->utf8.find("\\N") != std::string::npos);
    CHECK(back->utf8.find("\xC2\xA0") != std::string::npos);
}

TEST_CASE("MTEXT background mask survives DXF") {
    Document doc;
    {
        auto tx = doc.begin("mtext");
        for (int mode : {1, 2}) {
            MTextData m;
            m.position = {10.0 * mode, 20};
            m.height = 3;
            m.utf8 = "boxed";
            m.bgMode = static_cast<std::uint8_t>(mode);
            m.bgColor = 3;
            m.bgScale = 2.0;
            Entity e;
            e.data = Indirect<MTextData>(m);
            REQUIRE(tx.add(std::move(e)).ok());
        }
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    int seen = 0;
    rt.back->forEachEntity([&](const Entity& e) {
        const auto* m = entityAs<MTextData>(e);
        if (!m)
            return;
        ++seen;
        CHECK(m->bgScale == doctest::Approx(2.0));
        if (m->position.x < 15) {
            CHECK(m->bgMode == 1);
        } else {
            CHECK(m->bgMode == 2);
            CHECK(m->bgColor == 3);
        }
    });
    CHECK(seen == 2);
}

TEST_CASE("a picture keeps its polygon clip, brightness and contrast through DXF") {
    Document doc;
    {
        auto tx = doc.begin("picture");
        BlockId b = tx.addBlock(BlockDef{.name = "logo", .base = {0, 0}}).value();
        PolylineData frame;
        frame.closed = true;
        for (Vec2 v : {Vec2{0, 0}, Vec2{40, 0}, Vec2{40, 20}, Vec2{0, 20}})
            frame.v.push_back({v, 0, 0, 0});
        REQUIRE(tx.add(make(frame), b).ok());
        REQUIRE(tx.modifyBlock(b, [](BlockDef& d) { d.xrefPath = "pics/logo.png"; }).ok());
        underlay::ImageParams ip;
        ip.brightness = 70;
        ip.contrast = 30;
        ip.frame = 0;
        ip.clip = {{0.25, 0.25}, {0.75, 0.25}, {0.5, 0.75}};
        ip.u0 = 0.25;
        ip.u1 = 0.75;
        ip.v0 = 0.25;
        ip.v1 = 0.75;
        REQUIRE(underlay::setImageParams(tx, "logo", ip));
        REQUIRE(tx.add(make(InsertData{b, {0, 0}, {1, 1}, 0})).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    const InsertData* ins = nullptr;
    rt.back->forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            ins = i;
    });
    REQUIRE(ins);
    const BlockDef* b = rt.back->block(ins->block);
    REQUIRE(b);
    const underlay::ImageParams p = underlay::imageParams(rt.back->header(), b->name);
    CHECK(p.brightness == 70);
    CHECK(p.contrast == 30);
    CHECK(p.frame == 0);  // RASTERVARIABLES says the frames are hidden
    CHECK(rt.bytes.find("RASTERVARIABLES") != std::string::npos);
    REQUIRE(p.clip.size() == 3);
    CHECK(p.clip[2].x == doctest::Approx(0.5));
    CHECK(p.clip[2].y == doctest::Approx(0.75));
    CHECK(p.u0 == doctest::Approx(0.25));
    CHECK(p.v1 == doctest::Approx(0.75));
}

TEST_CASE("a tolerance frame is exported as TOLERANCE and comes back as the same frame") {
    Document doc;
    gdt::Spec spec;
    spec.symbol = gdt::Symbol::Position;
    spec.value = "0.05";
    spec.diameterZone = true;
    spec.material = 'M';
    spec.datums = {"A", "B"};
    {
        auto tx = doc.begin("frame");
        BlockId b = tx.addBlock(BlockDef{.name = "TB_TOL_1"}).value();
        for (Entity& e : gdt::frameEntities(spec))
            REQUIRE(tx.add(std::move(e), b).ok());
        REQUIRE(gdt::addFrame(tx, "TB_TOL_1", spec));
        REQUIRE(tx.add(make(InsertData{b, {30, 40}, {1, 1}, 0.25})).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    CHECK(rt.bytes.find("TOLERANCE") != std::string::npos);
    CHECK(rt.bytes.find("AcDbFcf") != std::string::npos);
    CHECK(rt.bytes.find("TB_TOL_1") == std::string::npos);  // no block definition for it
    const InsertData* ins = nullptr;
    rt.back->forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            ins = i;
    });
    REQUIRE(ins);
    CHECK(ins->position == Vec2{30, 40});
    CHECK(ins->rotation == doctest::Approx(0.25));
    const BlockDef* b = rt.back->block(ins->block);
    REQUIRE(b);
    const gdt::FrameTable table = gdt::readFrames(rt.back->header());
    REQUIRE(table.count(b->name) == 1);
    CHECK(table.at(b->name) == spec);
    CHECK(b->entities.size() == gdt::frameEntities(spec).size());
}

TEST_CASE("a layer description travels as AcAecLayerStandard XDATA") {
    Document doc;
    {
        Transaction tx = doc.begin("t");
        Layer walls;
        walls.name = "WALLS";
        walls.description = "Load bearing walls, Thai: \xE0\xB8\x81";
        REQUIRE(tx.addLayer(walls).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    CHECK(rt.bytes.find("AcAecLayerStandard") != std::string::npos);
    REQUIRE(rt.back->layerByName("WALLS"));
    CHECK(rt.back->layerByName("WALLS")->description == "Load bearing walls, Thai: \xE0\xB8\x81");
}

TEST_CASE("transparency (group 440) and the object's own text width factor survive a round trip") {
    Document doc;
    EntityId lineId, textId;
    {
        Transaction tx = doc.begin("t");
        Layer glass;
        glass.name = "GLASS";
        glass.transparency = 40;
        LayerId glassId = tx.addLayer(glass).value();
        TextStyle narrow;
        narrow.name = "Narrow";
        narrow.fontFile = "arial.ttf";
        narrow.widthFactor = 0.8;
        TextStyleId narrowId = tx.addTextStyle(narrow).value();

        Entity line = make(LineData{{0, 0}, {100, 0}}, glassId);
        line.common.transparency = 70;
        lineId = tx.add(std::move(line)).value();
        Entity byBlock = make(LineData{{0, 5}, {100, 5}}, glassId);
        byBlock.common.transparency = kTransparencyByBlock;
        REQUIRE(tx.add(std::move(byBlock)).ok());
        TextData t;
        t.position = {0, 10};
        t.alignPoint = t.position;
        t.height = 5;
        t.style = narrowId;
        t.widthFactor = 1;  // relative to the style: 0.8 in the file
        t.utf8 = "plain";
        textId = tx.add(make(Indirect<TextData>(t), glassId)).value();
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    // The file states the full factor of the object (group 41 of the TEXT).
    CHECK(rt.bytes.find(std::to_string(0x02000000 | 77)) != std::string::npos);
    CHECK(rt.bytes.find("16777216") != std::string::npos);  // ByBlock
    CHECK(rt.back->layerByName("GLASS")->transparency == 40);
    int lines = 0;
    rt.back->forEachEntity([&](const Entity& e) {
        if (const auto* l = entityAs<LineData>(e); l && l->a.y == 0) {
            ++lines;
            CHECK(e.common.transparency == 70);
        }
        if (const auto* l = entityAs<LineData>(e); l && l->a.y == 5) {
            ++lines;
            CHECK(e.common.transparency == kTransparencyByBlock);
        }
        if (const auto* t = entityAs<TextData>(e)) {
            const TextStyle* ts = rt.back->textStyle(t->style);
            REQUIRE(ts);
            CHECK(ts->widthFactor * t->widthFactor == doctest::Approx(0.8));
        }
    });
    CHECK(lines == 2);
    (void)lineId;
    (void)textId;
}

TEST_CASE("polyline linetype generation (flag 128) and the linetype scale variables survive") {
    Document doc;
    {
        auto tx = doc.begin("gen");
        PolylineData pl;
        pl.v = {pv(0, 0), pv(10, 0), pv(10, 10)};
        PolylineData restarts = pl;
        restarts.restartLinetype = true;
        REQUIRE(tx.add(make(pl)).ok());
        REQUIRE(tx.add(make(restarts)).ok());
        REQUIRE(tx.modifyHeader([](Header& h) {
                      h.custom[kCeLtScaleKey] = "0.5";
                      h.custom[kPsLtScaleKey] = "0";
                      h.custom[kMsLtScaleKey] = "0";
                      h.custom[kPlineGenKey] = "0";
                  }).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    int runs = 0, restarts = 0;
    rt.back->forEachEntity([&](const Entity& e) {
        if (const auto* pl = entityAs<PolylineData>(e))
            (pl->restartLinetype ? restarts : runs)++;
    });
    CHECK(runs == 1);
    CHECK(restarts == 1);
    const Header& h = rt.back->header();
    CHECK(ceLtScale(h) == doctest::Approx(0.5));
    CHECK_FALSE(psLtScale(h));
    CHECK_FALSE(msLtScale(h));
    CHECK_FALSE(plineGen(h));
}

TEST_CASE("text and shapes in a linetype survive DXF with the pattern unchanged") {
    Document doc;
    {
        auto tx = doc.begin("marks");
        TextStyle ts;
        ts.name = "Notes";
        REQUIRE(tx.addTextStyle(ts).ok());
        Linetype lt;
        lt.name = "GAS";
        lt.pattern = {10, -4, 0, -6};
        LinetypeMark t;
        t.text = "GAS";
        t.style = "Notes";
        t.scale = 1.5;
        t.rotation = 0.5;
        t.rotationKind = LinetypeMark::Rotation::Absolute;
        t.offset = 14;
        t.shift = {-1, -0.5};
        LinetypeMark s;
        s.kind = LinetypeMark::Kind::Shape;
        s.text = "BOX";
        s.scale = 2;
        s.offset = 20;
        LinetypeMark first;
        first.text = "A";
        first.offset = 0;
        first.scale = 1;
        lt.marks = {first, t, s};
        REQUIRE(tx.addLinetype(lt).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    const Linetype* got = nullptr;
    for (const auto& [id, lt] : rt.back->linetypes())
        if (lt.name == "GAS")
            got = &lt;
    REQUIRE(got);
    CHECK(got->pattern == std::vector<double>{10, -4, 0, -6});
    REQUIRE(got->marks.size() == 3);
    CHECK(got->marks[0].text == "A");
    CHECK(got->marks[0].offset == doctest::Approx(0));
    CHECK(got->marks[1].text == "GAS");
    CHECK(got->marks[1].style == "Notes");
    CHECK(got->marks[1].scale == doctest::Approx(1.5));
    CHECK(got->marks[1].rotation == doctest::Approx(0.5));
    CHECK(got->marks[1].rotationKind == LinetypeMark::Rotation::Absolute);
    CHECK(got->marks[1].offset == doctest::Approx(14));
    CHECK(got->marks[1].shift.y == doctest::Approx(-0.5));
    CHECK(got->marks[2].kind == LinetypeMark::Kind::Shape);
    CHECK(got->marks[2].text == "BOX");
    CHECK(got->marks[2].offset == doctest::Approx(20));
}

TEST_CASE(
    "named coordinate systems and the axes of an ordinate dimension survive a DXF round trip") {
    Document doc;
    {
        auto tx = doc.begin("ucs");
        REQUIRE(tx.modifyHeader([](Header& h) {
                      h.custom["tbcad.ucs"] =
                          "Plan A\t120\t-45\t0.5235987755982988\nWorld2\t0\t7\t0\n";
                  }).ok());
        DimensionData d;
        d.kind = DimKind::Ordinate;
        d.rotation = 0;
        d.oblique = 30 * kPi / 180;
        d.defPts = {Vec2{50, 20}, Vec2{40, 60}, Vec2{10, 5}, Vec2{}};
        d.textPos = {40, 62};
        d.measurement = dim::measurement(d);
        REQUIRE(tx.add(make(d)).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    const std::string& text = rt.back->header().custom.at("tbcad.ucs");
    REQUIRE(text.find("Plan A") != std::string::npos);
    CHECK(text.find("World2") != std::string::npos);
    double x = 0, y = 0, a = 0;
    char name[40];
    REQUIRE(std::sscanf(text.c_str(), "%39[^\t]\t%lf\t%lf\t%lf", name, &x, &y, &a) == 4);
    CHECK(x == doctest::Approx(120));
    CHECK(y == doctest::Approx(-45));
    CHECK(a == doctest::Approx(30 * kPi / 180).epsilon(1e-9));
    bool found = false;
    rt.back->forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<DimensionData>(e)) {
            found = true;
            CHECK(d->kind == DimKind::Ordinate);
            CHECK(d->oblique == doctest::Approx(30 * kPi / 180).epsilon(1e-9));
            CHECK(d->measurement == doctest::Approx(dim::measurement(*d)));
        }
    });
    CHECK(found);
}

TEST_CASE("a shape font name and the annotative flag of a text style survive DXF") {
    Document doc;
    {
        auto tx = doc.begin("styles");
        TextStyle a;
        a.name = "Plan";
        a.shxFile = "romans.shx";
        a.bigFontFile = "bigfont.shx";
        a.fontFamily = "TBCAD Stroke";
        a.widthFactor = 0.9 / 0.92;
        a.annotative = true;
        REQUIRE(tx.addTextStyle(a).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    const TextStyle* got = rt.back->textStyleByName("Plan");
    REQUIRE(got);
    CHECK(got->shxFile == "romans.shx");
    CHECK(got->bigFontFile == "bigfont.shx");
    CHECK(got->annotative);
    CHECK(got->widthFactor == doctest::Approx(0.9 / 0.92));
}

TEST_CASE("drawing limits survive a DXF round trip") {
    Document doc;
    {
        auto tx = doc.begin("limits");
        REQUIRE(
            tx.modifyHeader([](Header& h) { setDrawingLimits(h, {-5, -6}, {1000, 700}); }).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    const BBox2 b = drawingLimits(rt.back->header());
    REQUIRE_FALSE(b.empty());
    CHECK(b.min.x == doctest::Approx(-5));
    CHECK(b.max.y == doctest::Approx(700));
}

TEST_CASE("FILL and ATTDISP survive a DXF round trip") {
    Document doc;
    {
        auto tx = doc.begin("modes");
        REQUIRE(tx.modifyHeader([](Header& h) {
                      h.custom[kFillModeKey] = "0";
                      h.custom[kAttDispKey] = "1";
                      h.custom[kQTextKey] = "1";
                  }).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    CHECK_FALSE(fillMode(rt.back->header()));
    CHECK(attDisplay(rt.back->header()) == 1);
    CHECK(quickText(rt.back->header()));
}

TEST_CASE("the wipeout frame mode survives a DXF round trip") {
    for (const char* mode : {"0", "1", "2"}) {
        Document doc;
        {
            auto tx = doc.begin("frame");
            REQUIRE(tx.modifyHeader([&](Header& h) { h.custom[kWipeoutFrameKey] = mode; }).ok());
            tx.commit();
        }
        RoundTrip rt = roundTrip(doc);
        CHECK(wipeoutFrameMode(rt.back->header()) == mode[0] - '0');
    }
}

TEST_CASE("draw order (front, back, wipeouts, inside blocks) survives a DXF round trip") {
    Document doc;
    BlockId blk{};
    {
        auto tx = doc.begin("order");
        std::vector<EntityId> ids;
        for (int i = 0; i < 5; ++i) {
            Entity e = make(LineData{{0, double(i)}, {10, double(i)}});
            if (i == 2) {  // a wipeout among the lines
                PolylineData p;
                p.closed = true;
                for (Vec2 v : {Vec2{0, 0}, Vec2{4, 0}, Vec2{4, 4}, Vec2{0, 4}})
                    p.v.push_back({v, 0, 0, 0});
                e = make(p);
                e.common.wipeout = true;
            }
            ids.push_back(tx.add(std::move(e)).value());
        }
        REQUIRE(tx.bringToFront(ids[0]).ok());
        REQUIRE(tx.sendToBack(ids[3]).ok());
        REQUIRE(tx.bringToFront(ids[2]).ok());  // the wipeout above the rest
        blk = tx.addBlock(BlockDef{.name = "B"}).value();
        REQUIRE(tx.add(make(LineData{{0, 7}, {1, 7}}), blk).ok());
        REQUIRE(tx.add(make(LineData{{0, 8}, {1, 8}}), blk).ok());
        tx.commit();
    }
    auto sig = [](const Document& d) {
        std::string s;
        for (EntityId id : d.modelSpace()) {
            const Entity* e = d.find(id);
            if (const auto* l = entityAs<LineData>(*e))
                s += char('0' + int(l->a.y));
            else
                s += e->common.wipeout ? 'W' : '?';
        }
        return s;
    };
    const std::string before = sig(doc);
    REQUIRE(before.size() == 5);
    CHECK(before.back() == 'W');
    CHECK(before.front() == '3');
    RoundTrip rt = roundTrip(doc);
    CHECK(sig(*rt.back) == before);
}

TEST_CASE("every dimension style rule survives the DXF and the .tbcad round trip") {
    Document doc;
    {
        auto tx = doc.begin("style");
        DimStyle s;
        s.name = "Rules";
        s.arrowType = ArrowType::ArchTick;
        s.altUnits = true;
        s.altPrecision = 3;
        s.altPrefix = "[";
        s.altSuffix = " in]";
        s.tolerance = DimTolerance::Deviation;
        s.tolUpper = 0.1;
        s.tolLower = 0.05;
        s.tolPrecision = 3;
        s.linearFactor = 2;
        s.fit = DimFit::TextFirst;
        s.textMove = DimTextMove::Leader;
        s.textAlign = DimTextAlign::Horizontal;
        s.textAlignOutside = DimTextAlign::WithDimLine;
        s.textInsideForced = true;
        s.suppressArrowsOutside = true;
        s.forceLineInside = false;
        s.suppressExt1 = true;
        s.suppressDim2 = true;
        s.textJustify = 2;
        s.angleFormat = DimAngleFormat::Dms;
        s.angularPrecision = 4;
        s.commaDecimal = true;
        s.baselineSpacing = 7;
        s.centerMark = -3;
        REQUIRE(tx.addDimStyle(s).ok());
        tx.commit();
    }
    auto check = [](const Document& d) {
        const DimStyle* s = d.dimStyleByName("Rules");
        REQUIRE(s);
        CHECK(s->arrowType == ArrowType::ArchTick);
        CHECK(s->altUnits);
        CHECK(s->altPrecision == 3);
        CHECK(s->tolerance == DimTolerance::Deviation);
        CHECK(s->tolUpper == doctest::Approx(0.1));
        CHECK(s->tolLower == doctest::Approx(0.05));
        CHECK(s->tolPrecision == 3);
        CHECK(s->linearFactor == doctest::Approx(2));
        CHECK(s->fit == DimFit::TextFirst);
        CHECK(s->textMove == DimTextMove::Leader);
        CHECK(s->textAlign == DimTextAlign::Horizontal);
        CHECK(s->textAlignOutside == DimTextAlign::WithDimLine);
        CHECK(s->textInsideForced);
        CHECK(s->suppressArrowsOutside);
        CHECK_FALSE(s->forceLineInside);
        CHECK(s->suppressExt1);
        CHECK_FALSE(s->suppressExt2);
        CHECK_FALSE(s->suppressDim1);
        CHECK(s->suppressDim2);
        CHECK(s->textJustify == 2);
        CHECK(s->angleFormat == DimAngleFormat::Dms);
        CHECK(s->angularPrecision == 4);
        CHECK(s->commaDecimal);
        CHECK(s->baselineSpacing == doctest::Approx(7));
        CHECK(s->centerMark == doctest::Approx(-3));
    };
    RoundTrip rt = roundTrip(doc);
    check(*rt.back);
    auto bytes = io::writeTbcadToMemory(doc);
    REQUIRE(bytes.ok());
    io::IoReport ioReport;
    auto reopened = io::readTbcad(bytes.value(), ioReport);
    REQUIRE(reopened.ok());
    check(*reopened.value());
}

TEST_CASE("named plot styles of layers and objects survive a DXF round trip") {
    Document doc;
    EntityId plain{}, own{};
    LayerId walls{};
    {
        auto tx = doc.begin("styles");
        Layer l;
        l.name = "Walls";
        walls = tx.addLayer(l).value();
        plain = tx.add(make(LineData{{0, 0}, {10, 0}}, walls)).value();
        own = tx.add(make(LineData{{0, 5}, {10, 5}}, walls)).value();
        REQUIRE(tx.modifyHeader([&](Header& h) {
                      setColorDependentPlotStyles(h, false);
                      setLayerPlotStyle(h, walls, "Heavy Black");
                      setEntityPlotStyle(h, own, "Thin Red");
                  }).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    CHECK(rt.bytes.find("$PSTYLEMODE") != std::string::npos);
    CHECK(rt.bytes.find("ACDBPLACEHOLDER") != std::string::npos);
    CHECK_FALSE(colorDependentPlotStyles(rt.back->header()));
    const Layer* back = rt.back->layerByName("Walls");
    REQUIRE(back);
    CHECK(layerPlotStyle(rt.back->header(), back->id) == "Heavy Black");
    std::vector<std::string> seen;
    for (EntityId id : rt.back->modelSpace())
        seen.push_back(resolvedPlotStyle(rt.back->header(), id, rt.back->find(id)->common.layer));
    std::sort(seen.begin(), seen.end());
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == "Heavy Black");
    CHECK(seen[1] == "Thin Red");
}

TEST_CASE("a drawing without named plot styles writes none") {
    Document doc;
    {
        auto tx = doc.begin("line");
        REQUIRE(tx.add(make(LineData{{0, 0}, {1, 0}})).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    CHECK(rt.bytes.find("$PSTYLEMODE") == std::string::npos);
    CHECK(colorDependentPlotStyles(rt.back->header()));
}

}  // namespace tbcad::dxf

namespace tbcad::dxf {
TEST_CASE(
    "TBCAD settings kept in the header (styles, layer states, ...) survive a DXF round trip") {
    std::string longValue;
    for (int i = 0; i < 700; ++i)
        longValue += "แถว" + std::to_string(i) + "\\x\t";
    longValue += "line1\nline2\r\nend";
    Document doc;
    {
        auto tx = doc.begin("k");
        REQUIRE(tx.modifyHeader([&](Header& h) {
                      h.custom["tbcad.mleaderstyle.Note"] = "Text";
                      h.custom["tbcad.mleadercurrent"] = "Note";
                      h.custom["tbcad.layerstate.Plan"] = longValue;
                      h.custom["tbcad.tablestyle.Grid"] = "a\tb\nc";
                  }).ok());
        tx.commit();
    }
    RoundTrip rt = roundTrip(doc);
    const auto& c = rt.back->header().custom;
    REQUIRE(c.count("tbcad.layerstate.Plan"));
    CHECK(c.at("tbcad.layerstate.Plan") == longValue);
    CHECK(c.at("tbcad.mleaderstyle.Note") == "Text");
    CHECK(c.at("tbcad.mleadercurrent") == "Note");
    CHECK(c.at("tbcad.tablestyle.Grid") == "a\tb\nc");
}
}  // namespace tbcad::dxf
