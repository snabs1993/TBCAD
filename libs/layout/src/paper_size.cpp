#include "tbcad/layout/paper_size.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "tbcad/dim/dim.h"
#include "tbcad/export/plot.h"
#include "tbcad/layout/layout.h"
#include "tbcad/layout/plotframes.h"
#include "tbcad/text/text.h"

namespace tbcad::layout {

namespace {

bool holds(const BBox2& b, Vec2 p) {
    return !b.empty() && p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y;
}

double area(const BBox2& b) {
    return (b.max.x - b.min.x) * (b.max.y - b.min.y);
}

// 1:N of a fit-to-paper frame: the window filling the paper in both directions.
double fitScale(const PlotFrame& f) {
    double w = f.customWidthMm, h = f.customHeightMm;
    if (const auto* ps = exportlib::findPaperSize(f.paperName)) {
        w = ps->shortMm;
        h = ps->longMm;
    }
    if (f.landscape)
        std::swap(w, h);
    if (!(w > 0) || !(h > 0) || f.window.empty())
        return 1;
    const double n =
        std::max((f.window.max.x - f.window.min.x) / w, (f.window.max.y - f.window.min.y) / h);
    return n > 0 ? n : 1;
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

double dimTextHeight(const DimStyle& st, double own) {
    const double k = st.overallScale > 0 ? st.overallScale : 1.0;
    return (own > 0 ? own : st.textHeight) * k;
}

double plainTextHeight(const Document& doc, TextStyleId style, double own) {
    if (own > 0)
        return own;
    const TextStyle* st = text::styleFor(doc, style);
    return st && st->height > 0 ? st->height : 2.5;
}

}  // namespace

PaperPlace paperPlaceAt(const Document& doc, Vec2 p) {
    // Plot frames: the smallest one holding the point.
    const PlotFrame* best = nullptr;
    const auto frames = plotFrames(doc);
    for (const auto& f : frames)
        if (holds(f.window, p) && (!best || area(f.window) < area(best->window)))
            best = &f;
    if (best) {
        PaperPlace pl;
        pl.kind = PaperPlace::Kind::Frame;
        pl.scaleN = best->fitToPaper ? fitScale(*best) : best->scaleN;
        pl.annoOverride = best->fitToPaper ? 0 : best->scaleN;
        pl.name = best->name;
        pl.paperName = best->paperName;
        return pl;
    }
    // Layout viewports: the first one showing the point.
    for (const auto& lay : layouts(doc)) {
        for (const auto& vp : lay.viewports) {
            if (!vp.on || !(vp.scaleN > 0) || vp.paper.empty())
                continue;
            const double hw = (vp.paper.max.x - vp.paper.min.x) / 2 * vp.scaleN;
            const double hh = (vp.paper.max.y - vp.paper.min.y) / 2 * vp.scaleN;
            BBox2 view;
            view.expand({vp.center.x - hw, vp.center.y - hh});
            view.expand({vp.center.x + hw, vp.center.y + hh});
            if (!holds(view, p))
                continue;
            PaperPlace pl;
            pl.kind = PaperPlace::Kind::Viewport;
            pl.scaleN = vp.scaleN;
            pl.annoOverride = vp.scaleN;
            pl.name = lay.name;
            pl.viewport = vp.id;
            pl.paperName = lay.paperName;
            return pl;
        }
    }
    // Annotation zones, then the drawing's annotation scale.
    const Header& h = doc.header();
    const AnnotationZone* zone = nullptr;
    const auto zones = annotationZones(h);
    for (const auto& z : zones)
        if (z.scale > 0 && holds(z.box, p) && (!zone || area(z.box) < area(zone->box)))
            zone = &z;
    PaperPlace pl;
    if (zone) {
        pl.kind = PaperPlace::Kind::Zone;
        pl.scaleN = zone->scale;
        pl.name = zone->name;
        return pl;
    }
    pl.kind = PaperPlace::Kind::AnnotationScale;
    pl.scaleN = currentAnnotationScale(h);
    return pl;
}

std::optional<PaperPlace> paperPlaceOf(const Document& doc, EntityId id) {
    const Entity* e = doc.find(id);
    if (!e)
        return std::nullopt;
    if (const BlockId owner = doc.ownerBlock(id); owner.value != 0) {
        for (const auto& lay : layouts(doc)) {
            if (lay.content != owner)
                continue;
            PaperPlace pl;
            pl.kind = PaperPlace::Kind::Sheet;
            pl.scaleN = 1;
            pl.annoOverride = 1;
            pl.name = lay.name;
            pl.paperName = lay.paperName;
            return pl;
        }
        return std::nullopt;
    }
    const auto anchor = annotationAnchor(*e);
    if (!anchor)
        return std::nullopt;
    return paperPlaceAt(doc, *anchor);
}

std::optional<double> drawnTextHeight(const Document& doc, const Entity& e,
                                      const PaperPlace& place) {
    std::optional<double> h;
    if (const auto* t = entityAs<TextData>(e))
        h = plainTextHeight(doc, t->style, t->height);
    else if (const auto* m = entityAs<MTextData>(e))
        h = plainTextHeight(doc, m->style, m->height);
    else if (const auto* d = entityAs<DimensionData>(e))
        h = dimTextHeight(dim::drawStyleOf(doc, *d), 0);
    else if (const auto* l = entityAs<LeaderData>(e))
        h = dimTextHeight(dim::leaderStyleOf(doc, l->style), 0);
    else if (const auto* ml = entityAs<MLeaderData>(e)) {
        if (ml->content != MLeaderData::Content::Text)
            return std::nullopt;
        const DimStyle* st =
            doc.dimStyle(ml->style.value != 0 ? ml->style : doc.header().currentDimStyle);
        h = st ? dimTextHeight(*st, ml->textHeight) : ml->textHeight;
    }
    if (!h)
        return std::nullopt;
    if (place.kind == PaperPlace::Kind::Sheet)
        return *h;
    return *h * annotationFactor(doc.header(), e, place.annoOverride);
}

std::optional<double> paperTextHeightMm(const Document& doc, const Entity& e,
                                        const PaperPlace& place) {
    const auto h = drawnTextHeight(doc, e, place);
    if (!h || !(place.scaleN > 0))
        return std::nullopt;
    return *h / place.scaleN;
}

std::string describePaperPlace(const PaperPlace& pl) {
    const std::string paper = pl.paperName.empty() ? "" : " (" + pl.paperName + ")";
    const std::string scale = "1:" + num(pl.scaleN);
    switch (pl.kind) {
        case PaperPlace::Kind::Frame:
            return scale + ", frame " + pl.name + paper;
        case PaperPlace::Kind::Viewport:
            return scale + ", " + pl.name + " viewport " + std::to_string(pl.viewport) + paper;
        case PaperPlace::Kind::Zone:
            return scale + ", zone " + pl.name;
        case PaperPlace::Kind::AnnotationScale:
            return scale + ", annotation scale";
        case PaperPlace::Kind::Sheet:
            return "sheet " + pl.name + paper;
    }
    return scale;
}

PaperSizeSummary paperSizeSummary(const Document& doc, const std::vector<EntityId>& ids) {
    PaperSizeSummary s;
    bool samePlace = true;
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        const auto pl = paperPlaceOf(doc, id);
        if (!pl)
            continue;
        const auto mm = paperTextHeightMm(doc, *e, *pl);
        if (!mm)
            continue;
        if (s.count == 0) {
            s.minMm = s.maxMm = *mm;
            s.place = *pl;
        } else {
            s.minMm = std::min(s.minMm, *mm);
            s.maxMm = std::max(s.maxMm, *mm);
            if (samePlace && s.place &&
                (s.place->kind != pl->kind || s.place->name != pl->name ||
                 s.place->viewport != pl->viewport || s.place->scaleN != pl->scaleN))
                samePlace = false;
        }
        ++s.count;
    }
    if (!samePlace)
        s.place.reset();
    return s;
}

std::string formatPaperMm(double minMm, double maxMm) {
    auto f = [](double v) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.2f", v);
        std::string r = buf;
        while (!r.empty() && r.back() == '0')
            r.pop_back();
        if (!r.empty() && r.back() == '.')
            r.pop_back();
        return r;
    };
    const std::string a = f(minMm), b = f(maxMm);
    return a == b ? a + " mm" : a + " - " + b + " mm";
}

}  // namespace tbcad::layout

