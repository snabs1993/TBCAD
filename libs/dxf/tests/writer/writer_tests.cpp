// Unit tests of the DXF exporter: file structure, names, versions, encodings, sub-layers.
#include <algorithm>
#include <map>
#include <set>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "semantic_dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"

namespace tbcad::dxf {
namespace {

struct G {
    int code;
    std::string value;
};

// Raw groups of an ASCII DXF text.
std::vector<G> groups(const std::string& dxf) {
    std::vector<G> out;
    std::size_t pos = 0;
    auto line = [&]() {
        std::size_t end = dxf.find('\n', pos);
        std::string l = dxf.substr(pos, end == std::string::npos ? end : end - pos);
        pos = end == std::string::npos ? dxf.size() : end + 1;
        if (!l.empty() && l.back() == '\r')
            l.pop_back();
        return l;
    };
    while (pos < dxf.size()) {
        std::string c = line();
        std::string v = line();
        out.push_back({std::atoi(c.c_str()), v});
    }
    return out;
}

// Records (groups from one "0" to the next) of a section.
std::vector<std::vector<G>> records(const std::vector<G>& all, const std::string& section) {
    std::vector<std::vector<G>> out;
    bool in = false;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].code == 0 && all[i].value == "SECTION" && all[i + 1].value == section) {
            in = true;
            ++i;
            continue;
        }
        if (in && all[i].code == 0 && all[i].value == "ENDSEC")
            break;
        if (!in)
            continue;
        if (all[i].code == 0)
            out.emplace_back();
        if (!out.empty())
            out.back().push_back(all[i]);
    }
    return out;
}

const G* find(const std::vector<G>& rec, int code) {
    for (const G& g : rec) {
        if (g.code == code)
            return &g;
    }
    return nullptr;
}

std::string get(const std::vector<G>& rec, int code) {
    const G* g = find(rec, code);
    return g ? g->value : std::string();
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::uint64_t hex(const std::string& s) {
    return std::strtoull(s.c_str(), nullptr, 16);
}

Result<ExportResult> exportDoc(const Document& doc, ExportOptions options = {}) {
    Report report;
    return exportDxf(doc, options, report);
}

ImportResult importText(const std::string& text, Report* report = nullptr,
                        ImportOptions options = {}) {
    Report local;
    Result<ImportResult> r = importDxf(testing::bytesOf(text), options, report ? *report : local);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return std::move(r.value());
}

std::string semanticDiff(const Document& a, const Document& b) {
    return testing::SemanticDump::diff(testing::SemanticDump(a), testing::SemanticDump(b));
}

template <class T>
Entity make(T data, LayerId layer = {}) {
    Entity e;
    e.data = std::move(data);
    e.common.layer = layer;
    return e;
}

}  // namespace

