#include "tbcad/training/check.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "tbcad/geom/entity.h"

namespace tbcad::training {

namespace {

constexpr const char* kKindNames[] = {
    "line",  "circle", "arc",       "polyline", "point", "ellipse", "spline", "text",  "mtext",
    "hatch", "insert", "dimension", "leader",   "proxy", "table",   "ray",    "mline", "mleader"};
static_assert(std::size(kKindNames) == kEntityKindCount);

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return out;
}

bool sameName(std::string_view a, std::string_view b) {
    return upper(a) == upper(b);
}

bool hasNonAscii(std::string_view s) {
    return std::any_of(s.begin(), s.end(),
                       [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
}

// The colour index an object shows; nullopt for ByBlock, RGB and the like.
std::optional<int> shownColor(const Entity& e, const Document& doc) {
    const Color& c = e.common.color;
    if (c.kind == ColorKind::Index)
        return c.indexValue();
    if (c.kind == ColorKind::ByLayer)
        if (const Layer* l = doc.layer(e.common.layer); l && l->color.kind == ColorKind::Index)
            return l->color.indexValue();
    return std::nullopt;
}

bool matchesFilters(const Check& c, const Entity& e, const Document& doc) {
    if (!c.kind.empty() && c.kind != kKindNames[static_cast<std::size_t>(kind(e))])
        return false;
    if (!c.layer.empty()) {
        const Layer* l = doc.layer(e.common.layer);
        if (!l || !sameName(l->name, c.layer))
            return false;
    }
    if (c.color && shownColor(e, doc) != c.color)
        return false;
    return true;
}

bool inBox(const BBox2& b, Vec2 p, double tol) {
    return p.x >= b.min.x - tol && p.y >= b.min.y - tol && p.x <= b.max.x + tol &&
           p.y <= b.max.y + tol;
}

template <class Fn>
void forEachModelEntity(const Document& doc, Fn&& fn) {
    for (EntityId id : doc.modelSpace())
        if (const Entity* e = doc.find(id))
            fn(*e);
}

struct Segment {
    Vec2 a, b;
};

// The straight pieces of the lines and polylines that pass the filters.
std::vector<Segment> segments(const Check& c, const Document& doc) {
    Check filters = c;
    filters.kind.clear();  // a "line" may be drawn as a polyline segment
    std::vector<Segment> out;
    forEachModelEntity(doc, [&](const Entity& e) {
        if (!matchesFilters(filters, e, doc))
            return;
        if (const auto* l = entityAs<LineData>(e)) {
            out.push_back({l->a, l->b});
        } else if (const auto* pl = entityAs<PolylineData>(e)) {
            const std::size_t n = pl->v.size();
            const std::size_t segs = pl->closed ? n : (n ? n - 1 : 0);
            for (std::size_t i = 0; i < segs; ++i)
                if (std::abs(pl->v[i].bulge) < 1e-9)
                    out.push_back({pl->v[i].p, pl->v[(i + 1) % n].p});
        }
    });
    return out;
}

double distanceToSegment(Vec2 p, const Segment& s) {
    const Vec2 d = s.b - s.a;
    const double len2 = d.x * d.x + d.y * d.y;
    if (len2 <= 0)
        return distance(p, s.a);
    const double t = std::clamp(((p.x - s.a.x) * d.x + (p.y - s.a.y) * d.y) / len2, 0.0, 1.0);
    return distance(p, Vec2{s.a.x + d.x * t, s.a.y + d.y * t});
}

// Difference of two directions in degrees, 0..180; `either` treats opposite directions as equal.
double angleDiff(double a, double b, bool either) {
    double d = std::fmod(std::abs(a - b), 360.0);
    if (d > 180)
        d = 360 - d;
    if (either && d > 90)
        d = 180 - d;
    return d;
}

double degrees(Vec2 from, Vec2 to) {
    return std::atan2(to.y - from.y, to.x - from.x) * 180.0 / kPi;
}

bool lineMatches(const Check& c, const Segment& s) {
    auto near = [&](Vec2 p, Vec2 q) { return distance(p, q) <= c.tol; };
    if (c.from && c.to)
        return (near(s.a, *c.from) && near(s.b, *c.to)) || (near(s.b, *c.from) && near(s.a, *c.to));
    Vec2 start = s.a, end = s.b;
    bool either = true;
    if (c.from) {
        if (near(s.b, *c.from))
            std::swap(start, end);
        else if (!near(s.a, *c.from))
            return false;
        either = false;
    }
    if (c.length && std::abs(distance(start, end) - *c.length) > c.tol)
        return false;
    if (c.angle && angleDiff(degrees(start, end), *c.angle, either) > c.angleTol)
        return false;
    return true;
}

bool countOk(const Check& c, int n, int defaultMin) {
    if (c.equals)
        return n == *c.equals;
    if (c.max && n > *c.max)
        return false;
    return n >= c.min.value_or(defaultMin);
}

bool polylineMatches(const Check& c, const Entity& e, const PolylineData& pl) {
    if (c.closed && pl.closed != *c.closed)
        return false;
    if (c.vertices && static_cast<int>(pl.v.size()) != *c.vertices)
        return false;
    if (c.arcs) {
        const std::size_t n = pl.v.size();
        const std::size_t segs = pl.closed ? n : (n ? n - 1 : 0);
        int arcs = 0;
        for (std::size_t i = 0; i < segs; ++i)
            arcs += std::abs(pl.v[i].bulge) >= 1e-9 ? 1 : 0;
        if (arcs != *c.arcs)
            return false;
    }
    if (c.box) {
        const BBox2 b = geom::bounds(e);
        if (distance(b.min, c.box->min) > c.tol || distance(b.max, c.box->max) > c.tol)
            return false;
    }
    if (c.area && std::abs(geom::area(pl) - *c.area) > std::abs(*c.area) * c.areaTolPct / 100.0)
        return false;
    return true;
}

int countEvents(const Facts& f, Event::Kind kind, std::initializer_list<const char*> commands) {
    int n = 0;
    for (const Event& e : f.events) {
        if (e.kind == kind)
            ++n;
        else if (e.kind == Event::Kind::Started)
            for (const char* name : commands)
                n += sameName(e.command, name) ? 1 : 0;
    }
    return n;
}

bool sourceMatches(const std::string& wanted, Source s) {
    if (wanted.empty())
        return true;
    switch (s) {
        case Source::Button:
            return wanted == "button";
        case Source::Name:
            return wanted == "name";
        case Source::Alias:
            return wanted == "alias";
        case Source::Thai:
            return wanted == "thai";
        case Source::Other:
            break;
    }
    return false;
}

}  // namespace

Source classifySource(std::string_view word, bool fromButton, std::string_view command) {
    if (fromButton)
        return Source::Button;
    if (word.empty())
        return Source::Other;
    if (hasNonAscii(word))
        return Source::Thai;
    return sameName(word, command) ? Source::Name : Source::Alias;
}

bool passes(const Check& c, const Facts& f) {
    switch (c.type) {
        case Check::Type::Command: {
            int n = 0;
            for (const Event& e : f.events)
                if (e.kind == Event::Kind::Started && sameName(e.command, c.command) &&
                    sourceMatches(c.source, e.source))
                    ++n;
            return countOk(c, n, 1);
        }
        case Check::Type::Undo:
            return countOk(c, countEvents(f, Event::Kind::Undo, {"UNDO", "U"}), 1);
        case Check::Type::Redo:
            return countOk(c, countEvents(f, Event::Kind::Redo, {"REDO", "MREDO"}), 1);
        case Check::Type::Prompt:
            if (f.runningCommand.empty() ||
                (!c.command.empty() && !sameName(f.runningCommand, c.command)))
                return false;
            return upper(f.prompt).find(upper(c.contains)) != std::string::npos;
        case Check::Type::View: {
            if (!f.view || f.view->empty())
                return false;
            const BBox2& v = *f.view;
            const BBox2& b = *c.box;
            if (b.min.x < v.min.x || b.min.y < v.min.y || b.max.x > v.max.x || b.max.y > v.max.y)
                return false;
            const double fw = (b.max.x - b.min.x) / (v.max.x - v.min.x);
            const double fh = (b.max.y - b.min.y) / (v.max.y - v.min.y);
            return std::max(fw, fh) >= c.minFill;
        }
        default:
            break;
    }

    if (!f.doc)
        return false;
    const Document& doc = *f.doc;
    switch (c.type) {
        case Check::Type::Line: {
            const auto segs = segments(c, doc);
            const int n = static_cast<int>(std::count_if(
                segs.begin(), segs.end(), [&](const Segment& s) { return lineMatches(c, s); }));
            return countOk(c, n, 1);
        }
        case Check::Type::Path: {
            const auto segs = segments(c, doc);
            const std::size_t n = c.path.size();
            const std::size_t legs = c.closed.value_or(false) ? n : n - 1;
            for (std::size_t i = 0; i < legs; ++i) {
                const Vec2 p = c.path[i], q = c.path[(i + 1) % n];
                const bool covered = std::any_of(segs.begin(), segs.end(), [&](const Segment& s) {
                    return distanceToSegment(p, s) <= c.tol && distanceToSegment(q, s) <= c.tol;
                });
                if (!covered)
                    return false;
            }
            return true;
        }
        case Check::Type::Circle: {
            int n = 0;
            forEachModelEntity(doc, [&](const Entity& e) {
                const auto* ci = entityAs<CircleData>(e);
                if (!ci || !matchesFilters(c, e, doc))
                    return;
                if (c.center && distance(ci->center, *c.center) > c.tol)
                    return;
                if (c.radius && std::abs(ci->radius - *c.radius) > c.tol)
                    return;
                ++n;
            });
            return countOk(c, n, 1);
        }
        case Check::Type::Polyline: {
            int n = 0;
            forEachModelEntity(doc, [&](const Entity& e) {
                const auto* pl = entityAs<PolylineData>(e);
                if (pl && matchesFilters(c, e, doc) && polylineMatches(c, e, *pl))
                    ++n;
            });
            return countOk(c, n, 1);
        }
        case Check::Type::Count: {
            int n = 0;
            forEachModelEntity(doc,
                               [&](const Entity& e) { n += matchesFilters(c, e, doc) ? 1 : 0; });
            return countOk(c, n, 1);
        }
        case Check::Type::Layer: {
            const Layer* l = doc.layerByName(c.name);
            if (!l)
                for (const auto& [id, layer] : doc.layers())
                    if (sameName(layer.name, c.name))
                        l = &layer;
            if (!l)
                return false;
            if (c.color &&
                !(l->color.kind == ColorKind::Index && l->color.indexValue() == *c.color))
                return false;
            if (c.on && l->on != *c.on)
                return false;
            if (c.frozen && l->frozen != *c.frozen)
                return false;
            if (c.locked && l->locked != *c.locked)
                return false;
            return true;
        }
        case Check::Type::Arc: {
            int n = 0;
            forEachModelEntity(doc, [&](const Entity& e) {
                const auto* a = entityAs<ArcData>(e);
                if (!a || !matchesFilters(c, e, doc))
                    return;
                if (c.center && distance(a->center, *c.center) > c.tol)
                    return;
                if (c.radius && std::abs(a->radius - *c.radius) > c.tol)
                    return;
                ++n;
            });
            return countOk(c, n, 1);
        }
        case Check::Type::Text: {
            int n = 0;
            forEachModelEntity(doc, [&](const Entity& e) {
                if (!matchesFilters(c, e, doc))
                    return;
                const std::string* text = nullptr;
                std::optional<double> height;
                Vec2 at{};
                std::vector<Vec2> tips;  // where a leader's arrows point
                if (const auto* t = entityAs<TextData>(e)) {
                    text = &t->utf8;
                    height = t->height;
                    at = t->position;
                } else if (const auto* m = entityAs<MTextData>(e)) {
                    text = &m->utf8;
                    height = m->height;
                    at = m->position;
                } else if (const auto* l = entityAs<LeaderData>(e)) {
                    text = &l->utf8;
                    at = l->textPos;
                    if (!l->vertices.empty())
                        tips.push_back(l->vertices.front());
                } else if (const auto* ml = entityAs<MLeaderData>(e)) {
                    text = &ml->utf8;
                    at = ml->contentPos;
                    for (const auto& line : ml->lines)
                        if (!line.empty())
                            tips.push_back(line.front());
                }
                if (!text || text->empty() || text->find(c.contains) == std::string::npos)
                    return;
                if (c.height && (!height || std::abs(*height - *c.height) > c.tol))
                    return;
                if (c.box && !inBox(*c.box, at, 0))
                    return;
                if (c.tip && std::none_of(tips.begin(), tips.end(),
                                          [&](Vec2 p) { return inBox(*c.tip, p, c.tol); }))
                    return;
                ++n;
            });
            return countOk(c, n, 1);
        }
        case Check::Type::Dimension: {
            int n = 0;
            forEachModelEntity(doc, [&](const Entity& e) {
                const auto* d = entityAs<DimensionData>(e);
                if (!d || !matchesFilters(c, e, doc))
                    return;
                const bool angular =
                    d->kind == DimKind::Angular2Line || d->kind == DimKind::Angular3Point;
                if (!c.dim.empty()) {
                    const char* k = angular                        ? "angular"
                                    : d->kind == DimKind::Linear   ? "linear"
                                    : d->kind == DimKind::Aligned  ? "aligned"
                                    : d->kind == DimKind::Radius   ? "radius"
                                    : d->kind == DimKind::Diameter ? "diameter"
                                                                   : "";
                    if (c.dim != k)
                        return;
                }
                if (c.value) {
                    const double v = angular ? d->measurement * 180.0 / kPi : d->measurement;
                    if (std::abs(v - *c.value) > (angular ? c.angleTol : c.tol))
                        return;
                }
                ++n;
            });
            return countOk(c, n, 1);
        }
        case Check::Type::Object: {
            int n = 0;
            forEachModelEntity(doc, [&](const Entity& e) {
                if (!matchesFilters(c, e, doc))
                    return;
                if (c.box) {
                    const BBox2 b = geom::bounds(e, doc);
                    if (distance(b.min, c.box->min) > c.tol || distance(b.max, c.box->max) > c.tol)
                        return;
                }
                ++n;
            });
            return countOk(c, n, 1);
        }
        case Check::Type::Selection: {
            for (EntityId id : f.selection) {
                const Entity* e = doc.find(id);
                if (!e || !matchesFilters(c, *e, doc))
                    return false;
            }
            return countOk(c, static_cast<int>(f.selection.size()), 1);
        }
        default:
            return false;
    }
}

}  // namespace tbcad::training
