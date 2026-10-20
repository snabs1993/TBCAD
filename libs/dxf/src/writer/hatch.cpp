// DXF export: HATCH, including embedded pattern definitions.
#include <algorithm>
#include <cmath>
#include <map>

#include "tbcad/hatch/hatch.h"
#include "tbcad/hatch/pattern.h"
#include "writer_impl.h"

namespace tbcad::dxf::detail {

namespace {

constexpr double kDeg = 180.0 / kPi;

std::string lowerAscii(std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

// Corner points of a bulged segment as an arc: centre, radius, start and end angle (radians).
struct BulgeArc {
    Vec2 center{};
    double radius = 0;
    double start = 0, end = 0;
    bool ccw = true;
};

BulgeArc bulgeArc(Vec2 a, Vec2 b, double bulge) {
    double sweep = 4 * std::atan(bulge);
    Vec2 chord = b - a;
    double c = length(chord);
    double radius = c / (2 * std::sin(std::abs(sweep) / 2));
    Vec2 mid = (a + b) * 0.5;
    double h = radius * std::cos(sweep / 2);
    Vec2 center = mid + perp(normalized(chord)) * (bulge > 0 ? h : -h);
    BulgeArc r;
    r.center = center;
    r.radius = radius;
    r.start = normalizeAngle(std::atan2(a.y - center.y, a.x - center.x));
    r.end = normalizeAngle(std::atan2(b.y - center.y, b.x - center.x));
    r.ccw = bulge > 0;
    return r;
}

bool hasBulge(const std::vector<PolyVertex>& v) {
    return std::any_of(v.begin(), v.end(), [](const PolyVertex& p) { return p.bulge != 0; });
}

}  // namespace

// The pattern library of W5-4 (built-in patterns plus the user's own), in the .pat convention.
std::optional<std::vector<HatchPatternLine>> libraryPattern(std::string_view name) {
    const hatch::Pattern* pat = hatch::PatternRegistry::find(name);
    if (!pat)
        return std::nullopt;
    std::vector<HatchPatternLine> lines;
    for (const hatch::PatternFamily& f : pat->families) {
        HatchPatternLine l;
        l.angleDeg = f.angle * kDeg;
        l.originX = f.origin.x;
        l.originY = f.origin.y;
        l.dx = f.delta.x;
        l.dy = f.delta.y;
        l.dashes = f.dashes;
        lines.push_back(std::move(l));
    }
    return lines;
}

std::optional<std::vector<HatchPatternLine>> Writer::patternLines(const std::string& name) {
    std::string key = lowerAscii(name);
    auto cached = patternCache_.find(key);
    if (cached != patternCache_.end())
        return cached->second;
    std::optional<std::vector<HatchPatternLine>> found;
    if (opt_.patterns)
        found = opt_.patterns(name);
    if (!found)
        found = libraryPattern(name);
    if (found)
        patternCache_[key] = *found;
    return found;
}

void Writer::writeSolidHatch(const Props& p, const std::vector<std::vector<Vec2>>& rings,
                             std::uint64_t owner) {
    HatchData h;
    h.solid = true;
    for (std::size_t i = 0; i < rings.size(); ++i) {
        HatchPolylineEdge edge;
        edge.closed = true;
        for (Vec2 pt : rings[i]) {
            PolyVertex v;
            v.p = pt;
            edge.v.push_back(v);
        }
        HatchLoop loop;
        loop.outer = i == 0;
        loop.edges.emplace_back(std::move(edge));
        h.loops.push_back(std::move(loop));
    }
    writeHatch(p, h, owner);
}

void Writer::writeHatchBoundaryAsLines(const Props& p, const HatchData& d, std::uint64_t owner) {
    for (const HatchLoop& loop : d.loops) {
        for (const HatchEdge& edge : loop.edges) {
            if (const auto* pe = std::get_if<HatchPolylineEdge>(&edge)) {
                writeVertexPolyline(p, pe->v, pe->closed, 0, owner);
            } else if (const auto* le = std::get_if<HatchLineEdge>(&edge)) {
                writeLine(p, LineData{le->a, le->b}, owner);
            } else if (const auto* ae = std::get_if<HatchArcEdge>(&edge)) {
                ArcData arc;
                arc.center = ae->center;
                arc.radius = ae->radius;
                arc.startAngle = normalizeAngle(ae->ccw ? ae->startAngle : ae->endAngle);
                arc.endAngle = normalizeAngle(ae->ccw ? ae->endAngle : ae->startAngle);
                writeArc(p, arc, owner);
            } else if (const auto* ee = std::get_if<HatchEllipseEdge>(&edge)) {
                Entity tmp;
                EllipseData el;
                el.center = ee->center;
                el.majorAxis = ee->majorAxis;
                el.ratio = ee->ratio;
                el.startParam = ee->startParam;
                el.endParam = ee->endParam;
                tmp.common.layer = LayerId{};
                tmp.data = Indirect<EllipseData>(el);
                writeCurveFallback(tmp, p, owner);
            } else if (const auto* se = std::get_if<HatchSplineEdge>(&edge)) {
                Entity tmp;
                tmp.data = Indirect<SplineData>(se->spline);
                writeCurveFallback(tmp, p, owner);
            }
        }
    }
}

void Writer::writeHatch(const Props& p, const HatchData& d, std::uint64_t owner) {
    if (d.loops.empty()) {
        skipEntity("HATCH", "no boundary");
        return;
    }
    if (r12_) {
        downgrade("HATCH", "has no R12 form; only its boundary is written");
        writeHatchBoundaryAsLines(p, d, owner);
        return;
    }
    bool solid = d.solid || d.pattern.empty();
    head("HATCH", p, owner, "AcDbHatch");
    Out& o = *L().o;
    o.pt(10, {0, 0});
    o.d(210, 0);
    o.d(220, 0);
    o.d(230, 1);
    o.str(2, solid ? "SOLID" : d.pattern);
    o.i(70, solid ? 1 : 0);
    o.i(71, 0);
    o.i(91, static_cast<long long>(d.loops.size()));

    for (const HatchLoop& loop : d.loops) {
        int outer = loop.outer ? 1 : 0;
        if (loop.edges.size() == 1 && std::holds_alternative<HatchPolylineEdge>(loop.edges[0])) {
            const auto& pe = std::get<HatchPolylineEdge>(loop.edges[0]);
            o.i(92, outer | 2);
            o.i(72, hasBulge(pe.v) ? 1 : 0);
            o.i(73, pe.closed ? 1 : 0);
            o.i(93, static_cast<long long>(pe.v.size()));
            bool bulged = hasBulge(pe.v);
            for (const PolyVertex& v : pe.v) {
                o.pt2(10, v.p);
                if (bulged)
                    o.d(42, v.bulge);
            }
            o.i(97, 0);
            continue;
        }
        // Edge path: expand polyline edges into lines and arcs first.
        struct Edge {
            int type;  // 1 line, 2 arc, 3 ellipse, 4 spline
            Vec2 a{}, b{};
            BulgeArc arc{};
            const HatchEllipseEdge* ellipse = nullptr;
            const HatchSplineEdge* spline = nullptr;
            const HatchArcEdge* arcEdge = nullptr;
        };
        std::vector<Edge> edges;
        for (const HatchEdge& edge : loop.edges) {
            if (const auto* le = std::get_if<HatchLineEdge>(&edge)) {
                edges.push_back({1, le->a, le->b});
            } else if (const auto* ae = std::get_if<HatchArcEdge>(&edge)) {
                Edge e{2};
                e.arcEdge = ae;
                edges.push_back(e);
            } else if (const auto* ee = std::get_if<HatchEllipseEdge>(&edge)) {
                Edge e{3};
                e.ellipse = ee;
                edges.push_back(e);
            } else if (const auto* se = std::get_if<HatchSplineEdge>(&edge)) {
                Edge e{4};
                e.spline = se;
                edges.push_back(e);
            } else if (const auto* pe = std::get_if<HatchPolylineEdge>(&edge)) {
                std::size_t n = pe->v.size();
                std::size_t segs = pe->closed ? n : n - 1;
                for (std::size_t i = 0; i < segs; ++i) {
                    Vec2 a = pe->v[i].p, b = pe->v[(i + 1) % n].p;
                    double bulge = pe->v[i].bulge;
                    if (bulge == 0 || tol::isZeroLength(b - a)) {
                        edges.push_back({1, a, b});
                    } else {
                        Edge e{2};
                        e.arc = bulgeArc(a, b, bulge);
                        e.a = a;
                        e.b = b;
                        edges.push_back(e);
                    }
                }
            }
        }
        o.i(92, outer);
        o.i(93, static_cast<long long>(edges.size()));
        for (const Edge& e : edges) {
            o.i(72, e.type);
            if (e.type == 1) {
                o.pt2(10, e.a);
                o.pt2(11, e.b);
            } else if (e.type == 2) {
                if (e.arcEdge) {
                    o.pt2(10, e.arcEdge->center);
                    o.d(40, e.arcEdge->radius);
                    o.d(50, e.arcEdge->startAngle * kDeg);
                    o.d(51, e.arcEdge->endAngle * kDeg);
                    o.i(73, e.arcEdge->ccw ? 1 : 0);
                } else {
                    o.pt2(10, e.arc.center);
                    o.d(40, e.arc.radius);
                    o.d(50, e.arc.start * kDeg);
                    o.d(51, e.arc.end * kDeg);
                    o.i(73, e.arc.ccw ? 1 : 0);
                }
            } else if (e.type == 3) {
                o.pt2(10, e.ellipse->center);
                o.pt2(11, e.ellipse->majorAxis);
                o.d(40, e.ellipse->ratio);
                o.d(50, e.ellipse->startParam * kDeg);
                o.d(51, e.ellipse->endParam * kDeg);
                o.i(73, e.ellipse->ccw ? 1 : 0);
            } else {
                const SplineData& sp = e.spline->spline;
                std::vector<double> knots = sp.knots;
                std::size_t n = sp.control.size();
                std::size_t deg = static_cast<std::size_t>(std::max(sp.degree, 1));
                if (knots.size() != n + deg + 1) {
                    knots.clear();
                    std::size_t interior = n > deg ? n - deg - 1 : 0;
                    for (std::size_t i = 0; i <= deg; ++i)
                        knots.push_back(0);
                    for (std::size_t i = 1; i <= interior; ++i)
                        knots.push_back(static_cast<double>(i));
                    for (std::size_t i = 0; i <= deg; ++i)
                        knots.push_back(static_cast<double>(interior + 1));
                }
                bool rational = sp.weights.size() == n;
                o.i(94, sp.degree);
                o.i(73, rational ? 1 : 0);
                o.i(74, sp.periodic ? 1 : 0);
                o.i(95, static_cast<long long>(knots.size()));
                o.i(96, static_cast<long long>(n));
                for (double k : knots)
                    o.d(40, k);
                for (std::size_t i = 0; i < n; ++i) {
                    o.pt2(10, sp.control[i]);
                    if (rational)
                        o.d(42, sp.weights[i]);
                }
                o.i(97, static_cast<long long>(sp.fit.size()));
                for (Vec2 f : sp.fit)
                    o.pt2(11, f);
            }
        }
        o.i(97, 0);
    }

    o.i(75, 0);
    o.i(76, 1);
    if (!solid) {
        o.d(52, d.patternAngle * kDeg);
        o.d(41, d.patternScale);
        o.i(77, d.doubled ? 1 : 0);
        std::optional<std::vector<HatchPatternLine>> lines = patternLines(d.pattern);
        if (!lines && warnedPatterns_.insert(lowerAscii(d.pattern)).second) {
            rep_.info(wcode::kUnknownPattern,
                      "hatch pattern \"" + d.pattern +
                          "\" has no definition here; only its name is written (AutoCAD draws "
                          "its own patterns by name, programs that do not know it draw nothing)");
        }
        std::size_t count = lines ? lines->size() : 0;
        o.i(78, static_cast<long long>(count));
        if (lines) {
            double scale = d.patternScale;
            double ca = std::cos(d.patternAngle), sa = std::sin(d.patternAngle);
            for (const HatchPatternLine& l : *lines) {
                double angle = l.angleDeg + d.patternAngle * kDeg;
                double rad = angle / kDeg;
                double cr = std::cos(rad), sr = std::sin(rad);
                Vec2 origin{d.patternOrigin.x + scale * (l.originX * ca - l.originY * sa),
                            d.patternOrigin.y + scale * (l.originX * sa + l.originY * ca)};
                o.d(53, angle);
                o.d(43, origin.x);
                o.d(44, origin.y);
                o.d(45, scale * (l.dx * cr - l.dy * sr));
                o.d(46, scale * (l.dx * sr + l.dy * cr));
                o.i(79, static_cast<long long>(l.dashes.size()));
                for (double dash : l.dashes)
                    o.d(49, dash * scale);
            }
        }
    }
    o.d(47, 1);
    o.i(98, 0);
    if (d.gradient) {
        // Kind, angle, shift, one-colour tint and the two colours, each as its index (when it
        // has one) and as RGB.
        const GradientData& gd = *d.gradient;
        o.i(450, 1);
        o.i(451, 0);
        o.i(452, gd.oneColor ? 1 : 0);
        o.i(453, 2);
        double at = 0;
        for (const Color& c : {gd.color1, gd.color2}) {
            Rgba v{c.r, c.g, c.b};
            o.d(463, at);
            if (c.kind == ColorKind::Index) {
                v = paletteColor(c.r);
                o.i(63, c.r);
            }
            o.i(421, (static_cast<long long>(v.r) << 16) | (v.g << 8) | v.b);
            at = 1;
        }
        o.d(460, gd.angle);
        o.d(461, gd.shift);
        o.d(462, gd.tint);
        o.str(470, hatch::gradientName(gd.kind));
    }
    count("HATCH");
}

}  // namespace tbcad::dxf::detail
