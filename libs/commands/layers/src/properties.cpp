#include "tbcad/layers/properties.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "tbcad/geom/entity.h"

namespace tbcad::layers {

namespace {

template <class T>
void accumulate(Shared<T>& s, bool first, const T& v) {
    if (first) {
        s.value = v;
        s.varies = false;
    } else if (!(s.value == v)) {
        s.varies = true;
    }
}

bool matches(const Entity& e, std::optional<EntityKind> only) {
    return !only || kind(e) == *only;
}

double deg(double rad) {
    return rad * 180.0 / kPi;
}
double rad(double d) {
    return d * kPi / 180.0;
}

}  // namespace

const char* kindLabel(EntityKind k) {
    switch (k) {
        case EntityKind::Line:
            return "Line";
        case EntityKind::Circle:
            return "Circle";
        case EntityKind::Arc:
            return "Arc";
        case EntityKind::Polyline:
            return "Polyline";
        case EntityKind::Point:
            return "Point";
        case EntityKind::Ellipse:
            return "Ellipse";
        case EntityKind::Spline:
            return "Spline";
        case EntityKind::Text:
            return "Text";
        case EntityKind::MText:
            return "MText";
        case EntityKind::Hatch:
            return "Hatch";
        case EntityKind::Insert:
            return "Block";
        case EntityKind::Dimension:
            return "Dimension";
        case EntityKind::Leader:
            return "Leader";
        case EntityKind::Proxy:
            return "Proxy";
        case EntityKind::Table:
            return "Table";
        case EntityKind::Ray:
            return "Ray";
        case EntityKind::MLine:
            return "Multiline";
        case EntityKind::MLeader:
            return "Multileader";
    }
    return "Object";
}

CommonProps readCommon(const Document& doc, const std::vector<EntityId>& ids,
                       std::optional<EntityKind> only) {
    CommonProps p;
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        ++p.kinds[kind(*e)];
        if (!matches(*e, only))
            continue;
        const EntityCommon& c = e->common;
        bool first = p.count == 0;
        accumulate(p.layer, first, c.layer);
        accumulate(p.color, first, c.color);
        accumulate(p.linetype, first, c.linetype);
        accumulate(p.linetypeScale, first, static_cast<double>(c.linetypeScale));
        accumulate(p.lineweight, first, c.lineweight);
        accumulate(p.transparency, first, static_cast<int>(c.transparency));
        ++p.count;
    }
    return p;
}

Result<EditSummary> applyCommon(const Document& doc, Transaction& tx,
                                const std::vector<EntityId>& ids, const CommonEdit& edit,
                                std::optional<EntityKind> only) {
    if (edit.layer && !doc.layer(*edit.layer))
        return makeError(ErrorCode::NotFound, "No such layer.");
    if (edit.color && (edit.color->kind == ColorKind::ByParent))
        return makeError(ErrorCode::InvalidArgument, "ByParent applies to layers only.");
    if (edit.transparency && !validObjectTransparency(*edit.transparency))
        return makeError(ErrorCode::InvalidArgument, "Transparency must be 0 to 90.");
    if (edit.linetypeScale && !(*edit.linetypeScale > 0 && std::isfinite(*edit.linetypeScale)))
        return makeError(ErrorCode::InvalidArgument, "Linetype scale must be greater than zero.");
    EditSummary sum;
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e || !matches(*e, only))
            continue;
        if (doc.effective(e->common).locked) {
            ++sum.skippedLocked;
            continue;
        }
        auto r = tx.modify(id, [&](Entity& x) {
            if (edit.layer)
                x.common.layer = *edit.layer;
            if (edit.color)
                x.common.color = *edit.color;
            if (edit.linetype)
                x.common.linetype = *edit.linetype;
            if (edit.linetypeScale)
                x.common.linetypeScale = static_cast<float>(*edit.linetypeScale);
            if (edit.lineweight)
                x.common.lineweight = *edit.lineweight;
            if (edit.transparency)
                x.common.transparency = static_cast<std::int8_t>(*edit.transparency);
        });
        if (!r)
            return r.error();
        ++sum.changed;
    }
    return sum;
}

