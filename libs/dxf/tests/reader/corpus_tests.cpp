// Runs the reader over every tests/corpus/reader/*.dxf and compares the result with the
// neighbouring .expected.json (see tests/corpus/gen/gen_corpus.py for the format).
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "mini_json.h"
#include "tbcad/dxf/reader.h"

namespace tbcad::dxf {
namespace {

const char* kindName(EntityKind k) {
    switch (k) {
        case EntityKind::Line:
            return "Line";
        case EntityKind::Circle:
            return "Circle";
        case EntityKind::Arc:
            return "Arc";
        case EntityKind::Polyline:
            return "Polyline";
        case EntityKind::Point:
            return "Point";
        case EntityKind::Ellipse:
            return "Ellipse";
        case EntityKind::Spline:
            return "Spline";
        case EntityKind::Text:
            return "Text";
        case EntityKind::MText:
            return "MText";
        case EntityKind::Hatch:
            return "Hatch";
        case EntityKind::Insert:
            return "Insert";
        case EntityKind::Dimension:
            return "Dimension";
        case EntityKind::Leader:
            return "Leader";
        case EntityKind::Proxy:
            return "Proxy";
        case EntityKind::Table:
            return "Table";
    }
    return "?";
}

std::string layerPath(const Document& doc, LayerId id) {
    std::string path;
    for (LayerId p = id; p; p = doc.layer(p)->parent) {
        path = path.empty() ? doc.layer(p)->name : doc.layer(p)->name + "/" + path;
    }
    return path;
}

std::vector<std::string> sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

std::string join(const std::vector<std::string>& v) {
    std::string r;
    for (const auto& s : v)
        r += "[" + s + "]";
    return r;
}

std::vector<std::string> stringsOf(const testing::Json& j) {
    std::vector<std::string> out;
    for (const auto& e : j.a)
        out.push_back(e.s);
    return sorted(std::move(out));
}

void checkFile(const std::filesystem::path& dxfPath) {
    std::filesystem::path expectedPath = dxfPath;
    expectedPath.replace_extension(".expected.json");
    std::ifstream in(expectedPath, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "missing " << expectedPath.string());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    testing::Json exp = testing::JsonParser(text).parse();

    ImportOptions options;
    if (exp.has("options")) {
        const auto& o = exp.at("options");
        if (o.has("splitLayers"))
            options.splitLayers = o.at("splitLayers").s;
        if (o.has("insunits"))
            options.insunits = static_cast<int>(o.at("insunits").n);
    }

    std::vector<std::byte> bytes = testing::readFile(dxfPath.string());
    Report report;
    Result<ImportResult> result = importDxf(bytes, options, report);
    const int expectedExit = static_cast<int>(exp.at("exit").n);

    if (expectedExit == 2) {
        CHECK_MESSAGE(!result.ok(),
                      "expected the import of " << dxfPath.filename().string() << " to fail");
        return;
    }
    REQUIRE_MESSAGE(result.ok(), dxfPath.filename().string()
                                     << ": " << (result.ok() ? "" : result.error().message));
    const ImportResult& r = result.value();
    const Document& doc = *r.document;

    std::set<std::string> warningCodes;
    for (const Diagnostic& d : report.diagnostics()) {
        if (d.severity != Severity::Info)
            warningCodes.insert(d.code);
    }
    const int actualExit =
        (report.count(Severity::Warning) + report.count(Severity::Error)) > 0 ? 1 : 0;
    std::string codes;
    for (const auto& c : warningCodes)
        codes += c + " ";
    CHECK_MESSAGE(actualExit == expectedExit, "exit code; warnings were: " << codes);

    if (exp.has("version"))
        CHECK(r.summary.acadVer == exp.at("version").s);
    if (exp.has("encoding"))
        CHECK(std::string(encodingName(r.summary.encoding)) == exp.at("encoding").s);
    if (exp.has("encodingGuessed"))
        CHECK(r.summary.encodingGuessed == exp.at("encodingGuessed").b);
    if (exp.has("binary"))
        CHECK(r.summary.binary == exp.at("binary").b);
    if (exp.has("unitScale"))
        CHECK(r.summary.unitScale == doctest::Approx(exp.at("unitScale").n));

    if (exp.has("dxf")) {
        for (const auto& [type, n] : exp.at("dxf").o) {
            auto it = r.summary.entities.find(type);
            CHECK_MESSAGE((it != r.summary.entities.end() &&
                           it->second.read == static_cast<std::size_t>(n.n)),
                          "DXF type " << type);
        }
        for (const auto& [type, c] : r.summary.entities) {
            if (c.read > 0)
                CHECK_MESSAGE(exp.at("dxf").has(type), "unexpected DXF type " << type);
        }
    }
    if (exp.has("dxf_min")) {
        for (const auto& [type, n] : exp.at("dxf_min").o) {
            auto it = r.summary.entities.find(type);
            CHECK_MESSAGE((it != r.summary.entities.end() &&
                           it->second.read >= static_cast<std::size_t>(n.n)),
                          "DXF type " << type);
        }
    }

    std::map<std::string, std::size_t> kinds;
    std::vector<std::string> texts;
    std::size_t proxyItems = 0;
    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        ++kinds[kindName(kind(*e))];
        if (const auto* p = entityAs<ProxyData>(*e))
            proxyItems += p->graphics.items.size();
        if (const auto* t = entityAs<TextData>(*e))
            texts.push_back(t->utf8);
        if (const auto* t = entityAs<MTextData>(*e))
            texts.push_back(t->utf8);
    }
    if (exp.has("kinds")) {
        for (const auto& [k, n] : exp.at("kinds").o)
            CHECK_MESSAGE(kinds[k] == static_cast<std::size_t>(n.n), "model space kind " << k);
        for (const auto& [k, n] : kinds)
            CHECK_MESSAGE(exp.at("kinds").has(k), "unexpected model space kind " << k);
    }
    if (exp.has("kinds_min")) {
        for (const auto& [k, n] : exp.at("kinds_min").o)
            CHECK_MESSAGE(kinds[k] >= static_cast<std::size_t>(n.n), "model space kind " << k);
    }
    if (exp.has("proxy_items_min"))
        CHECK_MESSAGE(proxyItems >= static_cast<std::size_t>(exp.at("proxy_items_min").n),
                      "proxy display items " << proxyItems);
    if (exp.has("layers")) {
        std::vector<std::string> layers;
        for (const auto& [id, l] : doc.layers())
            layers.push_back(layerPath(doc, id));
        std::vector<std::string> want = stringsOf(exp.at("layers"));
        layers = sorted(layers);
        CHECK_MESSAGE(layers == want, "layers: " << join(layers) << " but expected " << join(want));
    }
    if (exp.has("texts")) {
        std::vector<std::string> want = stringsOf(exp.at("texts"));
        CHECK_MESSAGE(sorted(texts) == want, "texts: " << join(sorted(texts)));
    }
    if (exp.has("warnings")) {
        std::vector<std::string> got(warningCodes.begin(), warningCodes.end());
        CHECK_MESSAGE(got == stringsOf(exp.at("warnings")), "warnings: " << join(got));
    }
    if (exp.has("warnings_has")) {
        for (const auto& c : exp.at("warnings_has").a)
            CHECK_MESSAGE(warningCodes.count(c.s) > 0, "missing warning " << c.s);
    }
    // The report must always be valid JSON text.
    std::string json = r.summary.toJson(report);
    CHECK_NOTHROW(testing::JsonParser(json).parse());
}

}  // namespace

TEST_CASE("reader corpus: every file matches its .expected.json") {
    std::filesystem::path dir = testing::corpusPath("reader");
    REQUIRE(std::filesystem::is_directory(dir));
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".dxf")
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    CHECK(files.size() >= 20);
    for (const auto& f : files) {
        INFO("file: " << f.filename().string());
        checkFile(f);
    }
}

}  // namespace tbcad::dxf