namespace tbcad::layout {

namespace {

bool near(double a, double b, double tol) {
    return std::abs(a - b) <= tol;
}

bool isStandard(double mm, const PaperCheckOptions& o) {
    for (double s : o.standardMm)
        if (near(mm, s, o.toleranceMm))
            return true;
    return false;
}

double nearestStandard(double mm, const PaperCheckOptions& o) {
    double best = 0;
    for (double s : o.standardMm) {
        if (s < o.minMm - 1e-9 || s > o.maxMm + 1e-9)
            continue;
        if (best == 0 || std::abs(std::log(s / mm)) < std::abs(std::log(best / mm)))
            best = s;
    }
    return best > 0 ? best : std::clamp(mm, o.minMm, o.maxMm);
}

bool nearRel(double a, double b, double rel) {
    return std::abs(a - b) <= rel * b;
}

// The scales used in the drawing: frames, viewports, zones and the annotation scale.
std::vector<double> drawingScales(const Document& doc) {
    std::vector<double> out;
    auto add = [&](double n) {
        if (n > 0 &&
            std::none_of(out.begin(), out.end(), [&](double m) { return near(m, n, 1e-9); }))
            out.push_back(n);
    };
    for (const auto& f : plotFrames(doc))
        if (!f.fitToPaper)
            add(f.scaleN);
    for (const auto& lay : layouts(doc))
        for (const auto& vp : lay.viewports)
            add(vp.scaleN);
    for (const auto& z : annotationZones(doc.header()))
        add(z.scale);
    add(currentAnnotationScale(doc.header()));
    return out;
}

}  // namespace

const char* paperIssueLabel(PaperIssue issue) {
    switch (issue) {
        case PaperIssue::Ok:
            return "OK";
        case PaperIssue::OtherScale:
            return "Made for another scale";
        case PaperIssue::TooSmall:
            return "Too small";
        case PaperIssue::TooBig:
            return "Too big";
        case PaperIssue::OffStandard:
            return "Not a standard height";
    }
    return "";
}

std::vector<EntityId> paperObjects(const Document& doc) {
    std::vector<EntityId> out = doc.modelSpace();
    for (const auto& lay : layouts(doc))
        if (const BlockDef* b = doc.block(lay.content))
            out.insert(out.end(), b->entities.begin(), b->entities.end());
    return out;
}

std::vector<PaperCheckItem> paperCheck(const Document& doc, const std::vector<EntityId>& ids,
                                       const PaperCheckOptions& o) {
    std::vector<PaperCheckItem> out;
    const std::vector<double> used = drawingScales(doc);
    for (EntityId id : ids.empty() ? paperObjects(doc) : ids) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        const auto pl = paperPlaceOf(doc, id);
        if (!pl)
            continue;
        const auto drawn = drawnTextHeight(doc, *e, *pl);
        if (!drawn || !(*drawn > 0) || !(pl->scaleN > 0))
            continue;
        PaperCheckItem it;
        it.id = id;
        it.place = *pl;
        it.mm = *drawn / pl->scaleN;
        if (isStandard(it.mm, o)) {
            it.issue = PaperIssue::Ok;
            it.suggestMm = it.mm;
            out.push_back(it);
            continue;
        }
        // Left from a scale change: an ordinary model object that reads at a standard height
        // (within 2%) at half or double this scale, at another scale used in the drawing, or -
        // a dimension or leader - at its style's overall scale. The closest such scale wins.
        if (pl->kind != PaperPlace::Kind::Sheet && !e->common.annotative) {
            std::vector<double> cand = used;
            cand.push_back(pl->scaleN / 2);
            cand.push_back(pl->scaleN * 2);
            const DimStyle* st = nullptr;
            if (const auto* d = entityAs<DimensionData>(*e))
                st = &dim::drawStyleOf(doc, *d);
            else if (const auto* l = entityAs<LeaderData>(*e))
                st = &dim::leaderStyleOf(doc, l->style);
            if (st && st->overallScale > 0)
                cand.push_back(st->overallScale);
            for (double n : cand) {
                if (near(n, pl->scaleN, 1e-9))
                    continue;
                const double at = *drawn / n;
                if (std::none_of(o.standardMm.begin(), o.standardMm.end(),
                                 [&](double s) { return nearRel(at, s, 0.02); }))
                    continue;
                if (it.madeForN == 0 || std::abs(std::log(n / pl->scaleN)) <
                                            std::abs(std::log(it.madeForN / pl->scaleN)))
                    it.madeForN = n;
            }
        }
        if (it.madeForN > 0) {
            it.issue = PaperIssue::OtherScale;
            it.suggestMm = nearestStandard(*drawn / it.madeForN, o);
        } else {
            it.issue = it.mm < o.minMm   ? PaperIssue::TooSmall
                       : it.mm > o.maxMm ? PaperIssue::TooBig
                                         : PaperIssue::OffStandard;
            it.suggestMm = nearestStandard(it.mm, o);
        }
        out.push_back(it);
    }
    return out;
}

