// DXF import: HATCH.
//
// Group codes inside a HATCH depend on their position (10/20 are polyline vertices, edge points
// or seed points depending on where they appear), so the groups are walked with a cursor.
#include <algorithm>
#include <cmath>

#include "importer.h"

namespace tbcad::dxf::detail {

namespace {

constexpr double kDeg = kPi / 180.0;

bool foldAsciiEquals(const std::string& s, const char* lower) {
    std::size_t i = 0;
    for (; i < s.size() && lower[i]; ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != lower[i])
            return false;
    }
    return i == s.size() && lower[i] == 0;
}

// Fields of one non-polyline boundary edge.
struct EdgeFields {
    std::vector<double> x10, y10, x11, y11;
    std::vector<double> c40, c42;
    double c50 = 0, c51 = 0;
    bool ccw = true;
    int degree = 3;
    bool rational = false, periodic = false;
    std::int64_t ctrlCount = 0;
};

// Codes that end the current edge or the whole boundary path section.
bool endsEdge(int code) {
    switch (code) {
        case 72:
        case 92:
        case 97:
        case 75:
        case 76:
        case 91:
        case 98:
        case 330:
            return true;
        default:
            return false;
    }
}

}  // namespace

// Reads the gradient groups (450 ... 470) that follow the boundary paths of a HATCH: the angle,
// the two colours (group 463 starts each colour; 421 is its RGB value, 63 its palette index) and
// the name (LINEAR, CYLINDER; others are drawn as linear and reported).
void Importer::readGradient(const Record& r, std::size_t from, const Frame& f, HatchData& h) {
    const std::vector<Group>& g = r.g;
    GradientData gd;
    std::vector<Color> colors;
    bool inColor = false;
    for (std::size_t j = from; j < g.size(); ++j) {
        switch (g[j].code) {
            case 460:
                gd.angle = f.angle(g[j].number());
                if (!std::isfinite(gd.angle))
                    gd.angle = 0;
                break;
            case 463:
                if (colors.size() < 2)
                    colors.push_back(Color::byLayer());
                inColor = colors.size() <= 2;
                break;
            case 63:
                if (inColor && colors.back().kind == ColorKind::ByLayer)
                    colors.back() =
                        mapAci(static_cast<int>(std::clamp<std::int64_t>(g[j].integer(), 1, 255)));
                break;
            case 421:
                if (inColor) {
                    std::int64_t v = g[j].integer();
                    colors.back() = Color::rgb(static_cast<std::uint8_t>((v >> 16) & 0xFF),
                                               static_cast<std::uint8_t>((v >> 8) & 0xFF),
                                               static_cast<std::uint8_t>(v & 0xFF));
                }
                break;
            case 470: {
                std::string name = dec(g[j].str());
                if (foldAsciiEquals(name, "cylinder")) {
                    gd.kind = GradientKind::Cylinder;
                } else if (!foldAsciiEquals(name, "linear")) {
                    report_.info(code::kGradient,
                                 "gradient \"" + name + "\" is drawn as a linear gradient", r.line);
                }
                break;
            }
            default:
                break;
        }
    }
    if (colors.size() >= 1 && colors[0].kind != ColorKind::ByLayer)
        gd.color1 = colors[0];
    if (colors.size() >= 2 && colors[1].kind != ColorKind::ByLayer)
        gd.color2 = colors[1];
    h.gradient = gd;
}

