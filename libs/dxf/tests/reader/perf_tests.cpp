// Import speed: a ~50 MB DXF must convert in under 5 seconds in a Release build. Debug and
// sanitizer builds only exercise the same code on a much smaller file.
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>

#include "reader_helpers.h"

namespace tbcad::dxf {
namespace {

// A drawing with a realistic mix of entities, about `targetBytes` long.
std::string makeBigDxf(std::size_t targetBytes) {
    std::string t;
    t.reserve(targetBytes + 4096);
    auto g = [&](int code, const char* v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%3d\n", code);
        t += buf;
        t += v;
        t += '\n';
    };
    auto gd = [&](int code, double v) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.6f", v);
        g(code, buf);
    };
    g(0, "SECTION");
    g(2, "HEADER");
    g(9, "$ACADVER");
    g(1, "AC1015");
    g(9, "$INSUNITS");
    g(70, "4");
    g(0, "ENDSEC");
    g(0, "SECTION");
    g(2, "TABLES");
    g(0, "TABLE");
    g(2, "LAYER");
    g(70, "3");
    for (const char* name : {"WALLS", "TEXT", "DIMS"}) {
        g(0, "LAYER");
        g(2, name);
        g(70, "0");
        g(62, "7");
        g(6, "CONTINUOUS");
    }
    g(0, "ENDTAB");
    g(0, "ENDSEC");
    g(0, "SECTION");
    g(2, "BLOCKS");
    g(0, "BLOCK");
    g(2, "B");
    g(70, "0");
    g(10, "0.0");
    g(20, "0.0");
    g(0, "LINE");
    g(8, "0");
    g(10, "0.0");
    g(20, "0.0");
    g(11, "1.0");
    g(21, "1.0");
    g(0, "ENDBLK");
    g(0, "ENDSEC");
    g(0, "SECTION");
    g(2, "ENTITIES");
    const char* layers[] = {"WALLS", "TEXT", "DIMS"};
    int i = 0;
    while (t.size() < targetBytes) {
        const char* layer = layers[i % 3];
        const double x = (i % 1000) * 10.0, y = (i / 1000) * 10.0;
        switch (i % 5) {
            case 0:
                g(0, "LINE");
                g(8, layer);
                gd(10, x);
                gd(20, y);
                gd(30, 0);
                gd(11, x + 8);
                gd(21, y + 3);
                gd(31, 0);
                break;
            case 1:
                g(0, "CIRCLE");
                g(8, layer);
                gd(10, x);
                gd(20, y);
                gd(30, 0);
                gd(40, 2.5);
                break;
            case 2:
                g(0, "LWPOLYLINE");
                g(8, layer);
                g(90, "5");
                g(70, "1");
                for (int k = 0; k < 5; ++k) {
                    gd(10, x + k);
                    gd(20, y + (k % 2));
                }
                break;
            case 3:
                g(0, "TEXT");
                g(8, layer);
                gd(10, x);
                gd(20, y);
                gd(30, 0);
                gd(40, 2.5);
                g(1, "Sample label");
                break;
            default:
                g(0, "INSERT");
                g(8, layer);
                g(2, "B");
                gd(10, x);
                gd(20, y);
                gd(30, 0);
                break;
        }
        ++i;
    }
    g(0, "ENDSEC");
    g(0, "EOF");
    return t;
}

}  // namespace

TEST_CASE("performance: a large drawing imports quickly") {
#ifdef NDEBUG
    const std::size_t target = 50u * 1024 * 1024;
#else
    const std::size_t target = 2u * 1024 * 1024;
#endif
    std::string text = makeBigDxf(target);
    std::vector<std::byte> bytes = testing::bytesOf(text);
    text.clear();
    text.shrink_to_fit();

    auto start = std::chrono::steady_clock::now();
    Report report;
    Result<ImportResult> r = importDxf(bytes, {}, report);
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(r.ok());
    std::cout << "[perf] imported " << (bytes.size() / (1024 * 1024)) << " MB, "
              << r.value().document->modelSpaceCount() << " entities in " << seconds << " s\n";
    CHECK(r.value().document->modelSpaceCount() > 1000);
#ifdef NDEBUG
    CHECK_MESSAGE(seconds < 5.0, "50 MB took " << seconds << " s");
#endif
}

}  // namespace tbcad::dxf
