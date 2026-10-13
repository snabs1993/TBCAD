#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "tbcad/hatch/hatch.h"

namespace tbcad::hatch {

namespace {

void pushVec(std::vector<double>& out, Vec2 v) {
    out.push_back(v.x);
    out.push_back(v.y);
}

// Flat numeric description of the loops; nullopt-like (returns false) for splines, which are
// always treated as changed.
bool signature(const std::vector<HatchLoop>& loops, std::vector<double>& out) {
    for (const HatchLoop& l : loops) {
        out.push_back(l.outer ? 1 : 0);
        out.push_back(static_cast<double>(l.edges.size()));
        for (const HatchEdge& e : l.edges) {
            out.push_back(static_cast<double>(e.index()));
            if (auto* x = std::get_if<HatchLineEdge>(&e)) {
                pushVec(out, x->a);
                pushVec(out, x->b);
            } else if (auto* a = std::get_if<HatchArcEdge>(&e)) {
                pushVec(out, a->center);
                out.insert(out.end(), {a->radius, a->startAngle, a->endAngle, a->ccw ? 1.0 : 0.0});
            } else if (auto* el = std::get_if<HatchEllipseEdge>(&e)) {
                pushVec(out, el->center);
                pushVec(out, el->majorAxis);
                out.insert(out.end(),
                           {el->ratio, el->startParam, el->endParam, el->ccw ? 1.0 : 0.0});
            } else if (auto* p = std::get_if<HatchPolylineEdge>(&e)) {
                out.push_back(p->closed ? 1 : 0);
                for (const PolyVertex& v : p->v) {
                    pushVec(out, v.p);
                    out.push_back(v.bulge);
                }
            } else {
                return false;
            }
        }
    }
    return true;
}

bool sameLoops(const std::vector<HatchLoop>& a, const std::vector<HatchLoop>& b) {
    std::vector<double> sa, sb;
    if (!signature(a, sa) || !signature(b, sb) || sa.size() != sb.size())
        return false;
    for (std::size_t i = 0; i < sa.size(); ++i)
        if (std::abs(sa[i] - sb[i]) > 1e-9)
            return false;
    return true;
}

double overlapArea(const BBox2& a, const BBox2& b) {
    double w = std::min(a.max.x, b.max.x) - std::max(a.min.x, b.min.x);
    double h = std::min(a.max.y, b.max.y) - std::max(a.min.y, b.min.y);
    return (w > 0 && h > 0) ? w * h : 0;
}

BBox2 loopBox(const HatchLoop& l) {
    HatchData d;
    d.loops.push_back(l);
    return hatchBounds(d);
}

}  // namespace

std::size_t updateAssociativeHatches(Document& doc, Transaction& tx,
                                     const BoundaryOptions& options) {
    std::vector<EntityId> touched = tx.touchedEntities();
    if (touched.empty())
        return 0;
    std::unordered_set<EntityId> touchedSet(touched.begin(), touched.end());

    struct Job {
        EntityId id;
        std::vector<EntityId> sources;
    };
    std::vector<Job> jobs;
    doc.forEachEntity([&](const Entity& e) {
        const HatchData* h = entityAs<HatchData>(e);
        if (!h || h->sourceBoundary.empty())
            return;
        for (EntityId s : h->sourceBoundary)
            if (touchedSet.count(s)) {
                jobs.push_back({e.common.id, h->sourceBoundary});
                return;
            }
    });

    std::size_t changed = 0;
    for (const Job& job : jobs) {
        const Entity* he = doc.find(job.id);
        const HatchData* h = he ? entityAs<HatchData>(*he) : nullptr;
        if (!h)
            continue;

        bool erased = false;
        std::vector<Candidate> cands;
        for (EntityId s : job.sources) {
            const Entity* se = doc.find(s);
            if (!se) {
                erased = true;
                break;
            }
            cands.push_back({s, se});
        }

        std::vector<HatchLoop> newLoops;
        std::vector<EntityId> newSources;
        bool found = false;
        if (!erased) {
            BoundaryFinder finder(std::move(cands), options);
            Result<std::vector<Boundary>> roots = finder.outermost();
            if (roots.ok() && !roots.value().empty()) {
                BBox2 old;
                if (!h->loops.empty())
                    old = loopBox(h->loops.front());
                const Boundary* best = nullptr;
                double bestScore = -1;
                for (const Boundary& b : roots.value()) {
                    double score = old.empty() ? 0 : overlapArea(old, loopBox(b.loops.front()));
                    if (score > bestScore) {
                        bestScore = score;
                        best = &b;
                    }
                }
                if (best) {
                    newLoops = best->loops;
                    newSources = best->sources;
                    found = true;
                }
            }
        }

        const bool same = found && newSources == h->sourceBoundary && sameLoops(newLoops, h->loops);
        if (same)
            continue;
        (void)tx.modify(job.id, [&](Entity& e) {
            HatchData* d = entityAs<HatchData>(e);
            if (!d)
                return;
            if (found) {
                d->loops = std::move(newLoops);
                d->sourceBoundary = std::move(newSources);
            } else {
                d->sourceBoundary.clear();
            }
        });
        ++changed;
    }
    return changed;
}

}  // namespace tbcad::hatch