bool Importer::convHatch(const Record& r, const Frame& f, std::vector<Entity>& out) {
    const std::vector<Group>& g = r.g;
    const std::size_t n = g.size();
    std::size_t i = 0;

    HatchData h;
    std::string pattern = dec(r.raw(2));
    bool solid = r.integer(70) == 1 || foldAsciiEquals(pattern, "solid");
    if (r.integer(450) == 1) {
        solid = true;
    }

    while (i < n && g[i].code != 91)
        ++i;
    if (i >= n) {
        bad(r, "HATCH", "no boundary paths; skipped");
        return false;
    }
    std::int64_t pathCount = g[i++].integer();

    for (std::int64_t p = 0; p < pathCount && i < n; ++p) {
        while (i < n && g[i].code != 92)
            ++i;
        if (i >= n)
            break;
        std::int64_t pflags = g[i++].integer();
        HatchLoop loop;
        loop.outer = (pflags & 1) != 0;

        if (pflags & 2) {
            bool hasBulge = false, closed = true;
            std::int64_t count = 0;
            while (i < n && (g[i].code == 72 || g[i].code == 73 || g[i].code == 93)) {
                if (g[i].code == 72)
                    hasBulge = g[i].integer() != 0;
                else if (g[i].code == 73)
                    closed = g[i].integer() != 0;
                else
                    count = g[i].integer();
                ++i;
            }
            HatchPolylineEdge pe;
            pe.closed = closed;
            count = std::min<std::int64_t>(count, static_cast<std::int64_t>(n));
            for (std::int64_t k = 0; k < count && i < n && g[i].code == 10; ++k) {
                PolyVertex v;
                v.p.x = g[i++].number() * k_;
                if (i < n && g[i].code == 20)
                    v.p.y = g[i++].number() * k_;
                if (hasBulge && i < n && g[i].code == 42)
                    v.bulge = g[i++].number();
                v.p = f.p(v.p);
                v.bulge = f.bulge(v.bulge);
                pe.v.push_back(v);
            }
            if (pe.v.size() >= 2)
                loop.edges.emplace_back(std::move(pe));
        } else {
            std::int64_t edgeCount = 0;
            if (i < n && g[i].code == 93)
                edgeCount = g[i++].integer();
            for (std::int64_t e = 0; e < edgeCount && i < n && g[i].code == 72; ++e) {
                std::int64_t type = g[i++].integer();
                EdgeFields ef;
                bool fitSeen = false;
                while (i < n) {
                    int c = g[i].code;
                    if (type == 4 && c == 97 && !fitSeen &&
                        static_cast<std::int64_t>(ef.x10.size()) >= ef.ctrlCount) {
                        fitSeen = true;  // number of fit points of a spline edge
                        ++i;
                        continue;
                    }
                    if (endsEdge(c))
                        break;
                    double v = g[i].number();
                    switch (c) {
                        case 10:
                            ef.x10.push_back(v);
                            ef.y10.push_back(0);
                            break;
                        case 20:
                            if (!ef.y10.empty())
                                ef.y10.back() = v;
                            break;
                        case 11:
                            ef.x11.push_back(v);
                            ef.y11.push_back(0);
                            break;
                        case 21:
                            if (!ef.y11.empty())
                                ef.y11.back() = v;
                            break;
                        case 40:
                            ef.c40.push_back(v);
                            break;
                        case 42:
                            ef.c42.push_back(v);
                            break;
                        case 50:
                            ef.c50 = v;
                            break;
                        case 51:
                            ef.c51 = v;
                            break;
                        case 73:
                            ef.ccw = g[i].integer() != 0;
                            ef.rational = ef.ccw;  // same code: "rational" for splines
                            break;
                        case 74:
                            ef.periodic = g[i].integer() != 0;
                            break;
                        case 94:
                            ef.degree = static_cast<int>(g[i].integer());
                            break;
                        case 96:
                            ef.ctrlCount = g[i].integer();
                            break;
                        default:
                            break;
                    }
                    ++i;
                }
                auto pt10 = [&](std::size_t k) { return Vec2{ef.x10[k] * k_, ef.y10[k] * k_}; };
                if (type == 1 && !ef.x10.empty() && !ef.x11.empty()) {
                    HatchLineEdge le;
                    le.a = f.p(pt10(0));
                    le.b = f.p(Vec2{ef.x11[0] * k_, ef.y11[0] * k_});
                    loop.edges.emplace_back(le);
                } else if (type == 2 && !ef.x10.empty() && !ef.c40.empty() && ef.c40[0] > 0) {
                    HatchArcEdge ae;
                    ae.center = f.p(pt10(0));
                    ae.radius = ef.c40[0] * k_;
                    ae.startAngle = f.angle(ef.c50 * kDeg);
                    ae.endAngle = f.angle(ef.c51 * kDeg);
                    ae.ccw = f.mirror ? !ef.ccw : ef.ccw;
                    loop.edges.emplace_back(ae);
                } else if (type == 3 && !ef.x10.empty() && !ef.x11.empty() && !ef.c40.empty()) {
                    HatchEllipseEdge ee;
                    ee.center = f.p(pt10(0));
                    Vec2 major{ef.x11[0] * k_, ef.y11[0] * k_};
                    ee.majorAxis = f.mirror ? Vec2{-major.x, major.y} : major;
                    ee.ratio = ef.c40[0];
                    double s = ef.c50 * kDeg, en = ef.c51 * kDeg;
                    ee.startParam = f.mirror ? -s : s;
                    ee.endParam = f.mirror ? -en : en;
                    ee.ccw = f.mirror ? !ef.ccw : ef.ccw;
                    if (ee.ratio > 0 && ee.ratio <= 1 && length(ee.majorAxis) > 0)
                        loop.edges.emplace_back(ee);
                } else if (type == 4 && ef.x10.size() >= 2) {
                    HatchSplineEdge se;
                    se.spline.degree = std::clamp(ef.degree, 1, 15);
                    for (std::size_t k = 0; k < ef.x10.size(); ++k)
                        se.spline.control.push_back(f.p(pt10(k)));
                    se.spline.knots = ef.c40;
                    if (ef.rational && ef.c42.size() == ef.x10.size())
                        se.spline.weights = ef.c42;
                    if (se.spline.knots.size() != se.spline.control.size() +
                                                      static_cast<std::size_t>(se.spline.degree) +
                                                      1 ||
                        !std::is_sorted(se.spline.knots.begin(), se.spline.knots.end())) {
                        se.spline.knots.clear();
                    }
                    for (std::size_t k = 0; k < ef.x11.size(); ++k)
                        se.spline.fit.push_back(f.p({ef.x11[k] * k_, ef.y11[k] * k_}));
                    se.spline.periodic = ef.periodic;
                    if (se.spline.control.size() > static_cast<std::size_t>(se.spline.degree))
                        loop.edges.emplace_back(std::move(se));
                }
            }
        }
        // Source boundary object count (97) and handles (330) follow every path.
        while (i < n && (g[i].code == 97 || g[i].code == 330))
            ++i;
        if (!loop.edges.empty())
            h.loops.push_back(std::move(loop));
    }

    if (h.loops.empty()) {
        bad(r, "HATCH", "no usable boundary; skipped");
        return false;
    }

    h.solid = solid;
    if (!solid)
        h.pattern = pattern;
    for (std::size_t j = i; j < n; ++j) {
        if (g[j].code == 52)
            h.patternAngle = f.angle(g[j].number() * kDeg);
        else if (g[j].code == 41) {
            double s = g[j].number();
            if (std::isfinite(s) && s > 0)
                h.patternScale = s;
        }
    }
    if (r.integer(450) == 1)
        readGradient(r, i, f, h);
    out.push_back([&] {
        Entity e;
        e.data = std::move(h);
        return e;
    }());
    return true;
}

}  // namespace tbcad::dxf::detail
