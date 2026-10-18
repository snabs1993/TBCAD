#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <utility>

#include "tbcad/constraints/constraints.h"

namespace tbcad::constraints {

namespace {

constexpr std::string_view kNames[] = {"Coincident",    "Horizontal", "Vertical", "Parallel",
                                       "Perpendicular", "Equal",      "Tangent",  "Fix",
                                       "Distance",      "Radius",     "Angle",    "Concentric",
                                       "PointOnObject", "Midpoint",   "Collinear"};

enum class Type { None, Line, Circle, Arc, Point, Polyline, Ellipse, Insert };

Type typeOf(const Document& doc, EntityId id) {
    const Entity* e = doc.find(id);
    if (!e)
        return Type::None;
    switch (kind(*e)) {
        case EntityKind::Line:
            return Type::Line;
        case EntityKind::Circle:
            return Type::Circle;
        case EntityKind::Arc:
            return Type::Arc;
        case EntityKind::Point:
            return Type::Point;
        case EntityKind::Polyline:
            return Type::Polyline;
        case EntityKind::Ellipse:
            return Type::Ellipse;
        case EntityKind::Insert:
            return Type::Insert;
        default:
            return Type::None;
    }
}

const char* typeName(Type t) {
    switch (t) {
        case Type::Line:
            return "line";
        case Type::Circle:
            return "circle";
        case Type::Arc:
            return "arc";
        case Type::Point:
            return "point";
        case Type::Polyline:
            return "polyline";
        case Type::Ellipse:
            return "ellipse";
        case Type::Insert:
            return "insert";
        default:
            return "object";
    }
}

// Does `part` of an entity of type `t` name a point?
bool isPointRef(Type t, Part p) {
    switch (t) {
        case Type::Point:
            return p == Part::Whole || p == Part::Start;
        case Type::Line:
            return p == Part::Start || p == Part::End;
        case Type::Circle:
            return p == Part::Center;
        case Type::Arc:
            return p == Part::Start || p == Part::End || p == Part::Center;
        case Type::Polyline:
            return p == Part::Start;
        case Type::Ellipse:
            return p == Part::Center;
        case Type::Insert:
            return p == Part::Whole || p == Part::Start;
        default:
            return false;
    }
}

// Polyline geometry the references are checked against.
const PolylineData* polylineOf(const Document& doc, EntityId id) {
    const Entity* e = doc.find(id);
    return e ? entityAs<PolylineData>(*e) : nullptr;
}

// A point of the entity: the part names a point of its type and the index a vertex that exists.
bool pointOk(const Document& doc, const Ref& r) {
    const Type t = typeOf(doc, r.entity);
    if (!isPointRef(t, r.part))
        return false;
    if (t == Type::Polyline) {
        const PolylineData* pl = polylineOf(doc, r.entity);
        return pl && r.index < pl->v.size();
    }
    return r.index == 0;
}

// A straight segment: a line, or a polyline segment without a bulge.
bool segOk(const Document& doc, const Ref& r) {
    const Type t = typeOf(doc, r.entity);
    if (t == Type::Line)
        return r.part == Part::Whole && r.index == 0;
    if (t != Type::Polyline || r.part != Part::Whole)
        return false;
    const PolylineData* pl = polylineOf(doc, r.entity);
    if (!pl || pl->v.size() < 2)
        return false;
    const std::size_t n = pl->v.size();
    return r.index < (pl->closed ? n : n - 1) && pl->v[r.index].bulge == 0;
}

// A direction: a straight segment, or the x axis of an insert.
bool axisOk(const Document& doc, const Ref& r) {
    return segOk(doc, r) ||
           (typeOf(doc, r.entity) == Type::Insert && r.part == Part::Whole && r.index == 0);
}

Error bad(std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message)};
}

std::string refText(const Document& doc, const Ref& r) {
    std::string s = typeName(typeOf(doc, r.entity));
    s += " " + std::to_string(r.entity.value);
    if (typeOf(doc, r.entity) == Type::Polyline) {
        s += (r.part == Part::Whole ? " segment " : " vertex ") + std::to_string(r.index + 1);
        return s;
    }
    switch (r.part) {
        case Part::Start:
            s += " start";
            break;
        case Part::End:
            s += " end";
            break;
        case Part::Center:
            s += " center";
            break;
        default:
            break;
    }
    return s;
}

}  // namespace

