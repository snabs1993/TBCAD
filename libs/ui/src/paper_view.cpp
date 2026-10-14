#include "tbcad/ui/paper_view.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "tbcad/export/plot.h"
#include "tbcad/layout/layout.h"

namespace tbcad::ui {

namespace {

using exportlib::PlotArc;
using exportlib::PlotClip;
using exportlib::PlotDots;
using exportlib::PlotFill;
using exportlib::PlotPath;

// The renderer swaps pure black and pure white to stay visible against the window background (the
// way model space works). The sheet has its own white background, so a one-step nudge keeps ink
// black and paper white whatever the theme is.
Rgba keepColor(Rgba c) {
    if (c.r == 0 && c.g == 0 && c.b == 0)
        c.r = c.g = c.b = 1;
    else if (c.r == 255 && c.g == 255 && c.b == 255)
        c.r = c.g = c.b = 254;
    return c;
}

// Liang-Barsky: the part of segment ab inside the box, as parameters; false when none.
bool clipSegment(Vec2 a, Vec2 b, const BBox2& w, double& t0, double& t1) {
    t0 = 0;
    t1 = 1;
    const Vec2 d = b - a;
    const double p[4] = {-d.x, d.x, -d.y, d.y};
    const double q[4] = {a.x - w.min.x, w.max.x - a.x, a.y - w.min.y, w.max.y - a.y};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0)
                return false;
        } else {
            const double r = q[i] / p[i];
            if (p[i] < 0) {
                if (r > t1)
                    return false;
                t0 = std::max(t0, r);
            } else {
                if (r < t0)
                    return false;
                t1 = std::min(t1, r);
            }
        }
    }
    return t0 <= t1;
}

// Sutherland-Hodgman against an axis-aligned box.
std::vector<Vec2> clipRing(std::vector<Vec2> ring, const BBox2& w) {
    auto pass = [&](auto inside, auto cross) {
        std::vector<Vec2> out;
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const Vec2 cur = ring[i], prev = ring[(i + ring.size() - 1) % ring.size()];
            const bool ci = inside(cur), pi = inside(prev);
            if (ci != pi)
                out.push_back(cross(prev, cur));
            if (ci)
                out.push_back(cur);
        }
        ring = std::move(out);
    };
    auto atX = [](Vec2 a, Vec2 b, double x) {
        return Vec2{x, a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x)};
    };
    auto atY = [](Vec2 a, Vec2 b, double y) {
        return Vec2{a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y), y};
    };
    pass([&](Vec2 p) { return p.x >= w.min.x; },
         [&](Vec2 a, Vec2 b) { return atX(a, b, w.min.x); });
    if (!ring.empty())
        pass([&](Vec2 p) { return p.x <= w.max.x; },
             [&](Vec2 a, Vec2 b) { return atX(a, b, w.max.x); });
    if (!ring.empty())
        pass([&](Vec2 p) { return p.y >= w.min.y; },
             [&](Vec2 a, Vec2 b) { return atY(a, b, w.min.y); });
    if (!ring.empty())
        pass([&](Vec2 p) { return p.y <= w.max.y; },
             [&](Vec2 a, Vec2 b) { return atY(a, b, w.max.y); });
    return ring;
}

class Builder {
public:
    Builder(render::Scene& scene, std::deque<std::vector<double>>& patterns)
        : scene_(scene), patterns_(patterns) {}

    void add(const exportlib::PlotPrimitive& prim) {
        if (const auto* c = std::get_if<PlotClip>(&prim)) {
            clip_ = c->begin ? std::optional<BBox2>(c->rect) : std::nullopt;
        } else if (const auto* p = std::get_if<PlotPath>(&prim)) {
            path(p->pts, p->closed, p->style);
        } else if (const auto* a = std::get_if<PlotArc>(&prim)) {
            arc(*a);
        } else if (const auto* f = std::get_if<PlotFill>(&prim)) {
            fill(*f);
        } else if (const auto* d = std::get_if<PlotDots>(&prim)) {
            dots(*d);
        }
    }

    void flush() {
        if (list_.items.empty())
            return;
        scene_.upsert(EntityId{nextId_++}, list_, order_++);
        list_.items.clear();
    }

    void sheet(const BBox2& box) {
        DisplayFill paper;
        paper.rings = {{box.min, {box.max.x, box.min.y}, box.max, {box.min.x, box.max.y}}};
        paper.style.color = keepColor(Rgba{255, 255, 255, 255});
        list_.items.emplace_back(std::move(paper));
        flush();
    }

private:
    DisplayStyle styleOf(const exportlib::PlotStyle& s) {
        DisplayStyle d;
        d.color = keepColor(s.color);
        d.lineweight = s.widthMm;
        if (!s.dashMm.empty()) {
            std::vector<double> dashes = s.dashMm;
            for (std::size_t i = 1; i < dashes.size(); i += 2)
                dashes[i] = -std::abs(dashes[i]);  // gaps are negative in a display pattern
            patterns_.push_back(std::move(dashes));
            d.pattern = &patterns_.back();
        }
        return d;
    }

