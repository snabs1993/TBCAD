// Random object properties through DXF export and import (colour, linetype, lineweight,
// transparency, layer, scale, visibility on several entity kinds).
#include <array>
#include <random>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "semantic_dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"

namespace tbcad::dxf {
namespace {

constexpr std::array<int, 24> kWeights = {0,  5,  9,  13, 15, 18,  20,  25,  30,  35,  40,  50,
                                          53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};

EntityData randomGeometry(std::mt19937_64& rng) {
    auto u = [&](double lo, double hi) {
        return lo + (hi - lo) * static_cast<double>(rng() % 100000) / 100000.0;
    };
    switch (rng() % 9) {
        case 0:
            return LineData{{u(-500, 500), u(-500, 500)}, {u(-500, 500), u(-500, 500)}};
        case 1:
            return CircleData{{u(-500, 500), u(-500, 500)}, u(0.5, 80)};
        case 2:
            return ArcData{{u(-500, 500), u(-500, 500)}, u(0.5, 80), u(0, 3), u(3.2, 6)};
        case 5: {
            EllipseData e;
            e.center = {u(-500, 500), u(-500, 500)};
            e.majorAxis = {u(5, 90), u(-30, 30)};
            e.ratio = u(0.2, 1.0);
            if (rng() % 2 == 0) {
                e.startParam = u(0, 3);
                e.endParam = u(3.2, 6);
            }
            return Indirect<EllipseData>(e);
        }
        case 6: {
            SplineData sp;
            sp.degree = 3;
            sp.knots = {0, 0, 0, 0, 1, 1, 1, 1};
            for (int i = 0; i < 4; ++i)
                sp.control.push_back({u(-500, 500), u(-500, 500)});
            return Indirect<SplineData>(sp);
        }
        case 7: {
            TextData t;
            t.position = {u(-500, 500), u(-500, 500)};
            t.alignPoint = t.position;
            t.height = u(1, 20);
            t.rotation = u(0, 6);
            t.utf8 = (rng() % 2) ? "Hello 123" : "\u0e2a\u0e27\u0e31\u0e2a\u0e14\u0e35";
            return Indirect<TextData>(t);
        }
        case 8: {
            RayData r;
            r.origin = {u(-500, 500), u(-500, 500)};
            r.dir = {1, u(-1, 1)};
            r.infinite = rng() % 2 == 0;
            return Indirect<RayData>(r);
        }
        case 3:
            return PointData{{u(-500, 500), u(-500, 500)}};
        default: {
            PolylineData p;
            const int n = 2 + static_cast<int>(rng() % 5);
            for (int i = 0; i < n; ++i) {
                PolyVertex v;
                v.p = {u(-500, 500), u(-500, 500)};
                v.bulge = (rng() % 3 == 0) ? u(-1, 1) : 0;
                p.v.push_back(v);
            }
            p.closed = rng() % 2 == 0;
            return p;
        }
    }
}

}  // namespace

TEST_CASE("random object properties survive DXF export and import") {
    std::mt19937_64 rng(20261018);
    for (int round = 0; round < 25; ++round) {
        Document doc;
        {
            auto tx = doc.begin("fuzz");
            Linetype dash;
            dash.name = "DASH";
            dash.pattern = {10, -5};
            const LinetypeId dashId = tx.addLinetype(dash).value();
            std::vector<LayerId> layers;
            for (const char* name : {"A-WALL", "B-TEXT", "C-DIM"}) {
                Layer l;
                l.name = name;
                l.color = Color::index(static_cast<std::uint8_t>(1 + rng() % 250));
                layers.push_back(tx.addLayer(l).value());
            }
            for (int i = 0, n = 5 + static_cast<int>(rng() % 40); i < n; ++i) {
                Entity e;
                e.data = randomGeometry(rng);
                e.common.layer = layers[rng() % layers.size()];
                switch (rng() % 4) {
                    case 0:
                        e.common.linetype = kLinetypeByLayer;
                        break;
                    case 1:
                        e.common.linetype = kLinetypeByBlock;
                        break;
                    default:
                        e.common.linetype = dashId;
                        break;
                }
                switch (rng() % 4) {
                    case 0:
                        e.common.color = Color::byLayer();
                        break;
                    case 1:
                        e.common.color = Color::byBlock();
                        break;
                    case 2:
                        e.common.color = Color::index(static_cast<std::uint8_t>(1 + rng() % 255));
                        break;
                    default:
                        e.common.color = Color::rgb(static_cast<std::uint8_t>(rng()),
                                                    static_cast<std::uint8_t>(rng()),
                                                    static_cast<std::uint8_t>(rng()));
                }
                switch (rng() % 4) {
                    case 0:
                        e.common.lineweight = LineWeight::byLayer();
                        break;
                    case 1:
                        e.common.lineweight = LineWeight::byBlock();
                        break;
                    case 2:
                        e.common.lineweight = LineWeight::defaultWeight();
                        break;
                    default:
                        e.common.lineweight = LineWeight::hundredths(
                            static_cast<std::int16_t>(kWeights[rng() % kWeights.size()]));
                }
                switch (rng() % 3) {
                    case 0:
                        e.common.transparency = kTransparencyByLayer;
                        break;
                    case 1:
                        e.common.transparency = kTransparencyByBlock;
                        break;
                    default:
                        e.common.transparency = static_cast<std::int8_t>(rng() % 91);
                }
                e.common.linetypeScale = static_cast<float>(0.25 * (1 + rng() % 16));
                e.common.visible = rng() % 6 != 0;
                REQUIRE(tx.add(std::move(e)).ok());
            }
            tx.commit();
        }
        Report exportReport;
        Result<ExportResult> out = exportDxf(doc, ExportOptions{}, exportReport);
        REQUIRE(out.ok());
        Report importReport;
        Result<ImportResult> back =
            importDxf(testing::bytesOf(out.value().bytes), ImportOptions{}, importReport);
        REQUIRE_MESSAGE(back.ok(), (back.ok() ? "" : back.error().message));
        CAPTURE(round);
        const std::string diff = testing::SemanticDump::diff(
            testing::SemanticDump(doc), testing::SemanticDump(*back.value().document));
        CHECK_MESSAGE(diff.empty(), diff);
    }
}

}  // namespace tbcad::dxf
