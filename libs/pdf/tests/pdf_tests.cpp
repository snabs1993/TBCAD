#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "miniz.h"
#include "tbcad/pdf/pdf.h"

using namespace tbcad::pdf;

namespace {

std::string bytes(const PdfDocument& doc) {
    std::ostringstream os;
    REQUIRE(doc.write(os).ok);
    return os.str();
}

// Returns the (inflated if needed) content stream of the Nth page (0-based).
std::string contentStream(const std::string& pdf, size_t page) {
    size_t pos = 0;
    for (size_t i = 0; i <= page; ++i) {
        pos = pdf.find("stream\n", pos);
        REQUIRE(pos != std::string::npos);
        pos += 7;
        if (i < page)
            pos = pdf.find("endstream", pos);
    }
    size_t end = pdf.find("\nendstream", pos);
    REQUIRE(end != std::string::npos);
    std::string raw = pdf.substr(pos, end - pos);
    // Dictionary precedes "stream": look for the filter in the preceding 120 bytes.
    std::string dict = pdf.substr(pos > 120 ? pos - 120 : 0, 120);
    if (dict.find("/FlateDecode") == std::string::npos)
        return raw;
    std::string out(raw.size() * 40 + 1024, '\0');
    mz_ulong outLen = static_cast<mz_ulong>(out.size());
    REQUIRE(mz_uncompress(reinterpret_cast<unsigned char*>(out.data()), &outLen,
                          reinterpret_cast<const unsigned char*>(raw.data()),
                          static_cast<mz_ulong>(raw.size())) == MZ_OK);
    out.resize(outLen);
    return out;
}

struct Pt {
    double x, y;
};

// Tiny content-stream interpreter: tracks cm / q / Q and records path points in mm.
struct Interp {
    struct M {
        double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    };
    std::vector<Pt> pts;  // every m/l/c end point in page points (after CTM)
    void run(const std::string& s) {
        std::istringstream is(s);
        std::vector<double> st;
        std::vector<M> stack;
        M ctm;
        std::string tok;
        auto apply = [&](double x, double y) {
            return Pt{ctm.a * x + ctm.c * y + ctm.e, ctm.b * x + ctm.d * y + ctm.f};
        };
        while (is >> tok) {
            char c0 = tok[0];
            if (std::isdigit(static_cast<unsigned char>(c0)) || c0 == '-' || c0 == '.') {
                st.push_back(std::stod(tok));
            } else if (tok == "q") {
                stack.push_back(ctm);
                st.clear();
            } else if (tok == "Q") {
                ctm = stack.back();
                stack.pop_back();
                st.clear();
            } else if (tok == "cm") {
                REQUIRE(st.size() == 6);
                M n{st[0], st[1], st[2], st[3], st[4], st[5]};
                M r;
                r.a = n.a * ctm.a + n.b * ctm.c;
                r.b = n.a * ctm.b + n.b * ctm.d;
                r.c = n.c * ctm.a + n.d * ctm.c;
                r.d = n.c * ctm.b + n.d * ctm.d;
                r.e = n.e * ctm.a + n.f * ctm.c + ctm.e;
                r.f = n.e * ctm.b + n.f * ctm.d + ctm.f;
                ctm = r;
                st.clear();
            } else if (tok == "m" || tok == "l") {
                pts.push_back(apply(st[st.size() - 2], st[st.size() - 1]));
                st.clear();
            } else if (tok == "c") {
                pts.push_back(apply(st[st.size() - 2], st[st.size() - 1]));
                st.clear();
            } else {
                st.clear();
            }
        }
    }
};

constexpr double kMmPerPt = 25.4 / 72.0;

bool haveQpdf() {
    return std::system("qpdf --version > /dev/null 2>&1") == 0;
}

// Runs `qpdf --check`. Skips (with a message) when qpdf is missing, unless
// TBCAD_REQUIRE_QPDF is set (CI), in which case missing qpdf is a failure.
void checkWithQpdf(const PdfDocument& doc, const char* name) {
    static const bool available = haveQpdf();
    if (!available) {
        if (std::getenv("TBCAD_REQUIRE_QPDF"))
            FAIL("qpdf not installed but TBCAD_REQUIRE_QPDF is set");
        MESSAGE("qpdf not found, skipping check for ", name);
        return;
    }
    auto path =
        std::filesystem::temp_directory_path() / (std::string("tbcad_pdf_test_") + name + ".pdf");
    REQUIRE(doc.writeFile(path.string()).ok);
    std::string cmd = "qpdf --check \"" + path.string() + "\" > /dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    std::filesystem::remove(path);
    CHECK_MESSAGE(rc == 0, "qpdf --check failed for ", name);
}

}  // namespace

