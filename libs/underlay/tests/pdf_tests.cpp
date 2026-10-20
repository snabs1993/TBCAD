#include <doctest/doctest.h>
#include <miniz.h>

#include <cmath>
#include <map>
#include <string>

#include "tbcad/underlay/pdf.h"

using namespace tbcad;
using namespace tbcad::underlay;

namespace {

std::span<const std::byte> bytesOf(const std::string& s) {
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

// A one-object-per-line PDF with the given page content (no cross-reference table: the reader
// scans for objects, like it does for damaged files).
std::string simplePdf(const std::string& content, const std::string& pageExtra = "",
                      const std::string& mediaBox = "[0 0 200 100]") {
    std::string p = "%PDF-1.4\n";
    p += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    p += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox " + mediaBox + " >>\nendobj\n";
    p += "3 0 obj\n<< /Type /Page /Parent 2 0 R /Contents 4 0 R /Resources << >> " + pageExtra +
         " >>\nendobj\n";
    p += "4 0 obj\n<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
         "\nendstream\nendobj\n";
    p += "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
    return p;
}

std::vector<const PolylineData*> polylines(const Document& doc) {
    std::vector<const PolylineData*> out;
    doc.forEachEntity([&](const Entity& e) {
        if (const auto* p = entityAs<PolylineData>(e))
            out.push_back(p);
    });
    return out;
}

const double kMm = 25.4 / 72.0;

}  // namespace

TEST_CASE("strokes, rectangles and curves become polylines in millimetres") {
    std::string pdf = simplePdf(
        "q 0 0 0 RG 72 0 m 144 0 l S Q\n"
        "10 10 40 20 re S\n"
        "0 0 m 0 36 36 36 36 0 c S\n"
        "0.5 0.5 0.5 rg 100 20 m 130 20 l 130 50 l f\n");
    PdfReport rep;
    auto doc = importPdf(bytesOf(pdf), {}, &rep);
    REQUIRE(doc.ok());
    CHECK(rep.pageCount == 1);
    CHECK(rep.widthMm == doctest::Approx(200 * kMm));
    CHECK(rep.heightMm == doctest::Approx(100 * kMm));
    auto polys = polylines(*doc.value());
    REQUIRE(polys.size() == 4);
    // The first line runs from x = 72 pt to 144 pt.
    bool foundLine = false;
    for (const auto* p : polys)
        if (p->v.size() == 2 && !p->closed && std::abs(p->v[0].p.x - 72 * kMm) < 1e-6) {
            foundLine = true;
            CHECK(p->v[1].p.x == doctest::Approx(144 * kMm));
        }
    CHECK(foundLine);
    int closed = 0, curves = 0;
    for (const auto* p : polys) {
        closed += p->closed ? 1 : 0;
        curves += p->v.size() > 4 ? 1 : 0;
    }
    CHECK(closed == 2);  // the rectangle and the filled triangle
    CHECK(curves == 1);
}

TEST_CASE("colours are faded toward grey, the fade is adjustable") {
    std::string pdf = simplePdf("1 0 0 RG 0 0 m 50 50 l S");
    PdfOptions none;
    none.fadePercent = 0;
    auto a = importPdf(bytesOf(pdf), none);
    REQUIRE(a.ok());
    auto colorOf = [](const Document& d) {
        Color c{};
        d.forEachEntity([&](const Entity& ent) { c = ent.common.color; });
        return c;
    };
    CHECK(colorOf(*a.value()) == Color::rgb(255, 0, 0));
    PdfOptions half;
    half.fadePercent = 50;
    auto b = importPdf(bytesOf(pdf), half);
    Color faded = colorOf(*b.value());
    CHECK(faded.r == 208);  // 255 + (160 - 255) * 0.5, rounded
    CHECK(faded.g == 80);
}

TEST_CASE("monochrome and contrast change the colours, and the stored path keeps them") {
    std::string pdf = simplePdf("1 0 0 RG 0 0 m 50 50 l S");
    auto colorOf = [](const Document& d) {
        Color c{};
        d.forEachEntity([&](const Entity& ent) { c = ent.common.color; });
        return c;
    };
    PdfOptions mono;
    mono.fadePercent = 0;
    mono.monochrome = true;
    auto a = importPdf(bytesOf(pdf), mono);
    REQUIRE(a.ok());
    const Color g = colorOf(*a.value());
    CHECK(g.r == g.g);
    CHECK(g.g == g.b);
    CHECK(g.r == 54);  // 0.2126 * 255
    PdfOptions flat;
    flat.fadePercent = 0;
    flat.contrast = 0;  // everything collapses to mid grey
    auto b = importPdf(bytesOf(pdf), flat);
    CHECK(colorOf(*b.value()) == Color::rgb(128, 128, 128));
    PdfOptions both;
    both.page = 2;
    both.contrast = 70;
    both.monochrome = true;
    const Spec sp = parseSpec(makeSpec("a.pdf", both));
    CHECK(sp.file == "a.pdf");
    CHECK(sp.pdf.page == 2);
    CHECK(sp.pdf.contrast == 70);
    CHECK(sp.pdf.monochrome);
}

TEST_CASE("white and page-sized fills are paper, not drawing") {
    std::string pdf = simplePdf(
        "1 1 1 rg 0 0 200 100 re f\n"        // white page background
        "0.5 0.8 0.2 rg 0 0 200 100 re f\n"  // colour that covers the whole page
        "0 0 0 RG 10 10 m 20 20 l S\n");
    PdfReport rep;
    auto doc = importPdf(bytesOf(pdf), {}, &rep);
    REQUIRE(doc.ok());
    CHECK(polylines(*doc.value()).size() == 1);
    CHECK(rep.skippedFills == 2);
}

TEST_CASE("page rotation turns the drawing and swaps the page size") {
    std::string pdf = simplePdf("0 0 m 100 0 l S", "/Rotate 90");
    PdfReport rep;
    auto doc = importPdf(bytesOf(pdf), {}, &rep);
    REQUIRE(doc.ok());
    CHECK(rep.widthMm == doctest::Approx(100 * kMm));
    CHECK(rep.heightMm == doctest::Approx(200 * kMm));
    auto polys = polylines(*doc.value());
    REQUIRE(polys.size() == 1);
    // A clockwise quarter turn puts the bottom edge of the page on its left side, running down.
    CHECK(polys[0]->v[0].p.x == doctest::Approx(0));
    CHECK(polys[0]->v[0].p.y == doctest::Approx(200 * kMm));
    CHECK(polys[0]->v[1].p.x == doctest::Approx(0));
    CHECK(polys[0]->v[1].p.y == doctest::Approx(100 * kMm));
}

TEST_CASE("Flate content streams and form XObjects are read") {
    std::string form = "0 0 m 10 10 l S";
    std::string page = "q 2 0 0 2 20 20 cm /Fm0 Do Q 5 5 m 6 6 l S";
    auto deflate = [](const std::string& s) {
        mz_ulong n = mz_compressBound(static_cast<mz_ulong>(s.size()));
        std::string out(n, '\0');
        REQUIRE(mz_compress(reinterpret_cast<unsigned char*>(out.data()), &n,
                            reinterpret_cast<const unsigned char*>(s.data()),
                            static_cast<mz_ulong>(s.size())) == MZ_OK);
        out.resize(n);
        return out;
    };
    std::string zp = deflate(page), zf = deflate(form);
    std::string p = "%PDF-1.5\n";
    p += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    p += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    p +=
        "3 0 obj\n<< /Type /Page /MediaBox [0 0 100 100] /Contents 4 0 R /Resources << /XObject << "
        "/Fm0 5 0 R >> >> >>\nendobj\n";
    p += "4 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(zp.size()) + " >>\nstream\n" +
         zp + "\nendstream\nendobj\n";
    p += "5 0 obj\n<< /Type /XObject /Subtype /Form /BBox [0 0 10 10] /Filter /FlateDecode "
         "/Length " +
         std::to_string(zf.size()) + " >>\nstream\n" + zf + "\nendstream\nendobj\n";
    PdfReport rep;
    auto doc = importPdf(bytesOf(p), {}, &rep);
    REQUIRE(doc.ok());
    auto polys = polylines(*doc.value());
    REQUIRE(polys.size() == 2);
    bool scaled = false;
    for (const auto* pl : polys)
        if (std::abs(pl->v[0].p.x - 20 * kMm) < 1e-6 && std::abs(pl->v[1].p.x - 40 * kMm) < 1e-6)
            scaled = true;
    CHECK(scaled);  // the form is placed with its own matrix chain: scale 2, offset 20
}

TEST_CASE("objects inside an object stream are found") {
    std::string o1 = "<< /Type /Catalog /Pages 2 0 R >>";
    std::string o2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 50 50] >>";
    std::string o3 = "<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>";
    std::string body = o1 + " " + o2 + " " + o3;
    std::string head = "1 0 2 " + std::to_string(o1.size() + 1) + " 3 " +
                       std::to_string(o1.size() + o2.size() + 2) + " ";
    std::string stm = head + body;
    std::string content = "0 0 m 10 10 l S";
    std::string p = "%PDF-1.5\n";
    p += "5 0 obj\n<< /Type /ObjStm /N 3 /First " + std::to_string(head.size()) + " /Length " +
         std::to_string(stm.size()) + " >>\nstream\n" + stm + "\nendstream\nendobj\n";
    p += "4 0 obj\n<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
         "\nendstream\nendobj\n";
    p += "6 0 obj\n<< /Type /XRef /Root 1 0 R /Size 7 >>\nstream\n\nendstream\nendobj\n";
    auto doc = importPdf(bytesOf(p));
    REQUIRE(doc.ok());
    CHECK(polylines(*doc.value()).size() == 1);
}

