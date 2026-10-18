// Raster output of a plot sheet: an anti-aliased software rasteriser (vertical 4x supersampling
// with exact horizontal coverage), then PNG (miniz) or JPEG (our own baseline encoder).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>

#include "tbcad/export/plot.h"

namespace tbcad::exportlib {

namespace {

constexpr int kSub = 4;  // vertical sub-scanlines per pixel row

struct Canvas {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgb{};
    // Current clip, pixels, half-open.
    int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
};

using Ring = std::vector<Vec2>;  // pixel coordinates, y down

struct Edge {
    double x0, y0, x1, y1;  // y0 < y1
    int dir;                // +1 when the ring ran downwards through it
};

double signedArea(const Ring& r) {
    double a = 0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const Vec2& p = r[i];
        const Vec2& q = r[(i + 1) % r.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a / 2;
}

void fillRings(Canvas& cv, const std::vector<Ring>& rings, bool evenOdd, Rgba color) {
    std::vector<Edge> edges;
    double ymin = 1e300, ymax = -1e300, xmin = 1e300, xmax = -1e300;
    for (const Ring& r : rings) {
        if (r.size() < 3)
            continue;
        for (std::size_t i = 0; i < r.size(); ++i) {
            Vec2 p = r[i], q = r[(i + 1) % r.size()];
            xmin = std::min({xmin, p.x, q.x});
            xmax = std::max({xmax, p.x, q.x});
            if (p.y == q.y)
                continue;
            int dir = 1;
            if (p.y > q.y) {
                std::swap(p, q);
                dir = -1;
            }
            edges.push_back({p.x, p.y, q.x, q.y, dir});
            ymin = std::min(ymin, p.y);
            ymax = std::max(ymax, q.y);
        }
    }
    if (edges.empty())
        return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    const int row0 = std::max(cv.cy0, static_cast<int>(std::floor(ymin)));
    const int row1 = std::min(cv.cy1, static_cast<int>(std::ceil(ymax)));
    const int colMin = std::max(cv.cx0, static_cast<int>(std::floor(xmin)));
    const int colMax = std::min(cv.cx1, static_cast<int>(std::ceil(xmax)));
    if (row0 >= row1 || colMin >= colMax)
        return;

    std::vector<float> cov(static_cast<std::size_t>(colMax - colMin) + 2);
    std::vector<const Edge*> active;
    std::size_t next = 0;
    std::vector<std::pair<double, int>> xs;
    for (int y = row0; y < row1; ++y) {
        // Edges that start before the end of this row; drop the ones that ended.
        while (next < edges.size() && edges[next].y0 < y + 1)
            active.push_back(&edges[next++]);
        active.erase(
            std::remove_if(active.begin(), active.end(), [y](const Edge* e) { return e->y1 <= y; }),
            active.end());
        if (active.empty())
            continue;
        std::fill(cov.begin(), cov.end(), 0.0f);
        bool any = false;
        for (int s = 0; s < kSub; ++s) {
            const double sy = y + (s + 0.5) / kSub;
            xs.clear();
            for (const Edge* e : active)
                if (e->y0 <= sy && sy < e->y1)
                    xs.emplace_back(e->x0 + (sy - e->y0) / (e->y1 - e->y0) * (e->x1 - e->x0),
                                    e->dir);
            if (xs.size() < 2)
                continue;
            std::sort(xs.begin(), xs.end());
            int winding = 0;
            for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
                winding += xs[i].second;
                const bool inside = evenOdd ? ((i + 1) % 2 == 1) : winding != 0;
                if (!inside)
                    continue;
                double xa = std::max<double>(xs[i].first, colMin);
                double xb = std::min<double>(xs[i + 1].first, colMax);
                if (xb <= xa)
                    continue;
                any = true;
                const int ia = static_cast<int>(std::floor(xa)),
                          ib = static_cast<int>(std::floor(xb));
                if (ia == ib) {
                    cov[static_cast<std::size_t>(ia - colMin)] += static_cast<float>(xb - xa);
                } else {
                    cov[static_cast<std::size_t>(ia - colMin)] += static_cast<float>(ia + 1 - xa);
                    for (int x = ia + 1; x < ib; ++x)
                        cov[static_cast<std::size_t>(x - colMin)] += 1.0f;
                    cov[static_cast<std::size_t>(ib - colMin)] += static_cast<float>(xb - ib);
                }
            }
        }
        if (!any)
            continue;
        std::uint8_t* row = cv.rgb.data() + static_cast<std::size_t>(y) * cv.w * 3;
        for (int x = colMin; x < colMax; ++x) {
            const float a = std::min(1.0f, cov[static_cast<std::size_t>(x - colMin)] / kSub);
            if (a <= 0)
                continue;
            std::uint8_t* px = row + static_cast<std::size_t>(x) * 3;
            px[0] = static_cast<std::uint8_t>(px[0] + (color.r - px[0]) * a + 0.5f);
            px[1] = static_cast<std::uint8_t>(px[1] + (color.g - px[1]) * a + 0.5f);
            px[2] = static_cast<std::uint8_t>(px[2] + (color.b - px[2]) * a + 0.5f);
        }
    }
}

Ring circle(Vec2 c, double r) {
    const int n = std::clamp(
        static_cast<int>(std::ceil(kPi / std::acos(std::max(0.0, 1.0 - 0.05 / std::max(r, 0.06))))),
        8, 64);
    Ring ring;
    ring.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double a = kTwoPi * i / n;
        ring.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return ring;
}

void makePositive(Ring& r) {
    if (signedArea(r) < 0)
        std::reverse(r.begin(), r.end());
}

// A thick polyline as the union of its segment rectangles and (for round style) vertex discs.
void strokePolyline(Canvas& cv, const std::vector<Vec2>& pts, bool closed, double widthPx,
                    bool round, Rgba color) {
    if (pts.empty())
        return;
    const double hw = std::max(widthPx, 1.0) / 2;
    std::vector<Ring> rings;
    const std::size_t n = pts.size();
    const std::size_t segs = closed ? n : n - 1;
    for (std::size_t i = 0; i < segs; ++i) {
        Vec2 p = pts[i], q = pts[(i + 1) % n];
        Vec2 d = q - p;
        double len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 1e-9)
            continue;
        Vec2 nrm{-d.y / len * hw, d.x / len * hw};
        Ring quad{p + nrm, q + nrm, q - nrm, p - nrm};
        makePositive(quad);
        rings.push_back(std::move(quad));
    }
    if (round || n == 1) {
        for (const Vec2& p : pts) {
            Ring c = circle(p, hw);
            makePositive(c);
            rings.push_back(std::move(c));
        }
    }
    if (!rings.empty())
        fillRings(cv, rings, false, color);
}

// Splits a polyline into the "on" pieces of a dash pattern (lengths in pixels).
void dashPolyline(Canvas& cv, const std::vector<Vec2>& pts, bool closed,
                  const std::vector<double>& dashPx, double widthPx, Rgba color) {
    std::vector<Vec2> path = pts;
    if (closed && !pts.empty())
        path.push_back(pts.front());
    std::size_t di = 0;
    double left = dashPx[0];
    bool on = true;
    std::vector<Vec2> cur;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        Vec2 p = path[i], q = path[i + 1];
        Vec2 d = q - p;
        double len = std::sqrt(d.x * d.x + d.y * d.y);
        double pos = 0;
        while (len - pos > 1e-9) {
            const double step = std::min(left, len - pos);
            Vec2 a = p + d * (pos / len), b = p + d * ((pos + step) / len);
            if (on) {
                if (cur.empty())
                    cur.push_back(a);
                cur.push_back(b);
            }
            pos += step;
            left -= step;
            if (left <= 1e-9) {
                if (on && cur.size() >= 2) {
                    strokePolyline(cv, cur, false, widthPx, false, color);
                    cur.clear();
                } else if (on) {
                    cur.clear();
                }
                di = (di + 1) % dashPx.size();
                left = std::max(dashPx[di], 1e-3);
                on = !on;
                if (!on)
                    cur.clear();
            }
        }
    }
    if (on && cur.size() >= 2)
        strokePolyline(cv, cur, false, widthPx, false, color);
}