    void emit(std::vector<Vec2> pts, bool closed, const DisplayStyle& st) {
        if (pts.size() < 2)
            return;
        DisplayPolyline pl;
        pl.pts = std::move(pts);
        pl.closed = closed;
        pl.style = st;
        list_.items.emplace_back(std::move(pl));
        if (list_.items.size() >= 4000)
            flush();
    }

    void path(const std::vector<Vec2>& pts, bool closed, const exportlib::PlotStyle& style) {
        if (pts.size() < 2)
            return;
        const DisplayStyle st = styleOf(style);
        if (!clip_) {
            emit(pts, closed, st);
            return;
        }
        std::vector<Vec2> run;
        const std::size_t n = pts.size() + (closed ? 1 : 0);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const Vec2 a = pts[i], b = pts[(i + 1) % pts.size()];
            double t0, t1;
            if (!clipSegment(a, b, *clip_, t0, t1)) {
                emit(std::move(run), false, st);
                run.clear();
                continue;
            }
            const Vec2 s = lerp(a, b, t0), e = lerp(a, b, t1);
            if (!run.empty() && distance(run.back(), s) > 1e-9) {
                emit(std::move(run), false, st);
                run.clear();
            }
            if (run.empty())
                run.push_back(s);
            run.push_back(e);
            if (t1 < 1) {
                emit(std::move(run), false, st);
                run.clear();
            }
        }
        emit(std::move(run), false, st);
    }

    void arc(const PlotArc& a) {
        if (!(a.radius > 0))
            return;
        double sweep = a.full ? kTwoPi : a.end - a.start;
        while (!a.full && sweep <= 0)
            sweep += kTwoPi;
        const double step = 2.0 * std::acos(std::clamp(1.0 - 0.03 / a.radius, -1.0, 1.0));
        const int n = std::clamp(static_cast<int>(std::ceil(sweep / std::max(step, 1e-3))), 8, 720);
        std::vector<Vec2> pts;
        pts.reserve(static_cast<std::size_t>(n) + 1);
        for (int i = 0; i <= n; ++i) {
            const double t = a.start + sweep * i / n;
            pts.push_back(
                {a.center.x + a.radius * std::cos(t), a.center.y + a.radius * std::sin(t)});
        }
        path(pts, a.full, a.style);
    }

    void fill(const PlotFill& f) {
        DisplayFill out;
        out.style.color = keepColor(f.color);
        for (const auto& ring : f.rings) {
            if (ring.size() < 3)
                continue;
            std::vector<Vec2> r = clip_ ? clipRing(ring, *clip_) : ring;
            if (r.size() >= 3)
                out.rings.push_back(std::move(r));
        }
        if (!out.rings.empty())
            list_.items.emplace_back(std::move(out));
    }

    void dots(const PlotDots& d) {
        DisplayPoints out;
        out.style = styleOf(d.style);
        for (Vec2 p : d.pts)
            if (!clip_ || clip_->contains(p))
                out.pts.push_back(p);
        if (!out.pts.empty())
            list_.items.emplace_back(std::move(out));
    }

    render::Scene& scene_;
    std::deque<std::vector<double>>& patterns_;
    std::optional<BBox2> clip_{};
    DisplayList list_{};
    std::uint32_t order_ = 1;  // 0 is the sheet itself
    std::uint64_t nextId_ = 1;
};

}  // namespace

void PaperView::clear() {
    scene_.clear();
    patterns_.clear();
    sheet_ = {};
    layout_.clear();
}

std::string PaperView::build(const Document& doc, const std::string& layoutName) {
    clear();
    auto found = layout::findLayout(doc, layoutName);
    if (!found)
        return "No layout named \"" + layoutName + "\".";
    exportlib::PlotSettings base;
    base.color = exportlib::ColorMode::AsDisplayed;
    auto built = layout::buildLayoutSheet(doc, layoutName, base);
    if (!built)
        return built.error().message;
    sheet_.expand({0, 0});
    sheet_.expand({found->widthMm, found->heightMm});
    layout_ = layoutName;
    // The sheet is drawn first (order 0), its content on top in plot order.
    Builder b(scene_, patterns_);
    b.sheet(sheet_);
    for (const auto& prim : built.value().prims)
        b.add(prim);
    b.flush();
    return {};
}

}  // namespace tbcad::ui
