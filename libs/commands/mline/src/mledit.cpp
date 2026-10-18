// MLEDIT: fixes the joints of multilines: a corner join of two walls, adding or deleting a vertex,
// closing or opening the path.
#include <algorithm>
#include <cmath>
#include <optional>

#include "tbcad/cmd/cmd.h"
#include "tbcad/mline/mline.h"

namespace tbcad::mlinecmd {

namespace {

bool isMline(const Entity& e) {
    return kind(e) == EntityKind::MLine;
}

// Where the lines through (a0, a1) and (b0, b1) meet; empty when they are parallel.
std::optional<Vec2> meet(Vec2 a0, Vec2 a1, Vec2 b0, Vec2 b1) {
    const Vec2 da = a1 - a0, db = b1 - b0;
    const double den = da.x * db.y - da.y * db.x;
    if (std::abs(den) < 1e-12 * (std::abs(da.x * db.y) + std::abs(da.y * db.x) + 1e-300))
        return std::nullopt;
    const Vec2 d = b0 - a0;
    const double t = (d.x * db.y - d.y * db.x) / den;
    return a0 + da * t;
}

// The end of the path nearest to `p`: true for the first vertex, false for the last.
bool nearerToStart(const MlineData& m, Vec2 p) {
    return distance(p, m.v.front()) <= distance(p, m.v.back());
}

// ---- cuts, tees and crosses ---------------------------------------------------------------------

// Offsets of the outer lines of the multiline, from its path, in drawing units (scale and
// justification applied), positive to the left of the path direction.
struct Band {
    double lo = 0;
    double hi = 0;
    std::size_t lines = 0;
};

Band bandOf(const MlineData& m, const mline::Style& st) {
    Band b;
    if (st.offsets.empty())
        return b;
    double lo = *std::min_element(st.offsets.begin(), st.offsets.end());
    double hi = *std::max_element(st.offsets.begin(), st.offsets.end());
    double shift = m.justification == 0 ? -hi : m.justification == 2 ? -lo : 0;
    b.lo = (lo + shift) * m.scale;
    b.hi = (hi + shift) * m.scale;
    b.lines = st.offsets.size();
    return b;
}

struct SegPick {
    std::size_t seg = 0;
    double t = 0;  // along the segment, 0..1
    double dist = 1e300;
};

SegPick nearestOnPath(const MlineData& m, Vec2 p) {
    SegPick best;
    const std::size_t n = m.v.size();
    const std::size_t segs = m.closed ? n : n - 1;
    for (std::size_t i = 0; i < segs; ++i) {
        const Vec2 a = m.v[i], b = m.v[(i + 1) % n];
        const Vec2 d = b - a;
        const double len2 = d.x * d.x + d.y * d.y;
        double t = len2 > 0 ? ((p - a).x * d.x + (p - a).y * d.y) / len2 : 0;
        t = std::clamp(t, 0.0, 1.0);
        const double dist = distance(p, a + d * t);
        if (dist < best.dist)
            best = {i, t, dist};
    }
    return best;
}

Vec2 segA(const MlineData& m, std::size_t i) {
    return m.v[i];
}
Vec2 segB(const MlineData& m, std::size_t i) {
    return m.v[(i + 1) % m.v.size()];
}

// Where the paths of two multilines cross: the first pair of segments that intersect.
struct Crossing {
    std::size_t sa = 0, sb = 0;
    Vec2 at{};
};

std::optional<Crossing> crossingOf(const MlineData& a, const MlineData& b) {
    const std::size_t na = a.v.size(), nb = b.v.size();
    const std::size_t ea = a.closed ? na : na - 1, eb = b.closed ? nb : nb - 1;
    for (std::size_t i = 0; i < ea; ++i)
        for (std::size_t j = 0; j < eb; ++j) {
            const Vec2 a0 = segA(a, i), a1 = segB(a, i), b0 = segA(b, j), b1 = segB(b, j);
            const Vec2 da = a1 - a0, db = b1 - b0;
            const double den = da.x * db.y - da.y * db.x;
            if (std::abs(den) < 1e-12)
                continue;
            const Vec2 d = b0 - a0;
            const double t = (d.x * db.y - d.y * db.x) / den;
            const double u = (d.x * da.y - d.y * da.x) / den;
            if (t >= -1e-9 && t <= 1 + 1e-9 && u >= -1e-9 && u <= 1 + 1e-9)
                return Crossing{i, j, a0 + da * t};
        }
    return std::nullopt;
}

// The part of segment `seg` of `m` that lies inside the band of `other` along its segment `os`.
// Parameters of `m`'s segment; empty when the segment runs along the band.
std::optional<std::pair<double, double>> bandInterval(const MlineData& m, std::size_t seg,
                                                      const MlineData& other, std::size_t os,
                                                      const Band& band) {
    const Vec2 b0 = segA(other, os), b1 = segB(other, os);
    const Vec2 dir = normalized(b1 - b0);
    const Vec2 nrm{-dir.y, dir.x};
    const Vec2 a0 = segA(m, seg), a1 = segB(m, seg);
    const double d0 = dot(nrm, a0 - b0), d1 = dot(nrm, a1 - b0);
    if (std::abs(d1 - d0) < 1e-12)
        return std::nullopt;
    double t0 = (band.lo - d0) / (d1 - d0), t1 = (band.hi - d0) / (d1 - d0);
    if (t0 > t1)
        std::swap(t0, t1);
    t0 = std::max(t0, 0.0);
    t1 = std::min(t1, 1.0);
    if (t1 - t0 < 1e-9)
        return std::nullopt;
    return std::make_pair(t0, t1);
}

MlineCut cut(std::size_t seg, std::pair<double, double> iv, std::size_t lo, std::size_t hi) {
    MlineCut c;
    c.seg = static_cast<std::uint32_t>(seg);
    c.t0 = iv.first;
    c.t1 = iv.second;
    c.lo = static_cast<std::uint8_t>(lo);
    c.hi = static_cast<std::uint8_t>(hi);
    return c;
}

class MlEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MLEDIT"; }
    std::string_view description() const override {
        return "Join multiline corners, add or delete vertices, close or open";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Corner", "Add", "Delete", "Close", "Open", "Cross", "Tee", "Cut", "Weld"};
        ko.defaultKeyword = "Corner";
        auto k = co_await ctx.getKeyword(
            "Multiline edit [Corner join/Add vertex/Delete vertex/Close/Open/Cross/Tee/Cut/Weld] "
            "<Corner>",
            ko);
        if (!k.ok())
            co_return;
        cmd::EntityOptions eo;
        eo.filter = isMline;
        ctx.setUndoLabel("MLEDIT");

        if (k.keyword == "Corner") {
            auto first =
                co_await ctx.getEntity("Select the first multiline (near the end to join)", eo);
            if (!first.ok())
                co_return;
            auto second =
                co_await ctx.getEntity("Select the second multiline (near the end to join)", eo);
            if (!second.ok())
                co_return;
            if (first.id == second.id) {
                ctx.message("Select two different multilines.");
                co_return;
            }
            const auto* a = entityAs<MlineData>(*ctx.doc().find(first.id));
            const auto* b = entityAs<MlineData>(*ctx.doc().find(second.id));
            if (!a || !b || a->v.size() < 2 || b->v.size() < 2 || a->closed || b->closed) {
                ctx.message("Open multilines with at least two points are needed.");
                co_return;
            }
            const bool aStart = nearerToStart(*a, first.pickPoint);
            const bool bStart = nearerToStart(*b, second.pickPoint);
            const std::size_t ae = aStart ? 0 : a->v.size() - 1, an = aStart ? 1 : a->v.size() - 2;
            const std::size_t be = bStart ? 0 : b->v.size() - 1, bn = bStart ? 1 : b->v.size() - 2;
            auto corner = meet(a->v[an], a->v[ae], b->v[bn], b->v[be]);
            if (!corner) {
                ctx.message("The two walls are parallel; they have no corner.");
                co_return;
            }
            const Vec2 c = *corner;
            (void)ctx.tx().modify(first.id, [&](Entity& e) { entityAs<MlineData>(e)->v[ae] = c; });
            (void)ctx.tx().modify(second.id, [&](Entity& e) { entityAs<MlineData>(e)->v[be] = c; });
            ctx.message("Corner joined.");
            co_return;
        }

        if (k.keyword == "Cross" || k.keyword == "Tee") {
            co_await crossOrTee(ctx, k.keyword == "Tee", eo);
            co_return;
        }

        auto pick = co_await ctx.getEntity("Select a multiline", eo);
        if (!pick.ok())
            co_return;
        const auto* m = entityAs<MlineData>(*ctx.doc().find(pick.id));
        if (!m)
            co_return;

        if (k.keyword == "Cut" || k.keyword == "Weld") {
            co_await cutOrWeld(ctx, pick.id, pick.pickPoint, k.keyword == "Weld");
            co_return;
        }

        if (k.keyword == "Close" || k.keyword == "Open") {
            const bool close = k.keyword == "Close";
            if (m->closed == close) {
                ctx.message(close ? "It is closed already." : "It is open already.");
                co_return;
            }
            if (close && m->v.size() < 3) {
                ctx.message("A closed multiline needs three points.");
                co_return;
            }
            (void)ctx.tx().modify(pick.id, [&](Entity& e) {
                auto* md = entityAs<MlineData>(e);
                md->closed = close;
                if (!close) {  // the closing segment is gone, and so are its gaps
                    const std::uint32_t last = static_cast<std::uint32_t>(md->v.size() - 1);
                    std::erase_if(md->cuts, [&](const MlineCut& c) { return c.seg >= last; });
                }
            });
            co_return;
        }

        if (k.keyword == "Add") {
            auto at = co_await ctx.getPoint("Specify the new vertex");
            if (!at.ok())
                co_return;
            // The vertex goes into the segment nearest to the point.
            std::size_t best = 0;
            double bestDist = 1e300;
            const std::size_t segs = m->closed ? m->v.size() : m->v.size() - 1;
            for (std::size_t i = 0; i < segs; ++i) {
                const Vec2 p = m->v[i], q = m->v[(i + 1) % m->v.size()];
                const Vec2 d = q - p;
                const double len2 = d.x * d.x + d.y * d.y;
                double t = len2 > 0 ? ((at.point - p).x * d.x + (at.point - p).y * d.y) / len2 : 0;
                t = std::clamp(t, 0.0, 1.0);
                const double dist = distance(at.point, p + d * t);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = i;
                }
            }
            (void)ctx.tx().modify(pick.id, [&](Entity& e) {
                auto& v = entityAs<MlineData>(e)->v;
                v.insert(v.begin() + static_cast<std::ptrdiff_t>(best) + 1, at.point);
                auto& cuts = entityAs<MlineData>(e)->cuts;  // the split segment loses its gaps
                std::erase_if(cuts, [&](const MlineCut& c) { return c.seg == best; });
                for (MlineCut& c : cuts)
                    if (c.seg > best)
                        ++c.seg;
            });
            co_return;
        }

