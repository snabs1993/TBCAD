// DXF import diagnostics: how a dimension TBCAD draws differs from the anonymous *D block the
// program that wrote the file stored for it (ImportOptions::dimensionCompare). The block is what
// AutoCAD showed, so every difference is something the generator, or the reading of the
// dimension's groups, does not do the same way.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "importer.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dxf/encoding.h"
#include "tbcad/geom/geom.h"
#include "tbcad/hatch/hatch.h"

namespace tbcad::dxf::detail {

namespace {

std::optional<std::uint64_t> handleOf(const Record& r) {
    if (const Group* g = r.find(5)) {
        if (const auto* h = std::get_if<Handle>(&g->value)) {
            if (h->valid)
                return h->value;
        }
    }
    return std::nullopt;
}

struct Segment {
    Vec2 a{}, b{};
};

double pointSegment(Vec2 p, const Segment& s) {
    const Vec2 d = s.b - s.a;
    const double len2 = dot(d, d);
    const double t = len2 > 0 ? std::clamp(dot(p - s.a, d) / len2, 0.0, 1.0) : 0.0;
    return distance(p, s.a + d * t);
}

void addPath(const std::vector<Vec2>& pts, bool closed, std::vector<Segment>& out) {
    for (std::size_t i = 0; i + 1 < pts.size(); ++i)
        out.push_back({pts[i], pts[i + 1]});
    if (closed && pts.size() > 2)
        out.push_back({pts.back(), pts.front()});
}

// Share of the length of `from` that lies further than `tol` from every segment of `to` and
// outside every box of `covered`.
double strayShare(const std::vector<Segment>& from, const std::vector<Segment>& to, double tol,
                  const std::vector<BBox2>& covered = {}) {
    double total = 0, stray = 0;
    for (const Segment& s : from) {
        const double len = distance(s.a, s.b);
        if (!(len > 0))
            continue;
        const int n = std::clamp(static_cast<int>(len / tol), 2, 50);
        for (int i = 0; i <= n; ++i) {
            const Vec2 p = s.a + (s.b - s.a) * (static_cast<double>(i) / n);
            double best = 1e300;
            for (const Segment& t : to) {
                best = std::min(best, pointSegment(p, t));
                if (best <= tol)
                    break;
            }
            total += len / (n + 1);
            for (const BBox2& b : covered)
                if (best > tol && b.inflated(tol).contains(p))
                    best = 0;
            if (best > tol)
                stray += len / (n + 1);
        }
    }
    return total > 0 ? stray / total : 0;
}

Vec2 centroid(const std::vector<Vec2>& ring) {
    Vec2 c{};
    for (const Vec2& p : ring)
        c = c + p;
    return ring.empty() ? c : c * (1.0 / static_cast<double>(ring.size()));
}

const char* kindName(DimKind k) {
    switch (k) {
        case DimKind::Linear:
            return "linear";
        case DimKind::Aligned:
            return "aligned";
        case DimKind::Angular2Line:
            return "angular";
        case DimKind::Angular3Point:
            return "angular3p";
        case DimKind::Radius:
            return "radius";
        case DimKind::Diameter:
            return "diameter";
        case DimKind::Ordinate:
            return "ordinate";
        case DimKind::ArcLength:
            return "arclength";
        case DimKind::Jogged:
            return "jogged";
    }
    return "other";
}

void replaceAll(std::string& s, std::string_view from, std::string_view to) {
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

// The text without formatting codes or spaces at the ends, for comparing what is written. The
// diameter sign has several spellings that all show the same.
std::string plain(const std::string& s) {
    std::string t = stripMText(s);
    for (std::string_view d : {"%%c", "%%C", "\xE2\x8C\x80", "\xE2\x88\x85"})
        replaceAll(t, d, "\xC3\x98");
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n'))
        t.pop_back();
    std::size_t i = 0;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\n'))
        ++i;
    return t.substr(i);
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.8g", v);
    return buf;
}

std::string pt(Vec2 p) {
    return "(" + num(p.x) + "," + num(p.y) + ")";
}

// The DSTYLE variables of the ACAD extended data as "var=value" pairs.
std::string dstyleGroups(const Record& r) {
    std::string o;
    bool inAcad = false, inDstyle = false;
    int var = -1;
    for (const Group& g : r.g) {
        if (g.code == 1001) {
            inAcad = g.str() == "ACAD";
            inDstyle = false;
            continue;
        }
        if (!inAcad)
            continue;
        if (g.code == 1000) {
            inDstyle = g.str() == "DSTYLE";
            continue;
        }
        if (!inDstyle)
            continue;
        if (g.code == 1070 && var < 0) {
            var = static_cast<int>(g.integer());
            continue;
        }
        if (var >= 0) {
            o += (o.empty() ? "" : " ") + std::to_string(var) + "=";
            o += g.code == 1040                     ? num(g.number())
                 : g.code == 1070 || g.code == 1071 ? std::to_string(g.integer())
                                                    : std::string(g.str());
            var = -1;
        }
    }
    return o;
}

}  // namespace

void Importer::compareDimension(const Record& r, const Entity& dim) {
    const auto* d = entityAs<DimensionData>(dim);
    const BlockDef* def = d ? dimensionBlock(r) : nullptr;
    if (!def)
        return;
    std::vector<Entity> shapes, texts;
    dimensionBlockEntities(*def, shapes, texts);
    // The block is drawn at the dimension's group 12 point (zero unless the dimension was
    // copied or moved without its block being made again).
    const Vec2 shift = r.has(12) ? P(r, 12, 22) : Vec2{};

    const double factor = annotationFactor(doc_->header(), dim.common, 0);
    const dim::Layout L = dim::layoutDimension(*doc_, *d, 0.01, factor);
    const DimStyle& style = dim::styleOf(*doc_, d->style);

    DimComparison c;
    c.handle = handleOf(r).value_or(0);
    c.line = r.line;
    c.kind = kindName(d->kind);

    // Text: the stored one is the first text of the block (AutoCAD writes one MTEXT).
    // Its centre: AutoCAD writes it middle-centre aligned, so the insertion point is the centre;
    // other alignments go by the drawn box when the text has one here.
    double storedHeight = 0;
    std::optional<Vec2> storedCenter;
    std::string storedRaw;
    std::string storedSize;  // the writer's measured size of the text, and TBCAD's of the same
    if (!texts.empty()) {
        const Entity& t = texts.front();
        if (const auto* m = entityAs<MTextData>(t)) {
            c.storedText = plain(m->utf8);
            storedRaw = m->utf8;
            // A relative height that starts the text sizes all of it.
            storedHeight = m->height * leadingHeightFactor(m->utf8);
            if (m->attach == Attachment::MiddleCenter)
                storedCenter = m->position;
            storedSize = " attach=" + std::to_string(static_cast<int>(m->attach));
            if (auto it = mtextExtents_.find({m->position.x, m->position.y});
                it != mtextExtents_.end())
                storedSize += " storedExtent=" + num(it->second.x) + "x" + num(it->second.y);
            if (const BBox2 b = geom::bounds(t, *doc_); !b.empty())
                storedSize += " ourExtent=" + num(b.width()) + "x" + num(b.height());
        } else if (const auto* x = entityAs<TextData>(t)) {
            c.storedText = plain(x->utf8);
            storedRaw = x->utf8;
            storedHeight = x->height;
        }
        if (!storedCenter) {
            const BBox2 b = geom::bounds(t, *doc_);
            if (!b.empty())
                storedCenter = (b.min + b.max) * 0.5;
        }
        if (storedCenter)
            *storedCenter = *storedCenter + shift;
    }
    c.generatedText = plain(L.text.utf8);
    c.textMatch = c.storedText == c.generatedText;
    // A block made for another measurement: no text of its own and both numbers, but different.
    // AutoCAD makes such a block again whenever the dimension is edited, so TBCAD's is the one
    // to trust; it is counted apart.
    {
        auto isNumber = [](const std::string& t) {
            char* end = nullptr;
            return !t.empty() && (std::strtod(t.c_str(), &end), end && *end == 0);
        };
        c.stale = d->textOverride.empty() && !c.textMatch && isNumber(c.storedText) &&
                  isNumber(c.generatedText);
    }
    const double unit = storedHeight > 0    ? storedHeight
                        : L.text.height > 0 ? L.text.height
                                            : std::max(style.arrowSize * style.overallScale, 1e-6);
    if (storedHeight > 0 && L.text.height > 0)
        c.heightRatio = L.text.height / storedHeight;
    if (storedCenter && L.text.valid())
        c.textOffset = distance(*storedCenter, L.text.center) / unit;

    // Lines: everything in the block but text, points and filled arrowheads, against the
    // generator's lines, arcs and arrowhead outlines.
    std::vector<Segment> stored, generated, generatedLines;
    std::vector<Vec2> storedArrows;
    std::vector<geom::CurveRef> curves;
    for (const Entity& s : shapes) {
        if (kind(s) == EntityKind::Point)
            continue;
        if (const auto* h = entityAs<HatchData>(s)) {
            for (const HatchLoop& loop : h->loops) {
                HatchData single;
                single.loops = {loop};
                for (const std::vector<Vec2>& ring : hatch::loopRings(single, 0.01 * unit))
                    if (!ring.empty())
                        storedArrows.push_back(centroid(ring) + shift);
            }
            continue;
        }
        curves.clear();
        if (!geom::curvesOf(s, curves))
            continue;
        for (const geom::CurveRef& cv : curves) {
            std::vector<Vec2> pts;
            geom::flatten(cv, 0.01 * unit, pts);
            for (Vec2& q : pts)
                q = q + shift;
            addPath(pts, false, stored);
        }
    }
    for (const dim::Layout::Line& l : L.lines)
        addPath(l.pts, false, generatedLines);
    for (const dim::Layout::Arc& a : L.arcs)
        addPath(dim::arcPoints(a, 0.01 * unit), false, generatedLines);
    generated = generatedLines;
    for (const dim::Layout::Arrow& a : L.arrows)
        addPath(a.ring, true, generated);
    const double tol = 0.1 * unit;
    c.strayStored = strayShare(stored, generated, tol);
    // AutoCAD's block leaves out the part of the dimension line an arrowhead covers; TBCAD draws
    // the line to the tip. That part is not a difference.
    std::vector<BBox2> arrowBoxes;
    for (const dim::Layout::Arrow& a : L.arrows) {
        BBox2 b;
        for (const Vec2& q : a.ring)
            b.expand(q);
        if (!b.empty())
            arrowBoxes.push_back(b);
    }
    c.strayGenerated = strayShare(generatedLines, stored, tol, arrowBoxes);

    // Arrowheads: each stored one should have one of the generator's within an arrow size.
    c.arrowsStored = static_cast<int>(storedArrows.size());
    c.arrowsGenerated = static_cast<int>(L.arrows.size());
    const double reach = std::max(style.arrowSize * style.overallScale * factor, tol);
    for (const Vec2& p : storedArrows) {
        bool found = false;
        for (const dim::Layout::Arrow& a : L.arrows)
            found = found || (!a.ring.empty() && distance(p, centroid(a.ring)) <= reach);
        if (!found)
            ++c.arrowsUnmatched;
    }

    BBox2 storedBox, generatedBox;
    for (const Segment& s : stored) {
        storedBox.expand(s.a);
        storedBox.expand(s.b);
    }
    for (const Segment& s : generated) {
        generatedBox.expand(s.a);
        generatedBox.expand(s.b);
    }
    // Points are relative to the first definition point (`at`), so far from the origin they keep
    // their digits.
    const Vec2 o = d->defPts[0];
    const auto rel = [&](Vec2 p) { return pt(p - o); };
    const auto relBox = [&](const BBox2& b) {
        return b.empty() ? std::string("none") : rel(b.min) + "-" + rel(b.max);
    };
    std::string font = "\"\"";
    if (const TextStyle* ts = doc_->textStyle(style.textStyle))
        font = "\"" + ts->name + "\" font=\"" + (ts->shxFile.empty() ? ts->fontFile : ts->shxFile) +
               "\" family=\"" + ts->fontFamily + "\" wf=" + num(ts->widthFactor) +
               " fixedH=" + num(ts->height);
    c.style = style.name;
    c.detail =
        "override=\"" + dec(r.raw(1)) + "\" storedRaw=\"" + storedRaw + "\" style=\"" + style.name +
        "\" txt=" + num(style.textHeight) + " scale=" + num(style.overallScale) +
        " annotation=" + num(factor) + " lfac=" + num(style.linearFactor) + " prefix=\"" +
        style.prefix + "\" suffix=\"" + style.suffix + "\" units=" + num(k_) +
        " asz=" + num(style.arrowSize) + " gap=" + num(style.textGap) +
        " fit=" + std::to_string(static_cast<int>(style.fit)) +
        " tad=" + std::to_string(static_cast<int>(style.textPlacement)) +
        " tvp=" + num(style.textVertPos) +
        " tih=" + std::to_string(static_cast<int>(style.textAlign)) +
        " toh=" + std::to_string(static_cast<int>(style.textAlignOutside)) +
        " tmove=" + std::to_string(static_cast<int>(style.textMove)) +
        " tix=" + std::to_string(style.textInsideForced) +
        " tofl=" + std::to_string(style.forceLineInside) +
        " soxd=" + std::to_string(style.suppressArrowsOutside) +
        " just=" + std::to_string(style.textJustify) + " flags=" + std::to_string(r.integer(70)) +
        " user=" + std::to_string(d->textPosUser) + " textStyle=" + font +
        " textWidth=" + num(L.text.width) + " storedHeight=" + num(storedHeight) +
        " generatedHeight=" + num(L.text.height) + storedSize +
        " measured=" + num(dim::measurement(*d)) + " at=" + pt(o) + " defPts=" + rel(d->defPts[0]) +
        rel(d->defPts[1]) + rel(d->defPts[2]) + rel(d->defPts[3]) + " textPos=" + rel(d->textPos) +
        " storedText@" + (storedCenter ? rel(*storedCenter) : std::string("none")) +
        " generatedText@" + rel(L.text.center) + " storedBox=" + relBox(storedBox) +
        " generatedBox=" + relBox(generatedBox) + " dstyle={" + dstyleGroups(r) + "}";
    if (r.has(12))
        c.detail += " p12=" + rel(P(r, 12, 22));
    // The lines themselves (the longest first, at most twelve of each), so a difference can be
    // located without the file.
    auto segments = [&](std::vector<Segment> v) {
        std::sort(v.begin(), v.end(), [](const Segment& a, const Segment& b) {
            return distance(a.a, a.b) > distance(b.a, b.b);
        });
        std::string list = std::to_string(v.size()) + ":";
        for (std::size_t i = 0; i < v.size() && i < 12; ++i)
            list += " " + rel(v[i].a) + rel(v[i].b);
        return list;
    };
    c.detail +=
        " storedLines=[" + segments(stored) + "] generatedLines=[" + segments(generatedLines) + "]";
    sum_.dimCompare.push_back(std::move(c));
}

}  // namespace tbcad::dxf::detail
