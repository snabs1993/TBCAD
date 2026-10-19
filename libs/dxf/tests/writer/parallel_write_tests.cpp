// A long entity list is written on several threads; the bytes (handles included) must be the
// same as from one thread, whatever mix of plain and complex objects the list holds.
#include <cstdio>
#include <string>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "tbcad/core/core.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"

namespace tbcad::dxf {
namespace {

std::string hex(unsigned v) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", v);
    return b;
}

// Plain objects (some with the handle they came with) mixed with attribute inserts, hatches
// and a polyline that cannot be written.
std::string mixedDxf(int n) {
    std::string s =
        "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1015\n  0\nENDSEC\n"
        "  0\nSECTION\n  2\nBLOCKS\n"
        "  0\nBLOCK\n  2\nPLAIN\n 70\n     0\n 10\n0.0\n 20\n0.0\n  8\n0\n"
        "  0\nLINE\n  8\n0\n 10\n0.0\n 20\n0.0\n 11\n1.0\n 21\n1.0\n  0\nENDBLK\n"
        "  0\nBLOCK\n  2\nTAGGED\n 70\n     2\n 10\n0.0\n 20\n0.0\n  8\n0\n"
        "  0\nATTDEF\n  8\n0\n 10\n0.0\n 20\n0.0\n 40\n1.0\n  1\nx\n  3\nask\n  2\nTAG\n 70\n     "
        "0\n"
        "  0\nENDBLK\n"
        "  0\nENDSEC\n  0\nSECTION\n  2\nENTITIES\n";
    for (int i = 0; i < n; ++i) {
        const std::string h = i % 3 == 0 ? "  5\n" + hex(0x10000 + i) + "\n" : "";
        const std::string at =
            " 10\n" + std::to_string(i % 977) + ".5\n 20\n" + std::to_string(i / 977) + ".25\n";
        switch (i % 11) {
            case 0:
                s += "  0\nLINE\n" + h + "  8\nA" + std::to_string(i % 5) + "\n" + at + " 11\n" +
                     std::to_string(i % 977 + 3) + ".5\n 21\n" + std::to_string(i / 977 + 2) +
                     ".25\n";
                break;
            case 1:
                s += "  0\nCIRCLE\n" + h + "  8\n0\n" + at + " 40\n2.5\n";
                break;
            case 2:
                s += "  0\nARC\n" + h + "  8\n0\n" + at + " 40\n2.5\n 50\n10.0\n 51\n80.0\n";
                break;
            case 3:
                s += "  0\nLWPOLYLINE\n" + h + "  8\n0\n 90\n     3\n 70\n     0\n" + at +
                     " 10\n5.5\n 20\n6.5\n 10\n9.5\n 20\n1.5\n 42\n0.5\n";
                break;
            case 4:
                s += "  0\nTEXT\n" + h + "  8\n0\n" + at + " 40\n2.5\n  1\nlabel " +
                     std::to_string(i) + "\n";
                break;
            case 5:
                s += "  0\nMTEXT\n" + h + "  8\n0\n" + at + " 40\n2.5\n 41\n30.0\n  1\nnote " +
                     std::to_string(i) + "\n";
                break;
            case 6:
                s += "  0\nINSERT\n" + h + "  8\n0\n  2\nPLAIN\n" + at;
                break;
            case 7:  // an insert with an attribute: more records, more handles
                s += "  0\nINSERT\n" + h + "  8\n0\n  2\nTAGGED\n 66\n     1\n" + at +
                     "  0\nATTRIB\n  8\n0\n" + at + " 40\n1.0\n  1\nv" + std::to_string(i) +
                     "\n  2\nTAG\n 70\n     0\n  0\nSEQEND\n";
                break;
            case 8:
                s += "  0\nHATCH\n" + h +
                     "  8\n0\n 10\n0.0\n 20\n0.0\n 30\n0.0\n210\n0.0\n220\n0.0\n"
                     "230\n1.0\n  2\nSOLID\n 70\n     1\n 71\n     0\n 91\n        1\n 92\n     1\n"
                     " 93\n        3\n 10\n" +
                     std::to_string(i % 977) + ".0\n 20\n0.0\n 10\n" + std::to_string(i % 977 + 4) +
                     ".0\n 20\n0.0\n 10\n" + std::to_string(i % 977) +
                     ".0\n 20\n4.0\n 97\n        0\n 75\n     0\n"
                     " 76\n     1\n 98\n        0\n";
                break;
            case 9:
                s += "  0\nPOINT\n" + h + "  8\n0\n" + at;
                break;
            default:  // a polyline of one vertex: not written, with a warning
                s += "  0\nLWPOLYLINE\n" + h + "  8\n0\n 90\n     1\n 70\n     0\n" + at;
                break;
        }
    }
    s += "  0\nENDSEC\n  0\nEOF\n";
    return s;
}

}  // namespace

TEST_CASE("a long mixed entity list gives the same DXF on one thread and on many") {
    Report r;
    Result<ImportResult> in = importDxf(testing::bytesOf(mixedDxf(66000)), ImportOptions{}, r);
    REQUIRE(in.ok());
    const Document& doc = *in.value().document;
    std::string reference;
    for (std::size_t threads : {std::size_t{1}, std::size_t{4}, std::size_t{0}, std::size_t{3}}) {
        ExportOptions opt;
        opt.maxThreads = threads;
        Report report;
        Result<ExportResult> out = exportDxf(doc, opt, report);
        REQUIRE(out.ok());
        if (threads == 1) {
            reference = out.value().bytes;
            CHECK(reference.size() > 1000000);
            continue;
        }
        CHECK(out.value().bytes == reference);
    }
}

}  // namespace tbcad::dxf