std::vector<GeometryField> geometryFields(const Entity& e) {
    using F = GeometryField;
    std::vector<F> out;
    auto num = [&](const char* id, const char* label, double v, bool editable = true) {
        out.push_back({id, label, FieldType::Number, editable, v});
    };
    if (const auto* l = entityAs<LineData>(e)) {
        Vec2 d = l->b - l->a;
        num("startx", "Start X", l->a.x);
        num("starty", "Start Y", l->a.y);
        num("endx", "End X", l->b.x);
        num("endy", "End Y", l->b.y);
        num("length", "Length", std::hypot(d.x, d.y));
        out.push_back(
            {"angle", "Angle", FieldType::Angle, true, deg(normalizeAngle(std::atan2(d.y, d.x)))});
    } else if (const auto* c = entityAs<CircleData>(e)) {
        num("centerx", "Center X", c->center.x);
        num("centery", "Center Y", c->center.y);
        num("radius", "Radius", c->radius);
        num("diameter", "Diameter", c->radius * 2);
        num("circumference", "Circumference", kTwoPi * c->radius, false);
        num("area", "Area", kPi * c->radius * c->radius, false);
    } else if (const auto* a = entityAs<ArcData>(e)) {
        double sweep = normalizeAngle(a->endAngle - a->startAngle);
        num("centerx", "Center X", a->center.x);
        num("centery", "Center Y", a->center.y);
        num("radius", "Radius", a->radius);
        out.push_back({"startangle", "Start angle", FieldType::Angle, true, deg(a->startAngle)});
        out.push_back({"endangle", "End angle", FieldType::Angle, true, deg(a->endAngle)});
        out.push_back({"totalangle", "Total angle", FieldType::Angle, false, deg(sweep)});
        num("length", "Length", a->radius * sweep, false);
    } else if (const auto* p = entityAs<PolylineData>(e)) {
        out.push_back(
            {"vertices", "Vertices", FieldType::Count, false, static_cast<double>(p->v.size())});
        out.push_back({"closed", "Closed", FieldType::Flag, true, p->closed ? 1.0 : 0.0});
        num("length", "Length", geom::length(*p), false);
        num("area", "Area", geom::area(*p), false);
        num("width", "Global width", p->constantWidth);
    } else if (const auto* pt = entityAs<PointData>(e)) {
        num("x", "X", pt->p.x);
        num("y", "Y", pt->p.y);
    } else if (const auto* el = entityAs<EllipseData>(e)) {
        const double major = std::hypot(el->majorAxis.x, el->majorAxis.y);
        num("centerx", "Center X", el->center.x);
        num("centery", "Center Y", el->center.y);
        num("majorradius", "Major radius", major);
        num("minorradius", "Minor radius", major * el->ratio);
        num("ratio", "Radius ratio", el->ratio);
        out.push_back({"rotation", "Rotation", FieldType::Angle, true,
                       deg(normalizeAngle(std::atan2(el->majorAxis.y, el->majorAxis.x)))});
        out.push_back(
            {"startparam", "Start parameter", FieldType::Angle, true, deg(el->startParam)});
        out.push_back({"endparam", "End parameter", FieldType::Angle, true, deg(el->endParam)});
    } else if (const auto* sp = entityAs<SplineData>(e)) {
        out.push_back(
            {"degree", "Degree", FieldType::Count, false, static_cast<double>(sp->degree)});
        out.push_back({"controlpoints", "Control points", FieldType::Count, false,
                       static_cast<double>(sp->control.size())});
        out.push_back({"fitpoints", "Fit points", FieldType::Count, false,
                       static_cast<double>(sp->fit.size())});
        out.push_back({"closed", "Closed", FieldType::Flag, false, sp->closed ? 1.0 : 0.0});
    } else if (const auto* t = entityAs<TextData>(e)) {
        num("x", "Position X", t->position.x);
        num("y", "Position Y", t->position.y);
        num("height", "Height", t->height);
        out.push_back({"rotation", "Rotation", FieldType::Angle, true, deg(t->rotation)});
        num("widthfactor", "Width factor", t->widthFactor);
        out.push_back({"oblique", "Oblique angle", FieldType::Angle, true, deg(t->oblique)});
    } else if (const auto* mt = entityAs<MTextData>(e)) {
        num("x", "Position X", mt->position.x);
        num("y", "Position Y", mt->position.y);
        num("height", "Text height", mt->height);
        num("width", "Defined width", mt->width);
        out.push_back({"rotation", "Rotation", FieldType::Angle, true, deg(mt->rotation)});
    } else if (const auto* in = entityAs<InsertData>(e)) {
        num("x", "Position X", in->position.x);
        num("y", "Position Y", in->position.y);
        num("scalex", "Scale X", in->scale.x);
        num("scaley", "Scale Y", in->scale.y);
        out.push_back({"rotation", "Rotation", FieldType::Angle, true, deg(in->rotation)});
    } else if (const auto* ray = entityAs<RayData>(e)) {
        num("x", "Base X", ray->origin.x);
        num("y", "Base Y", ray->origin.y);
        out.push_back({"angle", "Angle", FieldType::Angle, true,
                       deg(normalizeAngle(std::atan2(ray->dir.y, ray->dir.x)))});
    } else if (const auto* ld = entityAs<LeaderData>(e)) {
        out.push_back({"vertices", "Vertices", FieldType::Count, false,
                       static_cast<double>(ld->vertices.size())});
    } else if (const auto* ml = entityAs<MlineData>(e)) {
        out.push_back(
            {"vertices", "Vertices", FieldType::Count, false, static_cast<double>(ml->v.size())});
        out.push_back({"closed", "Closed", FieldType::Flag, true, ml->closed ? 1.0 : 0.0});
        num("scale", "Scale", ml->scale);
    }
    return out;
}