TEST_CASE("an empty document exports a complete, consistent R2000 skeleton") {
    Document doc;
    Result<ExportResult> out = exportDoc(doc);
    REQUIRE(out.ok());
    const std::string& text = out.value().bytes;
    std::vector<G> all = groups(text);

    // Section order and the closing EOF.
    std::vector<std::string> sections;
    for (std::size_t i = 0; i + 1 < all.size(); ++i) {
        if (all[i].code == 0 && all[i].value == "SECTION")
            sections.push_back(all[i + 1].value);
    }
    CHECK(sections ==
          std::vector<std::string>{"HEADER", "CLASSES", "TABLES", "BLOCKS", "ENTITIES", "OBJECTS"});
    CHECK(all.back().code == 0);
    CHECK(all.back().value == "EOF");

    // Header variables strict readers look for.
    std::vector<G> header;
    {
        bool in = false;
        for (std::size_t i = 0; i + 1 < all.size(); ++i) {
            if (all[i].code == 0 && all[i].value == "SECTION")
                in = all[i + 1].value == "HEADER";
            else if (in && all[i].code != 2)
                header.push_back(all[i]);
            if (all[i].code == 0 && all[i].value == "ENDSEC")
                in = false;
        }
    }
    auto var = [&](const std::string& name, int code) {
        const auto& rec = header;
        for (std::size_t i = 0; i + 1 < rec.size(); ++i) {
            if (rec[i].code == 9 && rec[i].value == name) {
                for (std::size_t j = i + 1; j < rec.size() && rec[j].code != 9; ++j) {
                    if (rec[j].code == code)
                        return rec[j].value;
                }
            }
        }
        return std::string("<missing>");
    };
    CHECK(var("$ACADVER", 1) == "AC1015");
    CHECK(var("$INSUNITS", 70) == "4");
    CHECK(var("$MEASUREMENT", 70) == "1");
    CHECK(var("$DWGCODEPAGE", 3) == "ANSI_1252");

    // Every object has a unique handle, every owner exists, $HANDSEED is above all of them.
    std::set<std::uint64_t> handles;
    std::uint64_t maxHandle = 0;
    for (const char* sec : {"TABLES", "BLOCKS", "ENTITIES", "OBJECTS"}) {
        for (const auto& rec : records(all, sec)) {
            for (const G& g : rec) {
                if (g.code == 5 || g.code == 105) {
                    CHECK_MESSAGE(handles.insert(hex(g.value)).second,
                                  "duplicate handle " << g.value);
                    maxHandle = std::max(maxHandle, hex(g.value));
                }
            }
        }
    }
    // Table handles sit on the TABLE records themselves.
    CHECK(hex(var("$HANDSEED", 5)) > maxHandle);
    for (const char* sec : {"TABLES", "BLOCKS", "ENTITIES", "OBJECTS"}) {
        for (const auto& rec : records(all, sec)) {
            if (const G* owner = find(rec, 330)) {
                std::uint64_t h = hex(owner->value);
                CHECK_MESSAGE((h == 0 || handles.count(h) > 0),
                              sec << ": missing owner " << owner->value);
            }
        }
    }

    // Tables: the standard rows, BLOCK_RECORDs for both layouts, APPIDs.
    auto tables = records(all, "TABLES");
    std::map<std::string, int> rows;
    for (const auto& rec : tables)
        ++rows[rec[0].value];
    CHECK(rows["TABLE"] == 9);
    CHECK(rows["LTYPE"] == 3);
    CHECK(rows["LAYER"] == 1);
    CHECK(rows["STYLE"] == 1);
    CHECK(rows["DIMSTYLE"] == 1);
    CHECK(rows["BLOCK_RECORD"] == 2);
    CHECK(rows["APPID"] == 2);
    auto blocks = records(all, "BLOCKS");
    std::vector<std::string> blockNames;
    for (const auto& rec : blocks) {
        if (rec[0].value == "BLOCK")
            blockNames.push_back(get(rec, 2));
    }
    CHECK(blockNames == std::vector<std::string>{"*Model_Space", "*Paper_Space"});

    // The root dictionary names ACAD_GROUP, and the file reads back as an empty drawing.
    bool rootDict = false;
    for (const auto& rec : records(all, "OBJECTS")) {
        if (rec[0].value == "DICTIONARY" && get(rec, 330) == "0") {
            rootDict = true;
            bool group = false;
            for (const G& g : rec)
                group = group || (g.code == 3 && g.value == "ACAD_GROUP");
            CHECK(group);
        }
    }
    CHECK(rootDict);
    Report report;
    ImportResult back = importText(text, &report);
    CHECK(back.document->modelSpaceCount() == 0);
    CHECK(report.count(Severity::Warning) == 0);
}

TEST_CASE("the TBCAD Stroke font is written as txt.shx and reads back as the stroke font") {
    Document doc;
    {
        auto tx = doc.begin("style");
        REQUIRE(tx.addTextStyle(TextStyle{{}, "Strokes", "TBCAD Stroke", "", 1.0, 0.0}).ok());
        tx.commit();
    }
    Result<ExportResult> out = exportDoc(doc);
    REQUIRE(out.ok());
    bool found = false;
    for (const G& g : groups(out.value().bytes))
        found = found || (g.code == 3 && g.value == "txt.shx");
    CHECK(found);
    Report rep;
    Result<ImportResult> back =
        importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, rep);
    REQUIRE(back.ok());
    const TextStyle* ts = back.value().document->textStyleByName("Strokes");
    REQUIRE(ts);
    CHECK(ts->fontFamily == "TBCAD Stroke");
}