TEST_CASE("formatNumber: fixed precision, trimmed, no exponent") {
    CHECK(formatNumber(100) == "100");
    CHECK(formatNumber(0.5) == "0.5");
    CHECK(formatNumber(-0.00001) == "0");
    CHECK(formatNumber(1.23456789) == "1.2346");
    CHECK(formatNumber(1e-12) == "0");
    CHECK(formatNumber(1e30).find('e') == std::string::npos);
    CHECK(formatNumber(std::nan("")) == "0");
    CHECK(formatNumber(-3.25) == "-3.25");
    CHECK(formatNumber(1.0 / 3.0, 8) == "0.33333333");
}

TEST_CASE("A4 page: 100 mm line is 100 mm after the cm transform") {
    for (bool compress : {false, true}) {
        PdfDocument doc;
        doc.setCompression(compress);
        Page& p = doc.addPage(210, 297);
        p.moveTo(10, 20);
        p.lineTo(110, 20);
        p.stroke();
        Interp in;
        in.run(contentStream(bytes(doc), 0));
        REQUIRE(in.pts.size() == 2);
        double dx = (in.pts[1].x - in.pts[0].x) * kMmPerPt;
        double dy = (in.pts[1].y - in.pts[0].y) * kMmPerPt;
        CHECK(std::hypot(dx, dy) == doctest::Approx(100.0).epsilon(1e-5));
        CHECK(in.pts[0].x * kMmPerPt == doctest::Approx(10.0).epsilon(1e-5));
    }
}

TEST_CASE("MediaBox matches page size") {
    PdfDocument doc;
    doc.setCompression(false);
    doc.addPage(210, 297);
    std::string s = bytes(doc);
    CHECK(s.find("/MediaBox [0 0 595.2756 841.8898]") != std::string::npos);
}

TEST_CASE("xref entries are exactly 20 bytes and offsets are exact") {
    PdfDocument doc;
    doc.setInfo("x");
    doc.addPage(100, 100).moveTo(1, 1);
    doc.addPage(100, 100);
    std::string s = bytes(doc);
    size_t sx = s.rfind("startxref\n");
    REQUIRE(sx != std::string::npos);
    size_t xrefPos = std::stoul(s.substr(sx + 10));
    REQUIRE(s.compare(xrefPos, 5, "xref\n") == 0);
    size_t countStart = xrefPos + 5;
    size_t nl = s.find('\n', countStart);
    size_t count = std::stoul(s.substr(countStart + 2, nl - countStart - 2));
    size_t entries = nl + 1;
    CHECK(s.compare(entries, 20, "0000000000 65535 f\r\n") == 0);
    for (size_t i = 1; i < count; ++i) {
        size_t e = entries + 20 * i;
        REQUIRE(s[e + 18] == '\r');
        REQUIRE(s[e + 19] == '\n');
        size_t off = std::stoul(s.substr(e, 10));
        std::string expect = std::to_string(i) + " 0 obj";
        CHECK(s.compare(off, expect.size(), expect) == 0);
    }
}

TEST_CASE("output is deterministic") {
    auto build = [] {
        PdfDocument d;
        d.setInfo("Plan", "TBCAD");
        Page& p = d.addPage(420, 297);
        p.setStrokeRGB(1, 0, 0);
        p.setDashMm({3, 1}, 0.5);
        p.moveTo(0, 0);
        p.lineTo(100, 100);
        p.stroke();
        return bytes(d);
    };
    CHECK(build() == build());
    // No timestamp unless requested.
    CHECK(build().find("CreationDate") == std::string::npos);
}

TEST_CASE("arcTo uses Béziers of at most 90 degrees and stays on the circle") {
    PdfDocument doc;
    doc.setCompression(false);
    Page& p = doc.addPage(100, 100);
    p.arcTo(50, 50, 20, 0, 3.14159265358979323846, true);  // half circle: 2 segments
    p.stroke();
    const std::string& c = p.content();
    size_t curves = 0;
    for (size_t pos = 0; (pos = c.find(" c\n", pos)) != std::string::npos; ++pos)
        ++curves;
    CHECK(curves == 2);

    PdfDocument d2;
    d2.setCompression(false);
    Page& q = d2.addPage(100, 100);
    q.arcTo(0, 0, 10, 0, 2 * 3.14159265358979323846, true);  // full circle: 4 segments
    Interp in;
    in.run("1 0 0 1 0 0 cm\n" + q.content());
    CHECK(in.pts.size() == 5);  // m + 4 curve end points
    for (const Pt& pt : in.pts)
        CHECK(std::hypot(pt.x, pt.y) == doctest::Approx(10.0).epsilon(1e-6));
}