Result<void> setGeometryField(Transaction& tx, EntityId id, const std::string& f, double v) {
    if (!std::isfinite(v))
        return makeError(ErrorCode::InvalidArgument, "Value is not a finite number.");
    auto bad = [](const std::string& m) { return makeError(ErrorCode::InvalidArgument, m); };
    std::optional<Error> failure;
    auto r = tx.modify(id, [&](Entity& e) {
        auto fail = [&](Error err) { failure = std::move(err); };
        if (auto* l = entityAs<LineData>(e)) {
            Vec2 d = l->b - l->a;
            double len = std::hypot(d.x, d.y);
            double ang = std::atan2(d.y, d.x);
            if (f == "startx")
                l->a.x = v;
            else if (f == "starty")
                l->a.y = v;
            else if (f == "endx")
                l->b.x = v;
            else if (f == "endy")
                l->b.y = v;
            else if (f == "length") {
                if (v <= 0)
                    return fail(bad("Length must be greater than zero."));
                l->b = l->a + Vec2{std::cos(ang) * v, std::sin(ang) * v};
            } else if (f == "angle") {
                double a = rad(v);
                l->b = l->a + Vec2{std::cos(a) * len, std::sin(a) * len};
            } else
                return fail(bad("A line has no editable field \"" + f + "\"."));
        } else if (auto* c = entityAs<CircleData>(e)) {
            if (f == "centerx")
                c->center.x = v;
            else if (f == "centery")
                c->center.y = v;
            else if (f == "radius" || f == "diameter") {
                double r2 = f == "radius" ? v : v / 2;
                if (r2 <= 0)
                    return fail(bad("Radius must be greater than zero."));
                c->radius = r2;
            } else
                return fail(bad("A circle has no editable field \"" + f + "\"."));
        } else if (auto* a = entityAs<ArcData>(e)) {
            if (f == "centerx")
                a->center.x = v;
            else if (f == "centery")
                a->center.y = v;
            else if (f == "radius") {
                if (v <= 0)
                    return fail(bad("Radius must be greater than zero."));
                a->radius = v;
            } else if (f == "startangle" || f == "endangle") {
                double n = normalizeAngle(rad(v));
                double s = f == "startangle" ? n : a->startAngle;
                double en = f == "endangle" ? n : a->endAngle;
                if (std::fabs(s - en) < tol::kAngular)
                    return fail(bad("An arc needs different start and end angles."));
                a->startAngle = s;
                a->endAngle = en;
            } else
                return fail(bad("An arc has no editable field \"" + f + "\"."));
        } else if (auto* p = entityAs<PolylineData>(e)) {
            if (f == "closed")
                p->closed = v != 0;
            else if (f == "width") {
                if (v < 0)
                    return fail(bad("Width cannot be negative."));
                p->constantWidth = v;
            } else
                return fail(bad("A polyline has no editable field \"" + f + "\"."));
        } else if (auto* pt = entityAs<PointData>(e)) {
            if (f == "x")
                pt->p.x = v;
            else if (f == "y")
                pt->p.y = v;
            else
                return fail(bad("A point has no editable field \"" + f + "\"."));
        } else if (auto* el = entityAs<EllipseData>(e)) {
            const double major = std::hypot(el->majorAxis.x, el->majorAxis.y);
            const double ang = std::atan2(el->majorAxis.y, el->majorAxis.x);
            if (f == "centerx")
                el->center.x = v;
            else if (f == "centery")
                el->center.y = v;
            else if (f == "majorradius" || f == "minorradius" || f == "ratio") {
                if (v <= 0)
                    return fail(bad("A radius or ratio must be greater than zero."));
                double newMajor = major, newRatio = el->ratio;
                if (f == "majorradius")
                    newMajor = v;
                else if (f == "minorradius")
                    newRatio = v / major;
                else
                    newRatio = v;
                // The minor axis may not become longer than the major one.
                if (newRatio > 1)
                    return fail(bad("The minor radius cannot exceed the major radius."));
                el->majorAxis = Vec2{std::cos(ang), std::sin(ang)} * newMajor;
                el->ratio = newRatio;
            } else if (f == "rotation") {
                el->majorAxis = Vec2{std::cos(rad(v)), std::sin(rad(v))} * major;
            } else if (f == "startparam") {
                el->startParam = rad(v);
            } else if (f == "endparam") {
                el->endParam = rad(v);
            } else
                return fail(bad("An ellipse has no editable field \"" + f + "\"."));
        } else if (auto* t = entityAs<TextData>(e)) {
            if (f == "x" || f == "y") {
                // The alignment point travels with the position.
                const Vec2 before = t->position;
                (f == "x" ? t->position.x : t->position.y) = v;
                t->alignPoint = t->alignPoint + (t->position - before);
            } else if (f == "height") {
                if (v <= 0)
                    return fail(bad("Height must be greater than zero."));
                t->height = v;
            } else if (f == "rotation")
                t->rotation = rad(v);
            else if (f == "widthfactor") {
                if (v <= 0)
                    return fail(bad("Width factor must be greater than zero."));
                t->widthFactor = v;
            } else if (f == "oblique")
                t->oblique = rad(v);
            else
                return fail(bad("Text has no editable field \"" + f + "\"."));
        } else if (auto* mt = entityAs<MTextData>(e)) {
            if (f == "x")
                mt->position.x = v;
            else if (f == "y")
                mt->position.y = v;
            else if (f == "height") {
                if (v <= 0)
                    return fail(bad("Height must be greater than zero."));
                mt->height = v;
            } else if (f == "width") {
                if (v < 0)
                    return fail(bad("Width cannot be negative."));
                mt->width = v;
            } else if (f == "rotation")
                mt->rotation = rad(v);
            else
                return fail(bad("Multiline text has no editable field \"" + f + "\"."));
        } else if (auto* in = entityAs<InsertData>(e)) {
            if (f == "x")
                in->position.x = v;
            else if (f == "y")
                in->position.y = v;
            else if (f == "scalex" || f == "scaley") {
                if (v == 0)
                    return fail(bad("A scale cannot be zero."));
                (f == "scalex" ? in->scale.x : in->scale.y) = v;
            } else if (f == "rotation")
                in->rotation = rad(v);
            else
                return fail(bad("A block reference has no editable field \"" + f + "\"."));
        } else if (auto* ray = entityAs<RayData>(e)) {
            if (f == "x")
                ray->origin.x = v;
            else if (f == "y")
                ray->origin.y = v;
            else if (f == "angle")
                ray->dir = Vec2{std::cos(rad(v)), std::sin(rad(v))};
            else
                return fail(bad("A ray has no editable field \"" + f + "\"."));
        } else if (auto* ml = entityAs<MlineData>(e)) {
            if (f == "closed")
                ml->closed = v != 0;
            else if (f == "scale")
                ml->scale = v;
            else
                return fail(bad("A multiline has no editable field \"" + f + "\"."));
        } else {
            fail(bad("This kind of object has no geometry fields."));
        }
    });
    if (failure)
        return *failure;
    if (!r)
        return r.error();
    return {};
}