std::string_view kindName(Kind k) {
    return kNames[static_cast<int>(k)];
}

bool isDimensional(Kind k) {
    return k == Kind::Distance || k == Kind::Radius || k == Kind::Angle;
}

std::string describe(const Document& doc, const Constraint& c) {
    std::string s =
        "#" + std::to_string(c.id) + " " + std::string(kindName(c.kind)) + " " + refText(doc, c.a);
    if (c.b.entity)
        s += " / " + refText(doc, c.b);
    char buf[48];
    if (c.kind == Kind::Distance || c.kind == Kind::Radius) {
        std::snprintf(buf, sizeof buf, " = %.6g", c.value);
        s += buf;
    } else if (c.kind == Kind::Angle) {
        std::snprintf(buf, sizeof buf, " = %.6g deg", c.value * 180.0 / kPi);
        s += buf;
    }
    if (!c.expr.empty())
        s += "  [" + c.expr + "]";
    return s;
}

Result<void> validate(const Document& doc, const Constraint& c) {
    const Type ta = typeOf(doc, c.a.entity);
    const Type tb = c.b.entity ? typeOf(doc, c.b.entity) : Type::None;
    if (ta == Type::None)
        return bad(
            "the first object cannot be constrained (lines, circles, arcs, points, "
            "polylines, ellipses and inserts can)");
    const bool needsB = c.kind != Kind::Fix && c.kind != Kind::Radius &&
                        !(c.kind == Kind::Horizontal || c.kind == Kind::Vertical);
    if (needsB && tb == Type::None)
        return bad(std::string(kindName(c.kind)) + " needs a second object");
    const bool circA = ta == Type::Circle || ta == Type::Arc;
    const bool circB = tb == Type::Circle || tb == Type::Arc;
    switch (c.kind) {
        case Kind::Coincident:
            if (!pointOk(doc, c.a) || !pointOk(doc, c.b))
                return bad(
                    "Coincident joins two points (line ends, polyline vertices, arc ends "
                    "or centres, ellipse centres, insert points, points)");
            if (c.a == c.b)
                return bad("Coincident needs two different points");
            break;
        case Kind::Horizontal:
        case Kind::Vertical:
            if (!c.b.entity && axisOk(doc, c.a))
                break;
            if (c.b.entity && pointOk(doc, c.a) && pointOk(doc, c.b) && !(c.a == c.b))
                break;
            return bad(std::string(kindName(c.kind)) +
                       " takes one line, polyline segment or insert, or two points");
        case Kind::Parallel:
        case Kind::Perpendicular:
            if (!axisOk(doc, c.a) || !axisOk(doc, c.b) || c.a == c.b)
                return bad(std::string(kindName(c.kind)) +
                           " takes two different lines, polyline segments or inserts");
            break;
        case Kind::Equal:
            if (segOk(doc, c.a) && segOk(doc, c.b) && !(c.a == c.b))
                break;
            if (circA && circB && c.a.entity != c.b.entity)
                break;
            return bad("Equal takes two lines (or polyline segments) or two circles/arcs");
        case Kind::Tangent:
            if (c.a.entity == c.b.entity ||
                !((segOk(doc, c.a) && circB) || (circA && segOk(doc, c.b)) || (circA && circB)))
                return bad("Tangent takes a line and a circle/arc, or two circles/arcs");
            break;
        case Kind::Fix:
            if (c.a.part != Part::Whole && !pointOk(doc, c.a))
                return bad("Fix takes an object or one of its points");
            break;
        case Kind::Distance:
            if (!pointOk(doc, c.a) || !pointOk(doc, c.b) || c.a == c.b)
                return bad("Distance measures between two points");
            if (!(c.value >= 0) || !std::isfinite(c.value))
                return bad("the distance must be zero or more");
            break;
        case Kind::Radius:
            if (ta != Type::Circle && ta != Type::Arc)
                return bad("Radius takes a circle or an arc");
            if (!(c.value > 0) || !std::isfinite(c.value))
                return bad("the radius must be more than zero");
            break;
        case Kind::Angle:
            if (!axisOk(doc, c.a) || !axisOk(doc, c.b) || c.a == c.b)
                return bad("Angle takes two different lines, polyline segments or inserts");
            if (!std::isfinite(c.value))
                return bad("the angle must be a number");
            break;
        case Kind::Concentric: {
            const bool ca = circA || ta == Type::Point || ta == Type::Ellipse;
            const bool cb = circB || tb == Type::Point || tb == Type::Ellipse;
            if (!ca || !cb || c.a.entity == c.b.entity)
                return bad("Concentric takes two different circles, arcs, ellipses or points");
            break;
        }
        case Kind::PointOnObject:
            if (!pointOk(doc, c.a) || c.a.entity == c.b.entity || !(segOk(doc, c.b) || circB))
                return bad(
                    "Point on object takes a point and a line, polyline segment, circle "
                    "or arc");
            break;
        case Kind::Midpoint:
            if (!pointOk(doc, c.a) || !segOk(doc, c.b) || c.a.entity == c.b.entity)
                return bad("Midpoint takes a point and a line or polyline segment");
            break;
        case Kind::Collinear:
            if (!segOk(doc, c.a) || !segOk(doc, c.b) || c.a == c.b)
                return bad("Collinear takes two different lines or polyline segments");
            break;
    }
    return {};
}