TEST_CASE("Thai text is escaped in R2000 and survives; R2007 writes UTF-8") {
    Document doc;
    {
        auto tx = doc.begin("thai");
        Layer layer;
        layer.name = "ผนัง";
        LayerId id = tx.addLayer(layer).value();
        TextData t;
        t.position = {1, 2};
        t.alignPoint = t.position;
        t.height = 3;
        t.utf8 = "สวัสดี 123";
        REQUIRE(tx.add(make(t, id)).ok());
        MTextData m;
        m.position = {0, 10};
        m.height = 2.5;
        m.utf8 = "ห้องนอน\nชั้น 2 \xF0\x9F\x98\x80";  // with an emoji beyond the BMP
        REQUIRE(tx.add(make(m, id)).ok());
        tx.commit();
    }
    Result<ExportResult> r2000 = exportDoc(doc);
    REQUIRE(r2000.ok());
    const std::string& text = r2000.value().bytes;
    CHECK(std::all_of(text.begin(), text.end(), [](char c) { return (c & 0x80) == 0; }));
    CHECK(text.find("\\U+0E2A\\U+0E27\\U+0E31\\U+0E2A\\U+0E14\\U+0E35 123") != std::string::npos);
    CHECK(text.find("\\U+D83D\\U+DE00") != std::string::npos);  // surrogate pair
    ImportResult back = importText(text);
    CHECK_MESSAGE(semanticDiff(doc, *back.document).empty(), semanticDiff(doc, *back.document));
    bool found = false;
    back.document->forEachEntity([&](const Entity& e) {
        if (const auto* t = entityAs<TextData>(e)) {
            found = true;
            CHECK(t->utf8 == "สวัสดี 123");
        }
    });
    CHECK(found);

    ExportOptions utf8;
    utf8.version = DxfVersion::R2007;
    Result<ExportResult> r2007 = exportDoc(doc, utf8);
    REQUIRE(r2007.ok());
    CHECK(r2007.value().bytes.find("AC1021") != std::string::npos);
    CHECK(r2007.value().bytes.find("สวัสดี 123") != std::string::npos);
    CHECK(r2007.value().bytes.find("\\U+0E2A") == std::string::npos);
    ImportResult back2 = importText(r2007.value().bytes);
    CHECK_MESSAGE(semanticDiff(doc, *back2.document).empty(), semanticDiff(doc, *back2.document));
}