TEST_CASE("text and images are counted but not drawn - inline images are skipped") {
    std::string pdf = simplePdf(
        "BT /F1 12 Tf 10 10 Td (Hello) Tj ET\n"
        "q 10 0 0 10 0 0 cm BI /W 1 /H 1 /CS /G /BPC 8 ID \x01 EI Q\n"
        "0 0 m 5 5 l S\n");
    PdfReport rep;
    auto doc = importPdf(bytesOf(pdf), {}, &rep);
    REQUIRE(doc.ok());
    CHECK(polylines(*doc.value()).size() == 1);
    CHECK(rep.textRuns == 1);
    CHECK(rep.images == 1);
    CHECK(rep.notes.size() == 2);
}

TEST_CASE("bad input is rejected with a message") {
    CHECK_FALSE(importPdf(bytesOf("not a pdf at all")).ok());
    std::string enc = simplePdf("0 0 m 1 1 l S");
    enc.replace(enc.find("trailer\n<< "), 11, "trailer\n<< /Encrypt 9 0 R ");
    auto e = importPdf(bytesOf(enc));
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message.find("encrypted") != std::string::npos);
    PdfOptions p3;
    p3.page = 3;
    auto r = importPdf(bytesOf(simplePdf("0 0 m 1 1 l S")), p3);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("1 page") != std::string::npos);
}