// ---- number expressions -----------------------------------------------------------------------

namespace {

// Recursive descent: expr = term (('+'|'-') term)*, term = unary (('*'|'/') unary)*,
// unary = ['-'|'+'] primary, primary = number [unit] | '(' expr ')'.
class ExprParser {
public:
    ExprParser(const std::string& s, bool plain) : s_(s), plain_(plain) {}

    Result<double> run() {
        double v = expr();
        skip();
        if (!err_.empty() || pos_ != s_.size())
            return makeError(ErrorCode::Parse,
                             err_.empty() ? "Cannot read \"" + s_ + "\" as a number." : err_);
        if (!std::isfinite(v))
            return makeError(ErrorCode::Parse, "The result is not a finite number.");
        return v;
    }

private:
    void skip() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_])))
            ++pos_;
    }
    bool eat(char c) {
        skip();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    double expr() {
        double v = term();
        while (err_.empty()) {
            if (eat('+'))
                v += term();
            else if (eat('-'))
                v -= term();
            else
                break;
        }
        return v;
    }
    double term() {
        double v = unary();
        while (err_.empty()) {
            if (eat('*'))
                v *= unary();
            else if (eat('/')) {
                double d = unary();
                if (d == 0) {
                    err_ = "Division by zero.";
                    return 0;
                }
                v /= d;
            } else
                break;
        }
        return v;
    }
    double unary() {
        if (eat('-'))
            return -unary();
        if (eat('+'))
            return unary();
        return primary();
    }
    double primary() {
        skip();
        if (eat('(')) {
            double v = expr();
            if (!eat(')') && err_.empty())
                err_ = "Missing closing parenthesis.";
            return v;
        }
        const char* b = s_.c_str() + pos_;
        char* end = nullptr;
        double v = std::strtod(b, &end);
        if (end == b) {
            if (err_.empty())
                err_ = "Cannot read \"" + s_ + "\" as a number.";
            return 0;
        }
        pos_ += static_cast<std::size_t>(end - b);
        // Unit suffix.
        std::size_t u = pos_;
        while (u < s_.size() && std::isalpha(static_cast<unsigned char>(s_[u])))
            ++u;
        if (u > pos_) {
            std::string unit = s_.substr(pos_, u - pos_);
            for (char& c : unit)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            double f = 0;
            if (unit == "mm")
                f = 1;
            else if (unit == "cm")
                f = 10;
            else if (unit == "m")
                f = 1000;
            else if (unit == "in")
                f = 25.4;
            else if (unit == "ft")
                f = 304.8;
            else {
                err_ = "Unknown unit \"" + unit + "\".";
                return 0;
            }
            if (plain_) {
                err_ = "Units are not allowed here.";
                return 0;
            }
            v *= f;
            pos_ = u;
        }
        return v;
    }

    const std::string& s_;
    bool plain_;
    std::size_t pos_ = 0;
    std::string err_{};
};

}  // namespace

Result<double> parseNumberExpr(const std::string& text, bool plain) {
    if (text.find_first_not_of(" \t") == std::string::npos)
        return makeError(ErrorCode::Parse, "Value is empty.");
    return ExprParser(text, plain).run();
}

}  // namespace tbcad::layers