TEST_CASE(
    "sub-layers are flattened to parent~child names with TBCAD XDATA and come back as a tree") {
    Document doc;
    LayerId floor, edge, hidden, deep;
    {
        auto tx = doc.begin("layers");
        Layer top;
        top.name = "S-FLOOR";
        top.color = Color::index(3);
        top.lineweight = LineWeight::hundredths(50);
        floor = tx.addLayer(top).value();
        Layer child;
        child.name = "edge";
        child.parent = floor;
        child.color = Color::byParent();
        child.linetype = kLinetypeByParent;
        child.lineweight = LineWeight::byParent();
        edge = tx.addLayer(child).value();
        Layer h;
        h.name = "Floor hidden";
        h.parent = floor;
        h.color = Color::index(8);
        h.linetype = kLinetypeContinuous;
        h.lineweight = LineWeight::hundredths(13);
        h.on = false;
        hidden = tx.addLayer(h).value();
        Layer d;
        d.name = "deep";
        d.parent = edge;
        d.color = Color::byParent();
        d.linetype = kLinetypeByParent;
        d.lineweight = LineWeight::byParent();
        deep = tx.addLayer(d).value();
        REQUIRE(tx.add(make(LineData{{0, 0}, {5, 5}}, deep)).ok());
        REQUIRE(tx.add(make(LineData{{0, 1}, {5, 6}}, edge)).ok());
        tx.commit();
    }
    Result<ExportResult> out = exportDoc(doc);
    REQUIRE(out.ok());
    std::vector<G> all = groups(out.value().bytes);

    std::map<std::string, std::vector<G>> layers;
    for (const auto& rec : records(all, "TABLES")) {
        if (rec[0].value == "LAYER")
            layers[get(rec, 2)] = rec;
    }
    REQUIRE(layers.count("S-FLOOR"));
    REQUIRE(layers.count("S-FLOOR~edge"));
    REQUIRE(layers.count("S-FLOOR~edge~deep"));
    REQUIRE(layers.count("S-FLOOR~Floor hidden"));
    // Effective look: the ByParent children show their parent's colour (ACI 3) and weight (50).
    CHECK(get(layers["S-FLOOR~edge"], 62) == "3");
    CHECK(get(layers["S-FLOOR~edge~deep"], 62) == "3");
    CHECK(get(layers["S-FLOOR~edge~deep"], 370) == "50");
    // The hidden child is off (negative colour); XDATA carries the parent and the short name.
    CHECK(get(layers["S-FLOOR~Floor hidden"], 62) == "-8");
    const auto& x = layers["S-FLOOR~edge~deep"];
    auto xdata = [&](const std::string& prefix) {
        for (const G& g : x) {
            if (g.code == 1000 && g.value.rfind(prefix, 0) == 0)
                return g.value;
        }
        return std::string();
    };
    CHECK(get(x, 1001) == "TBCAD");
    CHECK(xdata("parent=") == "parent=S-FLOOR~edge");
    CHECK(xdata("name=") == "name=deep");
    CHECK(get(x, 1070) == "7");  // colour, linetype and lineweight are all ByParent
    CHECK(get(layers["S-FLOOR~Floor hidden"], 1070) == "0");
    CHECK(find(layers["S-FLOOR"], 1001) == nullptr);

    ImportResult back = importText(out.value().bytes);
    CHECK(semanticDiff(doc, *back.document).empty());
    const Layer* top = back.document->layerByName("S-FLOOR");
    REQUIRE(top);
    const Layer* e2 = back.document->layerByName(top->id, "edge");
    REQUIRE(e2);
    CHECK(e2->color.kind == ColorKind::ByParent);
    const Layer* d2 = back.document->layerByName(e2->id, "deep");
    REQUIRE(d2);
    CHECK(d2->lineweight.isByParent());

    // A different separator.
    ExportOptions sep;
    sep.layerSeparator = "-";
    Result<ExportResult> out2 = exportDoc(doc, sep);
    REQUIRE(out2.ok());
    CHECK(out2.value().bytes.find("S-FLOOR-edge-deep") != std::string::npos);
    ImportResult back2 = importText(out2.value().bytes);
    CHECK(semanticDiff(doc, *back2.document).empty());

    // Characters DXF reserves cannot be used.
    for (const char* bad : {"|", "$", "", "a/b"}) {
        ExportOptions o;
        o.layerSeparator = bad;
        CHECK_FALSE(exportDoc(doc, o).ok());
    }
}

TEST_CASE("names that DXF does not allow or that collide are repaired and reported") {
    Document doc;
    {
        auto tx = doc.begin("names");
        Layer a;
        a.name = "A<B>:C";
        REQUIRE(tx.addLayer(a).ok());
        Layer b;
        b.name = "A_B_C";  // collides with the repaired name above
        REQUIRE(tx.addLayer(b).ok());
        Layer c;
        c.name = "a?b?c";  // collides case-insensitively once repaired
        REQUIRE(tx.addLayer(c).ok());
        BlockDef def;
        def.name = "door*1";
        REQUIRE(tx.addBlock(def).ok());
        tx.commit();
    }
    Report report;
    Result<ExportResult> out = exportDxf(doc, {}, report);
    REQUIRE(out.ok());
    CHECK(report.countCode(wcode::kNameChanged) >= 3);
    std::set<std::string> names;
    for (const auto& rec : records(groups(out.value().bytes), "TABLES")) {
        if (rec[0].value == "LAYER")
            CHECK(names.insert(lowerAscii(get(rec, 2))).second);
    }
    CHECK(names.size() == 4);  // 0 plus the three repaired layers
    ImportResult back = importText(out.value().bytes);
    CHECK(back.document->layers().size() == 4);
    CHECK(back.document->blockByName("door_1") != nullptr);
}