struct Mapper {
    double k;  // pixels per mm
    int height;
    Vec2 px(Vec2 mm) const { return {mm.x * k, height - mm.y * k}; }
};

// Pixels of `cv` inside the clip rectangle but outside the polygon (pixel coordinates, even-odd)
// take their colour back from `before`: how a non-rectangular clip is applied after drawing.
void restoreOutside(Canvas& cv, const std::vector<std::uint8_t>& before, const Ring& poly) {
    if (poly.size() < 3)
        return;
    std::vector<double> xs;
    for (int y = cv.cy0; y < cv.cy1; ++y) {
        const double yc = y + 0.5;
        xs.clear();
        for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const Vec2 a = poly[j], b = poly[i];
            if ((a.y > yc) != (b.y > yc))
                xs.push_back(a.x + (yc - a.y) * (b.x - a.x) / (b.y - a.y));
        }
        std::sort(xs.begin(), xs.end());
        std::size_t row = static_cast<std::size_t>(y) * cv.w * 3;
        int x = cv.cx0;
        auto restore = [&](int from, int to) {
            from = std::max(from, cv.cx0);
            to = std::min(to, cv.cx1);
            if (to > from)
                std::copy(before.begin() + row + static_cast<std::size_t>(from) * 3,
                          before.begin() + row + static_cast<std::size_t>(to) * 3,
                          cv.rgb.begin() + row + static_cast<std::ptrdiff_t>(from) * 3);
        };
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
            restore(x, static_cast<int>(std::ceil(xs[k] - 0.5)));
            x = std::max(x, static_cast<int>(std::ceil(xs[k + 1] - 0.5)));
        }
        restore(x, cv.cx1);
    }
}

