// Text and shapes along a line, from the bracketed elements of a linetype (.lin file). The renderer
// only knows dashes, so these are drawn here as ordinary geometry next to the dashed line.
#include "linetype_marks.h"

#include <algorithm>
#include <cmath>

#include "tbcad/geom/geom.h"

namespace tbcad::geom {

namespace {

// Largest number of marks one object gets; beyond it they would only be noise.
constexpr std::size_t kMaxMarks = 4000;
// Marks smaller than this many pixels are not worth drawing.
constexpr double kMinPixels = 3.0;

DisplayStyle plainStyle(const DisplayStyle& host) {
    DisplayStyle s = host;
    s.pattern = nullptr;
    s.marks = nullptr;
    s.patternScale = 1;
    return s;
}

// Shapes of the standard shape file, drawn on a unit square centred on the pen position.
DisplayList shapeOutline(const std::string& name, double size, const DisplayStyle& style,
                         double pixelSizeMm) {
    std::string up = name;
    for (char& c : up)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto poly = [&](std::vector<Vec2> pts, bool closed) {
        DisplayPolyline p;
        p.style = style;
        p.closed = closed;
        for (Vec2& v : pts)
            v = v * size;
        p.pts = std::move(pts);
        return p;
    };
    DisplayList out;
    if (up == "BOX") {
        out.items.emplace_back(poly({{-0.5, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {-0.5, 0.5}}, true));
    } else if (up == "CIRC1") {
        const int n = std::clamp(static_cast<int>(size / std::max(pixelSizeMm, 1e-9)), 12, 48);
        std::vector<Vec2> pts;
        for (int i = 0; i < n; ++i) {
            const double a = kTwoPi * i / n;
            pts.push_back({0.5 * std::cos(a), 0.5 * std::sin(a)});
        }
        out.items.emplace_back(poly(std::move(pts), true));
    } else if (up == "TRACK1") {
        out.items.emplace_back(poly({{0, -0.5}, {0, 0.5}}, false));
    } else if (up == "ZIG") {
        out.items.emplace_back(poly({{-0.5, 0}, {-0.25, 0.5}, {0.25, -0.5}, {0.5, 0}}, false));
    } else if (up == "BAT") {
        out.items.emplace_back(poly({{-0.5, 0.5}, {0, -0.5}, {0.5, 0.5}}, false));
    } else if (up == "X") {
        out.items.emplace_back(poly({{-0.5, -0.5}, {0.5, 0.5}}, false));
        out.items.emplace_back(poly({{-0.5, 0.5}, {0.5, -0.5}}, false));
    }
    return out;
}

struct Placed {
    Vec2 pos;
    double c, s;  // cosine and sine of the rotation
    Vec2 map(Vec2 p) const { return {pos.x + c * p.x - s * p.y, pos.y + s * p.x + c * p.y}; }
};

void place(const DisplayList& local, const Placed& at, DisplayList& out) {
    for (const DisplayItem& item : local.items) {
        if (const auto* p = std::get_if<DisplayPolyline>(&item)) {
            DisplayPolyline q = *p;
            for (Vec2& v : q.pts)
                v = at.map(v);
            out.items.emplace_back(std::move(q));
        } else if (const auto* f = std::get_if<DisplayFill>(&item)) {
            DisplayFill q = *f;
            q.glyphRef.reset();
            for (auto& ring : q.rings)
                for (Vec2& v : ring)
                    v = at.map(v);
            out.items.emplace_back(std::move(q));
        } else if (const auto* d = std::get_if<DisplayPoints>(&item)) {
            DisplayPoints q = *d;
            for (Vec2& v : q.pts)
                v = at.map(v);
            out.items.emplace_back(std::move(q));
        }
    }
}

}  // namespace

void appendLinetypeMarks(const Document& doc, const DisplayContext& ctx,
                         const DisplayPolyline& host, DisplayList& out) {
    if (!host.style.marks || !host.style.pattern || host.pts.size() < 2)
        return;
    const double ps = host.style.patternScale > 0 ? host.style.patternScale : 1.0;
    double cycle = 0;
    for (double d : *host.style.pattern)
        cycle += std::abs(d);
    cycle *= ps;
    if (!(cycle > 0))
        return;

    // The path, closed outlines included, with the distance at each point.
    std::vector<Vec2> pts = host.pts;
    if (host.closed)
        pts.push_back(pts.front());
    std::vector<double> at(pts.size(), 0.0);
    for (std::size_t i = 1; i < pts.size(); ++i)
        at[i] = at[i - 1] + distance(pts[i - 1], pts[i]);
    const double length = at.back();
    if (!(length > 0))
        return;

    // Geometry of each mark at the origin, pointing along +X, already scaled.
    const DisplayStyle style = plainStyle(host.style);
    struct Prepared {
        const LinetypeMark* mark;
        DisplayList geometry;
        double size;
    };
    std::vector<Prepared> prepared;
    for (const LinetypeMark& m : *host.style.marks) {
        Prepared pr{&m, {}, 0};
        if (m.kind == LinetypeMark::Kind::Shape) {
            pr.size = m.scale * ps;
            if (pr.size >= kMinPixels * ctx.pixelSizeMm)
                pr.geometry = shapeOutline(m.text, pr.size, style, ctx.pixelSizeMm);
        } else {
            const TextStyle* ts = m.style.empty() ? nullptr : doc.textStyleByName(m.style);
            const double fixed = ts && ts->height > 0 ? ts->height : 2.5;
            pr.size = (m.scale > 0 ? m.scale * ps : fixed);
            if (pr.size >= kMinPixels * ctx.pixelSizeMm && !m.text.empty()) {
                Entity t;
                TextData td;
                td.height = pr.size;
                td.utf8 = m.text;
                td.style = ts ? ts->id : kTextStyleStandard;
                t.data = std::move(td);
                t.common.layer = host.style.layer;
                t.common.linetype = kLinetypeContinuous;
                DisplayList raw;
                buildDisplay(doc, t, ctx, raw);
                for (DisplayItem& item : raw.items) {
                    if (auto* p = std::get_if<DisplayPolyline>(&item))
                        p->style = style;
                    else if (auto* f = std::get_if<DisplayFill>(&item))
                        f->style = style;
                    else if (auto* d = std::get_if<DisplayPoints>(&item))
                        d->style = style;
                }
                pr.geometry = std::move(raw);
            }
        }
        if (!pr.geometry.items.empty())
            prepared.push_back(std::move(pr));
    }
    if (prepared.empty())
        return;
    if ((length / cycle + 1) * static_cast<double>(prepared.size()) >
        static_cast<double>(kMaxMarks))
        return;

    for (double base = 0; base <= length; base += cycle) {
        for (const Prepared& pr : prepared) {
            const LinetypeMark& m = *pr.mark;
            const double s = base + m.offset * ps;
            if (s > length)
                continue;
            // The segment the distance falls on.
            std::size_t seg = static_cast<std::size_t>(
                std::lower_bound(at.begin() + 1, at.end(), s) - at.begin());
            seg = std::min(seg, pts.size() - 1);
            const Vec2 a = pts[seg - 1], b = pts[seg];
            const double span = at[seg] - at[seg - 1];
            const double t = span > 0 ? (s - at[seg - 1]) / span : 0;
            const Vec2 point = a + (b - a) * t;
            double angle = std::atan2(b.y - a.y, b.x - a.x);
            switch (m.rotationKind) {
                case LinetypeMark::Rotation::Relative:
                    angle += m.rotation;
                    break;
                case LinetypeMark::Rotation::Absolute:
                    angle = m.rotation;
                    break;
                case LinetypeMark::Rotation::Upright:
                    angle = normalizeAngle(angle + m.rotation);
                    if (angle > kPi / 2 + 1e-9 && angle < 3 * kPi / 2 + 1e-9)
                        angle = normalizeAngle(angle + kPi);
                    break;
            }
            Placed pl{point, std::cos(angle), std::sin(angle)};
            const Vec2 shift = m.shift * ps;
            pl.pos = point + Vec2{pl.c * shift.x - pl.s * shift.y, pl.s * shift.x + pl.c * shift.y};
            place(pr.geometry, pl, out);
        }
    }
}

}  // namespace tbcad::geom