TEST_CASE("arcTo clockwise sweeps the long way round correctly") {
    PdfDocument doc;
    Page& p = doc.addPage(100, 100);
    p.arcTo(0, 0, 10, 0, 1.5707963267948966, false);  // cw from 0 to 90deg = 270deg: 3 segments
    size_t curves = 0;
    const std::string& c = p.content();
    for (size_t pos = 0; (pos = c.find(" c\n", pos)) != std::string::npos; ++pos)
        ++curves;
    CHECK(curves == 3);
}

TEST_CASE("info title with Thai text is written as UTF-16BE") {
    PdfDocument doc;
    doc.setCompression(false);
    doc.setInfo("แบบ A");
    doc.addPage(100, 100);
    std::string s = bytes(doc);
    CHECK(s.find("/Title <FEFF0E41") != std::string::npos);
    checkWithQpdf(doc, "thai_title");
}

TEST_CASE("writeFile to an unwritable path returns an error") {
    PdfDocument doc;
    doc.addPage(10, 10);
    Result r = doc.writeFile("/nonexistent-dir-tbcad/x.pdf");
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("qpdf --check: empty page") {
    PdfDocument doc;
    doc.setInfo("empty");
    doc.addPage(210, 297);
    checkWithQpdf(doc, "empty");
}

TEST_CASE("qpdf --check: no pages") {
    PdfDocument doc;
    checkWithQpdf(doc, "nopages");
}

TEST_CASE("qpdf --check: 10,000 random paths") {
    PdfDocument doc;
    Page& p = doc.addPage(420, 297);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> ux(0, 420), uy(0, 297), uc(0, 1), uw(0, 2);
    for (int i = 0; i < 10000; ++i) {
        p.setStrokeRGB(uc(rng), uc(rng), uc(rng));
        p.setFillRGB(uc(rng), uc(rng), uc(rng));
        p.setLineWidthMm(uw(rng));
        p.moveTo(ux(rng), uy(rng));
        switch (i % 4) {
            case 0:
                p.lineTo(ux(rng), uy(rng));
                p.stroke();
                break;
            case 1:
                p.curveTo(ux(rng), uy(rng), ux(rng), uy(rng), ux(rng), uy(rng));
                p.stroke();
                break;
            case 2:
                p.lineTo(ux(rng), uy(rng));
                p.lineTo(ux(rng), uy(rng));
                p.closePath();
                p.fill();
                break;
            default:
                p.arcTo(ux(rng), uy(rng), uw(rng) * 10, 0, uc(rng) * 6, i % 8 == 3);
                p.fillStroke(FillRule::EvenOdd);
                break;
        }
    }
    checkWithQpdf(doc, "random10000");
}

TEST_CASE("qpdf --check: dashed lines, caps, joins, clip, transform") {
    PdfDocument doc;
    Page& p = doc.addPage(297, 210);
    p.clipRect(5, 5, 287, 200);
    p.save();
    p.transform(0.8660254, 0.5, -0.5, 0.8660254, 50, 20);
    p.setDashMm({4, 1, 0.5, 1}, 2);
    p.setLineCap(LineCap::Round);
    p.setLineJoin(LineJoin::Bevel);
    p.setLineWidthMm(kDefaultLineWidthMm);
    p.moveTo(0, 0);
    p.lineTo(100, 0);
    p.lineTo(100, 50);
    p.stroke();
    p.restore();
    p.setDashMm({});
    checkWithQpdf(doc, "dashed");
}

TEST_CASE("qpdf --check: even-odd square with a hole") {
    PdfDocument doc;
    Page& p = doc.addPage(210, 297);
    p.setFillRGB(0, 0, 1);
    p.moveTo(10, 10);
    p.lineTo(110, 10);
    p.lineTo(110, 110);
    p.lineTo(10, 110);
    p.closePath();
    p.moveTo(40, 40);  // hole wound the same way: only even-odd cuts it out
    p.lineTo(80, 40);
    p.lineTo(80, 80);
    p.lineTo(40, 80);
    p.closePath();
    p.fill(FillRule::EvenOdd);
    CHECK(p.content().find("f*\n") != std::string::npos);
    checkWithQpdf(doc, "evenodd");
}

TEST_CASE("qpdf --check: A0 page") {
    PdfDocument doc;
    Page& p = doc.addPage(841, 1189);
    p.moveTo(0, 0);
    p.lineTo(841, 1189);
    p.stroke();
    checkWithQpdf(doc, "a0");
    // Line length survives at A0 scale.
    doc.setCompression(false);
    Interp in;
    in.run(contentStream(bytes(doc), 0));
    REQUIRE(in.pts.size() == 2);
    CHECK(std::hypot((in.pts[1].x - in.pts[0].x) * kMmPerPt,
                     (in.pts[1].y - in.pts[0].y) * kMmPerPt) ==
          doctest::Approx(std::hypot(841.0, 1189.0)).epsilon(1e-5));
}