// A picture: every device pixel inside its (possibly turned) rectangle takes the average of 2x2
// samples of the picture, blended over the sheet by the picture's opacity and alpha.
void drawPicture(Canvas& cv, const PlotImage& im, const std::array<Vec2, 4>& dev) {
    const ImageData& src = *im.data;
    if (src.width <= 0 || src.height <= 0)
        return;
    const Vec2 o = dev[0], ex = dev[1] - dev[0], ey = dev[3] - dev[0];
    const double det = ex.x * ey.y - ex.y * ey.x;
    if (std::abs(det) < 1e-9)
        return;
    double x0 = dev[0].x, x1 = x0, y0 = dev[0].y, y1 = y0;
    for (const Vec2& d : dev) {
        x0 = std::min(x0, d.x);
        x1 = std::max(x1, d.x);
        y0 = std::min(y0, d.y);
        y1 = std::max(y1, d.y);
    }
    const int colMin = std::max(cv.cx0, static_cast<int>(std::floor(x0)));
    const int colMax = std::min(cv.cx1, static_cast<int>(std::ceil(x1)));
    const int rowMin = std::max(cv.cy0, static_cast<int>(std::floor(y0)));
    const int rowMax = std::min(cv.cy1, static_cast<int>(std::ceil(y1)));
    for (int y = rowMin; y < rowMax; ++y) {
        std::uint8_t* row = cv.rgb.data() + static_cast<std::size_t>(y) * cv.w * 3;
        for (int x = colMin; x < colMax; ++x) {
            double sum[4] = {0, 0, 0, 0};
            int hits = 0;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const double dx = x + 0.25 + 0.5 * i - o.x, dy = y + 0.25 + 0.5 * j - o.y;
                    const double sf = (dx * ey.y - dy * ey.x) / det;
                    const double tf = (ex.x * dy - ex.y * dx) / det;
                    if (sf < 0 || sf >= 1 || tf < 0 || tf >= 1)
                        continue;
                    const int ix = std::min(static_cast<int>(sf * src.width), src.width - 1);
                    const int iy =
                        std::min(static_cast<int>((1 - tf) * src.height), src.height - 1);
                    const std::uint8_t* px =
                        src.rgba.data() + (static_cast<std::size_t>(iy) * src.width + ix) * 4;
                    const double a = px[3] / 255.0;
                    sum[0] += px[0] * a;
                    sum[1] += px[1] * a;
                    sum[2] += px[2] * a;
                    sum[3] += a;
                    ++hits;
                }
            if (!hits)
                continue;
            // sum[0..2] are premultiplied; the missing samples (edge) count as transparent.
            const double cover = sum[3] / 4.0 * im.opacity;
            if (cover <= 0)
                continue;
            const double scale = im.opacity / 4.0;
            for (int c = 0; c < 3; ++c) {
                const double v = row[x * 3 + c] * (1 - cover) + sum[c] * scale;
                row[x * 3 + c] = static_cast<std::uint8_t>(std::clamp(v + 0.5, 0.0, 255.0));
            }
        }
    }
}

}  // namespace