Result<std::size_t> fitToPaper(Transaction& tx, const std::vector<PaperCheckItem>& items) {
    std::size_t changed = 0;
    for (const auto& it : items) {
        if (it.issue == PaperIssue::Ok || !(it.suggestMm > 0))
            continue;
        const Document& doc = tx.document();
        const Entity* e = doc.find(it.id);
        if (!e)
            continue;
        const double factor = it.place.kind == PaperPlace::Kind::Sheet
                                  ? 1.0
                                  : annotationFactor(doc.header(), *e, it.place.annoOverride);
        const double wantDrawn = it.suggestMm * it.place.scaleN;  // drawing units
        const double wantOwn = wantDrawn / factor;                // before the annotation factor
        // Dimensions and leaders: an override style whose overall scale gives the height.
        auto styled = [&](DimStyleId sid) -> Result<DimStyleId> {
            const DimStyle* st = doc.dimStyle(sid.value != 0 ? sid : doc.header().currentDimStyle);
            if (!st || !(st->textHeight > 0))
                return Error{ErrorCode::InvalidArgument, "no dimension style"};
            const double k = wantOwn / st->textHeight;
            DimStyleId ns =
                dim::overrideStyle(tx, st->id, [k](DimStyle& s) { s.overallScale = k; });
            if (ns.value == 0)
                return Error{ErrorCode::InvalidArgument, "cannot add a dimension style"};
            return ns;
        };
        Result<void> r{};
        if (const auto* d = entityAs<DimensionData>(*e)) {
            auto ns = styled(d->style);
            if (!ns)
                return ns.error();
            const DimStyleId sid = ns.value();
            r = tx.modify(it.id, [&](Entity& x) {
                auto* dd = entityAs<DimensionData>(x);
                dd->style = sid;
                dim::refresh(tx.document(), *dd);
            });
        } else if (const auto* l = entityAs<LeaderData>(*e)) {
            auto ns = styled(l->style);
            if (!ns)
                return ns.error();
            const DimStyleId sid = ns.value();
            r = tx.modify(it.id, [&](Entity& x) { entityAs<LeaderData>(x)->style = sid; });
        } else if (const auto* ml = entityAs<MLeaderData>(*e)) {
            const DimStyle* st =
                doc.dimStyle(ml->style.value != 0 ? ml->style : doc.header().currentDimStyle);
            const double k = st && st->overallScale > 0 ? st->overallScale : 1.0;
            r = tx.modify(it.id,
                          [&](Entity& x) { entityAs<MLeaderData>(x)->textHeight = wantOwn / k; });
        } else if (entityAs<TextData>(*e)) {
            r = tx.modify(it.id, [&](Entity& x) { entityAs<TextData>(x)->height = wantOwn; });
        } else if (entityAs<MTextData>(*e)) {
            r = tx.modify(it.id, [&](Entity& x) { entityAs<MTextData>(x)->height = wantOwn; });
        } else {
            continue;
        }
        if (!r)
            return r.error();
        ++changed;
    }
    return changed;
}

}  // namespace tbcad::layout