TEST_CASE("R12 export has no handles, no subclass markers and no newer entities") {
    Document doc;
    {
        auto tx = doc.begin("r12");
        PolylineData pl;
        pl.v = {{{0, 0}, 0.5, 1, 1}, {{10, 0}, 0, 0, 0}, {{10, 10}, 0, 0, 0}};
        pl.closed = true;
        REQUIRE(tx.add(make(pl)).ok());
        EllipseData el;
        el.center = {50, 50};
        el.majorAxis = {10, 0};
        el.ratio = 0.5;
        REQUIRE(tx.add(make(Indirect<EllipseData>(el))).ok());
        MTextData m;
        m.position = {0, 30};
        m.height = 2;
        m.utf8 = "one\ntwo\\H3;x";
        REQUIRE(tx.add(make(Indirect<MTextData>(m))).ok());
        HatchData h;
        h.solid = true;
        HatchPolylineEdge edge;
        edge.v = {{{0, 0}, 0, 0, 0}, {{5, 0}, 0, 0, 0}, {{5, 5}, 0, 0, 0}};
        HatchLoop loop;
        loop.edges.emplace_back(edge);
        h.loops.push_back(loop);
        REQUIRE(tx.add(make(Indirect<HatchData>(h))).ok());
        REQUIRE(tx.add(make(LineData{{0, 0}, {1, 1}})).ok());
        tx.commit();
    }
    ExportOptions o;
    o.version = DxfVersion::R12;
    Report report;
    Result<ExportResult> out = exportDxf(doc, o, report);
    REQUIRE(out.ok());
    const std::string& text = out.value().bytes;
    std::vector<G> all = groups(text);
    CHECK(text.find("AC1009") != std::string::npos);
    for (const char* sec : {"TABLES", "BLOCKS", "ENTITIES"}) {
        for (const auto& rec : records(all, sec)) {
            CHECK_MESSAGE(find(rec, 5) == nullptr, sec << " " << rec[0].value << " has a handle");
            CHECK_MESSAGE(find(rec, 100) == nullptr, sec << " " << rec[0].value << " has a marker");
        }
    }
    std::map<std::string, int> kinds;
    for (const auto& rec : records(all, "ENTITIES"))
        ++kinds[rec[0].value];
    CHECK(kinds["LWPOLYLINE"] == 0);
    CHECK(kinds["ELLIPSE"] == 0);
    CHECK(kinds["MTEXT"] == 0);
    CHECK(kinds["HATCH"] == 0);
    CHECK(kinds["POLYLINE"] >= 3);  // polyline, ellipse, hatch outline
    CHECK(kinds["TEXT"] == 2);
    CHECK(kinds["SEQEND"] == kinds["POLYLINE"]);
    CHECK(report.countCode(wcode::kDowngraded) == 3);
    CHECK(records(all, "OBJECTS").empty());

    Report back;
    ImportResult r = importText(text, &back);
    CHECK(back.count(Severity::Warning) == 0);
    int polylines = 0, texts = 0;
    r.document->forEachEntity([&](const Entity& e) {
        polylines += kind(e) == EntityKind::Polyline;
        texts += kind(e) == EntityKind::Text;
    });
    CHECK(polylines >= 3);
    CHECK(texts == 2);
    // The bulged first segment survives.
    bool bulge = false;
    r.document->forEachEntity([&](const Entity& e) {
        if (const auto* pl = entityAs<PolylineData>(e)) {
            if (pl->closed && pl->v.size() == 3 && std::abs(pl->v[0].bulge - 0.5) < 1e-12)
                bulge = true;
        }
    });
    CHECK(bulge);
}

}  // namespace tbcad::dxf