        // Delete: the vertex nearest to the pick.
        if (m->v.size() <= 2) {
            ctx.message("A multiline keeps at least two points.");
            co_return;
        }
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < m->v.size(); ++i)
            if (distance(m->v[i], pick.pickPoint) < distance(m->v[nearest], pick.pickPoint))
                nearest = i;
        (void)ctx.tx().modify(pick.id, [&](Entity& e) {
            auto& v = entityAs<MlineData>(e)->v;
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(nearest));
            entityAs<MlineData>(e)->cuts.clear();
            if (v.size() < 3)
                entityAs<MlineData>(e)->closed = false;
        });
    }

private:
    static const mline::Style styleOf(cmd::CommandContext& ctx, const MlineData& m) {
        auto st = mline::findStyle(ctx.doc(), m.style);
        return st ? *st : mline::standardStyle();
    }

    // Cut: remove stretches of lines between two points of one segment; Weld: put them back.
    static cmd::Task cutOrWeld(cmd::CommandContext& ctx, EntityId id, Vec2 first, bool weld) {
        cmd::PointOptions po;
        po.base = first;
        auto second = co_await ctx.getPoint(
            weld ? "Specify the second weld point" : "Specify the second cut point", po);
        if (!second.ok())
            co_return;
        const auto* m = entityAs<MlineData>(*ctx.doc().find(id));
        if (!m)
            co_return;
        const SegPick a = nearestOnPath(*m, first), b = nearestOnPath(*m, second.point);
        if (a.seg != b.seg) {
            ctx.message("Both points must be on the same stretch between two vertices.");
            co_return;
        }
        const double t0 = std::min(a.t, b.t), t1 = std::max(a.t, b.t);
        if (t1 - t0 < 1e-9) {
            ctx.message("The two points are the same.");
            co_return;
        }
        std::size_t lo = 0, hi = 255;
        if (!weld) {
            cmd::KeywordOptions ko;
            ko.keywords = {"All", "Single"};
            ko.defaultKeyword = "All";
            auto w = co_await ctx.getKeyword("Cut [All elements/Single element] <All>", ko);
            if (!w.ok())
                co_return;
            if (w.keyword == "Single") {
                // The line nearest to the first point.
                const mline::Style st = styleOf(ctx, *m);
                const mline::Strokes sk = mline::strokes(*m, st);
                std::size_t best = 0;
                double bestDist = 1e300;
                for (std::size_t i = 0; i < st.offsets.size() && i < sk.lines.size(); ++i) {
                    const auto& l = sk.lines[i];
                    for (std::size_t j = 0; j + 1 < l.size(); ++j) {
                        const Vec2 d = l[j + 1] - l[j];
                        const double len2 = d.x * d.x + d.y * d.y;
                        double t = len2 > 0 ? dot(first - l[j], d) / len2 : 0;
                        t = std::clamp(t, 0.0, 1.0);
                        const double dist = distance(first, l[j] + d * t);
                        if (dist < bestDist) {
                            bestDist = dist;
                            best = i;
                        }
                    }
                }
                std::size_t rank = 0;
                for (std::size_t i = 0; i < st.offsets.size(); ++i)
                    if (st.offsets[i] < st.offsets[best])
                        ++rank;
                lo = hi = rank;
            }
        }
        std::size_t removed = 0;
        (void)ctx.tx().modify(id, [&](Entity& e) {
            auto& cuts = entityAs<MlineData>(e)->cuts;
            if (weld) {
                removed = std::erase_if(cuts, [&](const MlineCut& c) {
                    return c.seg == a.seg && c.t0 < t1 && c.t1 > t0;
                });
            } else {
                cuts.push_back(cut(a.seg, {t0, t1}, lo, hi));
            }
        });
        if (weld)
            ctx.message(removed ? "Welded." : "There was no gap there.");
    }

    // Tees and crosses between two multilines.
    static cmd::Task crossOrTee(cmd::CommandContext& ctx, bool tee, const cmd::EntityOptions& eo) {
        cmd::KeywordOptions ko;
        if (tee)
            ko.keywords = {"Open", "Closed"};
        else
            ko.keywords = {"Open", "Closed", "Merged"};
        ko.defaultKeyword = "Closed";
        auto style = co_await ctx.getKeyword(
            tee ? "Tee [Open/Closed] <Closed>" : "Cross [Open/Closed/Merged] <Closed>", ko);
        if (!style.ok())
            co_return;
        auto first = co_await ctx.getEntity(
            tee ? "Select the multiline that ends at the other (near its end)"
                : "Select the first multiline",
            eo);
        if (!first.ok())
            co_return;
        auto second = co_await ctx.getEntity("Select the second multiline", eo);
        if (!second.ok())
            co_return;
        if (first.id == second.id) {
            ctx.message("Select two different multilines.");
            co_return;
        }
        const auto* a = entityAs<MlineData>(*ctx.doc().find(first.id));
        const auto* b = entityAs<MlineData>(*ctx.doc().find(second.id));
        if (!a || !b || a->v.size() < 2 || b->v.size() < 2) {
            ctx.message("Multilines with at least two points are needed.");
            co_return;
        }
        const mline::Style sa = styleOf(ctx, *a), sb = styleOf(ctx, *b);
        const Band ba = bandOf(*a, sa), bb = bandOf(*b, sb);

        if (!tee) {
            auto x = crossingOf(*a, *b);
            if (!x) {
                ctx.message("The two multilines do not cross.");
                co_return;
            }
            const auto ia = bandInterval(*a, x->sa, *b, x->sb, bb);
            const auto ib = bandInterval(*b, x->sb, *a, x->sa, ba);
            if (!ia || !ib) {
                ctx.message("The two multilines run alongside each other here.");
                co_return;
            }
            std::vector<MlineCut> forA, forB;
            if (style.keyword == "Closed") {
                forA.push_back(cut(x->sa, *ia, 0, 255));
            } else if (style.keyword == "Open") {
                forA.push_back(cut(x->sa, *ia, 0, 255));
                forB.push_back(cut(x->sb, *ib, 0, 0));
                forB.push_back(cut(x->sb, *ib, sb.offsets.size() - 1, sb.offsets.size() - 1));
            } else {  // Merged: the outer lines of both go through, the inner ones stop
                if (sa.offsets.size() < 3 || sb.offsets.size() < 3) {
                    ctx.message("A merged cross needs styles with at least three lines.");
                    co_return;
                }
                forA.push_back(cut(x->sa, *ia, 1, sa.offsets.size() - 2));
                forB.push_back(cut(x->sb, *ib, 1, sb.offsets.size() - 2));
            }
            (void)ctx.tx().modify(first.id, [&](Entity& e) {
                for (const MlineCut& c : forA)
                    entityAs<MlineData>(e)->cuts.push_back(c);
            });
            if (!forB.empty())
                (void)ctx.tx().modify(second.id, [&](Entity& e) {
                    for (const MlineCut& c : forB)
                        entityAs<MlineData>(e)->cuts.push_back(c);
                });
            ctx.message(style.keyword + " cross made.");
            co_return;
        }

        // Tee: the first multiline ends at the second. Its end moves to the near edge of the
        // second.
        if (a->closed) {
            ctx.message("A closed multiline has no end to bring to the other one.");
            co_return;
        }
        const bool atStart = nearerToStart(*a, first.pickPoint);
        const std::size_t end = atStart ? 0 : a->v.size() - 1;
        const std::size_t prev = atStart ? 1 : a->v.size() - 2;
        const SegPick on = nearestOnPath(*b, a->v[end]);
        const Vec2 b0 = segA(*b, on.seg), b1 = segB(*b, on.seg);
        auto x = meet(a->v[prev], a->v[end], b0, b1);
        if (!x) {
            ctx.message("The two multilines are parallel; they cannot make a tee.");
            co_return;
        }
        const Vec2 bdir = normalized(b1 - b0);
        const Vec2 nrm{-bdir.y, bdir.x};
        const Vec2 out = normalized(a->v[prev] - *x);  // from the meeting point back along A
        const double side = dot(nrm, out);
        if (std::abs(side) < 1e-9) {
            ctx.message("The two multilines run along each other; they cannot make a tee.");
            co_return;
        }
        const double near = side > 0 ? bb.hi : bb.lo;
        const Vec2 edge = *x + out * (near / side);
        (void)ctx.tx().modify(first.id, [&](Entity& e) { entityAs<MlineData>(e)->v[end] = edge; });
        if (style.keyword == "Open") {
            // The near outer line of the second multiline stops where the first one joins it.
            MlineData moved = *a;
            moved.v[end] = edge;
            const std::size_t aseg = atStart ? 0 : a->v.size() - 2;
            const auto iv = bandInterval(*b, on.seg, moved, aseg, ba);
            if (iv) {
                const std::size_t rank = side > 0 ? sb.offsets.size() - 1 : 0;
                (void)ctx.tx().modify(second.id, [&](Entity& e) {
                    entityAs<MlineData>(e)->cuts.push_back(cut(on.seg, *iv, rank, rank));
                });
            }
        }
        ctx.message(style.keyword + " tee made.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeMlEditCommand() {
    return std::make_unique<MlEditCommand>();
}

}  // namespace tbcad::mlinecmd