std::uint32_t ConstraintSet::add(Constraint c) {
    c.id = next_++;
    items_.push_back(c);
    return c.id;
}

bool ConstraintSet::remove(std::uint32_t id) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->id == id) {
            items_.erase(it);
            return true;
        }
    }
    return false;
}

const Constraint* ConstraintSet::find(std::uint32_t id) const {
    for (const Constraint& c : items_) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

Constraint* ConstraintSet::find(std::uint32_t id) {
    return const_cast<Constraint*>(std::as_const(*this).find(id));
}

bool ConstraintSet::involves(EntityId e) const {
    for (const Constraint& c : items_) {
        if (c.a.entity == e || c.b.entity == e)
            return true;
    }
    return false;
}

// Line format: "<id> <kind> <entityA> <partA> <entityB> <partB> <value> <indexA> <indexB>"; entity
// 0 = none. Files from before the indices were added end after the value.
std::string ConstraintSet::toText() const {
    std::string out;
    char buf[160];
    for (const Constraint& c : items_) {
        std::snprintf(buf, sizeof buf, "%u %d %llu %d %llu %d %.17g %u %u", c.id,
                      static_cast<int>(c.kind), static_cast<unsigned long long>(c.a.entity.value),
                      static_cast<int>(c.a.part), static_cast<unsigned long long>(c.b.entity.value),
                      static_cast<int>(c.b.part), c.value, c.a.index, c.b.index);
        out += buf;
        if (!c.expr.empty()) {
            out += " |";
            out += c.expr;
        }
        out += '\n';
    }
    return out;
}

ConstraintSet ConstraintSet::fromText(const std::string& text) {
    ConstraintSet set;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        unsigned id = 0, kind = 0, pa = 0, pb = 0;
        unsigned long long a = 0, b = 0;
        double value = 0;
        std::istringstream fields(line);
        if (!(fields >> id >> kind >> a >> pa >> b >> pb >> value))
            continue;
        unsigned ia = 0, ib = 0;
        if (!(fields >> ia >> ib))
            ia = ib = 0;
        if (kind > static_cast<unsigned>(Kind::Collinear) || pa > 3 || pb > 3 || a == 0 || id == 0)
            continue;
        Constraint c;
        c.id = id;
        c.kind = static_cast<Kind>(kind);
        c.a = {EntityId{a}, static_cast<Part>(pa), ia};
        c.b = {EntityId{b}, static_cast<Part>(pb), ib};
        c.value = value;
        if (const auto bar = line.find('|'); bar != std::string::npos) {
            c.expr = line.substr(bar + 1);
            while (!c.expr.empty() && (c.expr.back() == '\r' || c.expr.back() == ' '))
                c.expr.pop_back();
        }
        set.items_.push_back(c);
        set.next_ = std::max(set.next_, id + 1);
    }
    return set;
}

ConstraintSet load(const Document& doc) {
    auto it = doc.header().custom.find(kHeaderKey);
    return it == doc.header().custom.end() ? ConstraintSet{} : ConstraintSet::fromText(it->second);
}

Result<void> store(Transaction& tx, const ConstraintSet& set) {
    const std::string text = set.toText();
    return tx.modifyHeader([&](Header& h) {
        if (text.empty())
            h.custom.erase(kHeaderKey);
        else
            h.custom[kHeaderKey] = text;
    });
}

}  // namespace tbcad::constraints