Result<RasterImage> rasterize(const PlotSheet& sheet, double dpi, std::int64_t maxPixels) {
    if (!(dpi >= 10) || dpi > 2400)
        return Error{ErrorCode::InvalidArgument, "The resolution must be between 10 and 2400 dpi"};
    const PlotLayout& lay = sheet.layout;
    const double k = dpi / 25.4;
    const std::int64_t w = static_cast<std::int64_t>(std::llround(lay.pageWidthMm * k));
    const std::int64_t h = static_cast<std::int64_t>(std::llround(lay.pageHeightMm * k));
    if (w < 1 || h < 1)
        return Error{ErrorCode::InvalidArgument, "The page is empty"};
    if (w * h > maxPixels)
        return Error{ErrorCode::OutOfRange, "The image would be " + std::to_string(w) + " x " +
                                                std::to_string(h) +
                                                " pixels; choose a lower resolution"};
    Canvas cv;
    cv.w = static_cast<int>(w);
    cv.h = static_cast<int>(h);
    cv.rgb.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3, 255);
    const Mapper m{k, cv.h};

    auto setClip = [&](const BBox2& r) {
        Vec2 a = m.px(r.min), b = m.px(r.max);
        cv.cx0 = std::clamp(static_cast<int>(std::floor(std::min(a.x, b.x))), 0, cv.w);
        cv.cx1 = std::clamp(static_cast<int>(std::ceil(std::max(a.x, b.x))), 0, cv.w);
        cv.cy0 = std::clamp(static_cast<int>(std::floor(std::min(a.y, b.y))), 0, cv.h);
        cv.cy1 = std::clamp(static_cast<int>(std::ceil(std::max(a.y, b.y))), 0, cv.h);
    };
    std::vector<std::array<int, 4>> stack;
    struct PolyClip {
        std::vector<std::uint8_t> before{};
        Ring ring{};
    };
    std::vector<PolyClip> polys;
    setClip(lay.printable);
    auto saveClip = [&] { return std::array<int, 4>{cv.cx0, cv.cy0, cv.cx1, cv.cy1}; };

    auto stroke = [&](const std::vector<Vec2>& mmPts, bool closed, const PlotStyle& st) {
        std::vector<Vec2> pts;
        pts.reserve(mmPts.size());
        for (const Vec2& p : mmPts)
            pts.push_back(m.px(p));
        const double widthPx = st.widthMm * k;
        if (!st.dashMm.empty()) {
            std::vector<double> d;
            for (double v : st.dashMm)
                d.push_back(v * k);
            double total = 0;
            for (double v : d)
                total += v;
            if (total > 0.5) {
                dashPolyline(cv, pts, closed, d, widthPx, st.color);
                return;
            }
        }
        strokePolyline(cv, pts, closed, widthPx, true, st.color);
    };

    for (const PlotPrimitive& prim : sheet.prims) {
        if (const auto* p = std::get_if<PlotPath>(&prim)) {
            stroke(p->pts, p->closed, p->style);
        } else if (const auto* a = std::get_if<PlotArc>(&prim)) {
            const double rPx = a->radius * k;
            const double sweep = a->full ? kTwoPi : (a->end - a->start);
            const double step = 2 * std::acos(std::max(0.0, 1.0 - 0.05 / std::max(rPx, 0.06)));
            const int n =
                std::clamp(static_cast<int>(std::ceil(sweep / std::max(step, 1e-3))), 8, 20000);
            std::vector<Vec2> pts;
            for (int i = 0; i <= n; ++i) {
                const double ang = (a->full ? 0.0 : a->start) + sweep * i / n;
                pts.push_back(a->center + Vec2{std::cos(ang), std::sin(ang)} * a->radius);
            }
            if (a->full)
                pts.pop_back();
            stroke(pts, a->full, a->style);
        } else if (const auto* f = std::get_if<PlotFill>(&prim)) {
            std::vector<Ring> rings;
            for (const auto& ring : f->rings) {
                Ring r;
                for (const Vec2& v : ring)
                    r.push_back(m.px(v));
                rings.push_back(std::move(r));
            }
            fillRings(cv, rings, true, f->color);
        } else if (const auto* d = std::get_if<PlotDots>(&prim)) {
            const double r = std::max(d->style.widthMm * k, 1.0) / 2;
            for (const Vec2& v : d->pts) {
                std::vector<Ring> one{circle(m.px(v), r)};
                fillRings(cv, one, false, d->style.color);
            }
        } else if (const auto* im = std::get_if<PlotImage>(&prim)) {
            drawPicture(cv, *im,
                        {m.px(im->corners[0]), m.px(im->corners[1]), m.px(im->corners[2]),
                         m.px(im->corners[3])});
        } else if (const auto* c = std::get_if<PlotClip>(&prim)) {
            if (c->begin) {
                stack.push_back(saveClip());
                const auto old = saveClip();
                setClip(c->rect);
                cv.cx0 = std::max(cv.cx0, old[0]);
                cv.cy0 = std::max(cv.cy0, old[1]);
                cv.cx1 = std::min(cv.cx1, old[2]);
                cv.cy1 = std::min(cv.cy1, old[3]);
                polys.emplace_back();
                if (c->poly.size() >= 3) {  // draw inside the box, then undo what is outside
                    polys.back().before = cv.rgb;
                    for (const Vec2& v : c->poly)
                        polys.back().ring.push_back(m.px(v));
                }
            } else if (!stack.empty()) {
                if (!polys.empty()) {
                    if (!polys.back().ring.empty())
                        restoreOutside(cv, polys.back().before, polys.back().ring);
                    polys.pop_back();
                }
                const auto o = stack.back();
                stack.pop_back();
                cv.cx0 = o[0];
                cv.cy0 = o[1];
                cv.cx1 = o[2];
                cv.cy1 = o[3];
            }
        }
    }
    RasterImage img;
    img.width = cv.w;
    img.height = cv.h;
    img.dpi = dpi;
    img.rgb = std::move(cv.rgb);
    return img;
}

}  // namespace tbcad::exportlib