TEST_CASE("a page tree that lists itself many times is read once (found by fuzzing)") {
    std::string pdf = simplePdf("0 0 m 10 10 l S");
    const std::string kids = "/Kids [3 0 R]";
    std::string many = "/Kids [";
    for (int i = 0; i < 8; ++i)
        many += "2 0 R ";
    many += "3 0 R]";
    pdf.replace(pdf.find(kids), kids.size(), many);
    PdfReport rep;
    auto doc = importPdf(bytesOf(pdf), {}, &rep);
    REQUIRE(doc.ok());
    CHECK(rep.pageCount == 1);
    CHECK(polylines(*doc.value()).size() == 1);
}

TEST_CASE("the vertex limit stops the import and says so") {
    std::string content;
    for (int i = 0; i < 100; ++i)
        content += "0 0 m 1 1 l 2 0 l S\n";
    PdfOptions o;
    o.maxVertices = 100;
    PdfReport rep;
    auto doc = importPdf(bytesOf(simplePdf(content)), o, &rep);
    REQUIRE(doc.ok());
    CHECK(rep.truncated);
    CHECK(rep.vertices <= 100);
}

TEST_CASE("stored path specs: page and fade travel with the path") {
    Spec s = parseSpec("plans/ground.pdf#page=3,fade=70");
    CHECK(s.hasSpec);
    CHECK(s.file == "plans/ground.pdf");
    CHECK(s.pdf.page == 3);
    CHECK(s.pdf.fadePercent == doctest::Approx(70));
    Spec plain = parseSpec("ground.pdf");
    CHECK_FALSE(plain.hasSpec);
    CHECK(plain.file == "ground.pdf");
    // A '#' that is not a spec stays part of the file name.
    Spec odd = parseSpec("C:/job #5/plan.pdf");
    CHECK_FALSE(odd.hasSpec);
    CHECK(odd.file == "C:/job #5/plan.pdf");
    PdfOptions o;
    o.page = 2;
    CHECK(makeSpec("a.pdf", o) == "a.pdf#page=2");
    CHECK(makeSpec("a.pdf", PdfOptions{}) == "a.pdf");
    CHECK(isPdfPath("A.PDF#page=2"));
    CHECK_FALSE(isPdfPath("a.png"));
}

