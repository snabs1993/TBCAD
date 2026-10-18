// Random drawings through boundary search, fills at extreme scales and angles, explode and
// gradient bands: no crash, no non-finite numbers, and every search either answers or says why.
#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "helpers.h"
#include "tbcad/hatch/hatch.h"

using namespace tbcad;
using namespace tbcad::hatch;
using namespace tbcad::hatch::testing;

namespace {

Entity randomEntity(std::mt19937_64& rng) {
    auto u = [&](double lo, double hi) {
        return lo + (hi - lo) * static_cast<double>(rng() % 100000) / 100000.0;
    };
    switch (rng() % 5) {
        case 0:
            return line({u(0, 100), u(0, 100)}, {u(0, 100), u(0, 100)});
        case 1:
            return circle({u(0, 100), u(0, 100)}, u(1, 40));
        case 2:
            return arc({u(0, 100), u(0, 100)}, u(1, 40), u(0, 3), u(3.1, 6.2));
        case 3: {
            PolylineData pl;
            pl.closed = rng() % 2 == 0;
            for (int i = 0, n = 2 + static_cast<int>(rng() % 6); i < n; ++i)
                pl.v.push_back({{u(0, 100), u(0, 100)}, rng() % 3 == 0 ? u(-1, 1) : 0, 0, 0});
            return ent(std::move(pl));
        }
        default: {
            const double x = u(0, 70), y = u(0, 70);
            return rectPolyline({x, y}, {x + u(1, 30), y + u(1, 30)});
        }
    }
}

bool finite(Vec2 p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

}  // namespace

TEST_CASE("random drawings: boundary, fill, explode and gradient never produce bad numbers") {
    std::mt19937_64 rng(20261022);
    const auto& patterns = builtinPatterns();
    int found = 0;
    for (int round = 0; round < 60; ++round) {
        Scene scene;
        for (int i = 0, n = 3 + static_cast<int>(rng() % 12); i < n; ++i)
            scene.add(randomEntity(rng));
        BoundaryOptions opt;
        opt.gapTolerance = (rng() % 3 == 0) ? 1.5 : 0;
        BoundaryFinder finder(scene.candidates(), opt);
        if (!finder.ok())
            continue;
        for (int pick = 0; pick < 6; ++pick) {
            const Vec2 at{static_cast<double>(rng() % 10000) / 100,
                          static_cast<double>(rng() % 10000) / 100};
            Result<Boundary> b = finder.find(at);
            if (!b.ok())
                continue;
            ++found;
            const Pattern& pat = patterns[rng() % patterns.size()];
            static const double kScales[] = {1e-4, 0.01, 0.5, 1, 7, 300, 1e5};
            Entity h = makeHatchEntity(b.value(), pat, kScales[rng() % 7],
                                       static_cast<double>(rng() % 2000) / 100 - 10,
                                       {static_cast<double>(rng() % 200) - 100, 0});
            const HatchData& hd = *std::get<Indirect<HatchData>>(h.data);
            CAPTURE(round);
            CAPTURE(pat.name);
            CAPTURE(hd.patternScale);
            FillOptions fo;
            fo.pixelSizeMm = (rng() % 2) ? 0.05 : 0;
            FillResult fr = generateFill(hd, pat, fo);
            for (const auto& ring : fr.rings)
                for (Vec2 p : ring)
                    CHECK(finite(p));
            for (const auto& s : fr.segments) {
                CHECK(finite(s.first));
                CHECK(finite(s.second));
            }
            for (Vec2 p : fr.dots)
                CHECK(finite(p));
            Document doc;
            Result<std::vector<Entity>> ex = explodeHatch(doc, h);
            CHECK((ex.ok() || !ex.error().message.empty()));
            for (const GradientBand& g : gradientBands(hd, 0.05))
                for (const auto& ring : g.rings)
                    for (Vec2 p : ring)
                        CHECK(finite(p));
        }
    }
    CHECK(found > 30);  // the generator does close regions often enough to be meaningful
}