TEST_CASE("PDF layers (optional content) become layers, hidden ones start off") {
    std::string content =
        "/OC /A BDC 0 0 m 10 0 l S EMC\n"
        "/OC /B BDC 0 10 m 10 10 l S EMC\n"
        "20 0 m 30 0 l S\n";
    std::string p = "%PDF-1.5\n";
    p += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /OCProperties << /OCGs [5 0 R 6 0 R] "
         "/D << /OFF [6 0 R] >> >> >>\nendobj\n";
    p += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 200 100] >>\nendobj\n";
    p += "3 0 obj\n<< /Type /Page /Parent 2 0 R /Contents 4 0 R /Resources << /Properties "
         "<< /A 5 0 R /B 6 0 R >> >> >>\nendobj\n";
    p += "4 0 obj\n<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
         "\nendstream\nendobj\n";
    p += "5 0 obj\n<< /Type /OCG /Name (Walls) >>\nendobj\n";
    p += "6 0 obj\n<< /Type /OCG /Name (Furniture) >>\nendobj\n";
    p += "trailer\n<< /Root 1 0 R >>\n%%EOF\n";
    PdfReport rep;
    auto doc = importPdf(bytesOf(p), {}, &rep);
    REQUIRE(doc.ok());
    REQUIRE(rep.layers.size() == 2);
    CHECK(rep.layers[0] == "Walls");
    CHECK(rep.layers[1] == "Furniture");
    const Document& d = *doc.value();
    const Layer* walls = d.layerByName("Walls");
    const Layer* furniture = d.layerByName("Furniture");
    REQUIRE(walls);
    REQUIRE(furniture);
    CHECK(walls->on);
    CHECK_FALSE(furniture->on);
    std::map<std::string, int> perLayer;
    d.forEachEntity([&](const Entity& e) { ++perLayer[d.layer(e.common.layer)->name]; });
    CHECK(perLayer["Walls"] == 1);
    CHECK(perLayer["Furniture"] == 1);
    CHECK(perLayer[kPdfLayerName] == 1);
}
