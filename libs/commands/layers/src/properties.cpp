#include "tbcad/layers/properties.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "tbcad/dim/dim.h"
#include "tbcad/geom/entity.h"
#include "tbcad/hatch/fill.h"
#include "tbcad/text/text.h"

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

std::vector<GeometryField> geometryFields(const Entity& e, int vertex) {
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
        num("deltax", "Delta X", d.x, false);
        num("deltay", "Delta Y", d.y, false);
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
        num("area", "Area", 0.5 * a->radius * a->radius * sweep, false);
        num("startx", "Start X", a->center.x + a->radius * std::cos(a->startAngle), false);
        num("starty", "Start Y", a->center.y + a->radius * std::sin(a->startAngle), false);
        num("endx", "End X", a->center.x + a->radius * std::cos(a->endAngle), false);
        num("endy", "End Y", a->center.y + a->radius * std::sin(a->endAngle), false);
    } else if (const auto* p = entityAs<PolylineData>(e)) {
        out.push_back(
            {"vertices", "Vertices", FieldType::Count, false, static_cast<double>(p->v.size())});
        if (!p->v.empty()) {
            // As AutoCAD: one vertex at a time, picked with "Current vertex".
            const auto i =
                static_cast<std::size_t>(std::clamp(vertex, 0, static_cast<int>(p->v.size()) - 1));
            const PolyVertex& pv = p->v[i];
            out.push_back(
                {"vertex", "Current vertex", FieldType::Count, true, static_cast<double>(i + 1)});
            num("vertexx", "Vertex X", pv.p.x);
            num("vertexy", "Vertex Y", pv.p.y);
            num("startwidth", "Start segment width", pv.startWidth);
            num("endwidth", "End segment width", pv.endWidth);
        }
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
        num("area", "Area", kPi * major * major * el->ratio, false);
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
    } else if (const auto* h = entityAs<HatchData>(e)) {
        out.push_back(
            {"loops", "Boundaries", FieldType::Count, false, static_cast<double>(h->loops.size())});
        num("area", "Area", hatch::hatchArea(*h), false);
    } else if (const auto* d = entityAs<DimensionData>(e)) {
        const bool angular = d->kind == DimKind::Angular2Line || d->kind == DimKind::Angular3Point;
        const double m = dim::measurement(*d);
        if (angular)
            out.push_back({"measurement", "Measurement", FieldType::Angle, false, deg(m)});
        else
            num("measurement", "Measurement", m, false);
        num("textx", "Text position X", d->textPos.x);
        num("texty", "Text position Y", d->textPos.y);
        out.push_back({"textrotation", "Text rotation", FieldType::Angle, true,
                       d->textRotated ? deg(d->textRotation) : 0.0});
    } else if (const auto* tb = entityAs<TableData>(e)) {
        double w = 0, hgt = 0;
        for (double x : tb->colWidths)
            w += x;
        for (double y : tb->rowHeights)
            hgt += y;
        num("x", "Position X", tb->origin.x);
        num("y", "Position Y", tb->origin.y);
        out.push_back({"rotation", "Rotation", FieldType::Angle, true, deg(tb->rotation)});
        out.push_back(
            {"rows", "Rows", FieldType::Count, false, static_cast<double>(tb->rowHeights.size())});
        out.push_back({"columns", "Columns", FieldType::Count, false,
                       static_cast<double>(tb->colWidths.size())});
        num("tablewidth", "Table width", w, false);
        num("tableheight", "Table height", hgt, false);
        num("textheight", "Text height", tb->textHeight);
        num("margin", "Cell margin", tb->margin);
    } else if (const auto* ml = entityAs<MLeaderData>(e)) {
        out.push_back({"leaders", "Leader lines", FieldType::Count, false,
                       static_cast<double>(ml->lines.size())});
        num("landingx", "Landing X", ml->landing.x);
        num("landingy", "Landing Y", ml->landing.y);
        if (ml->content == MLeaderData::Content::Block) {
            num("blockscale", "Block scale", ml->blockScale);
            out.push_back({"blockrotation", "Block rotation", FieldType::Angle, true,
                           deg(ml->blockRotation)});
        } else {
            num("textheight", "Text height", ml->textHeight);
        }
    } else if (const auto* ld = entityAs<LeaderData>(e)) {
        out.push_back({"vertices", "Vertices", FieldType::Count, false,
                       static_cast<double>(ld->vertices.size())});
    } else if (const auto* mln = entityAs<MlineData>(e)) {
        out.push_back(
            {"vertices", "Vertices", FieldType::Count, false, static_cast<double>(mln->v.size())});
        out.push_back({"closed", "Closed", FieldType::Flag, true, mln->closed ? 1.0 : 0.0});
        num("scale", "Scale", mln->scale);
    }
    return out;
}

std::pair<std::string, int> splitFieldVertex(const std::string& fieldId) {
    const auto at = fieldId.find('@');
    if (at == std::string::npos)
        return {fieldId, 0};
    const std::string n = fieldId.substr(at + 1);
    int index = -1;
    if (!n.empty() && n.size() < 9 &&
        std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; }))
        index = std::atoi(n.c_str()) - 1;
    return {fieldId.substr(0, at), index};
}

Result<void> setGeometryField(Transaction& tx, EntityId id, const std::string& field, double v) {
    // Plain variables, not a structured binding: the lambda below captures them.
    const auto split = splitFieldVertex(field);
    const std::string& f = split.first;
    const int vertex = split.second;
    if (vertex < 0)
        return makeError(ErrorCode::InvalidArgument, "Vertices are counted from 1.");
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
            const bool perVertex =
                f == "vertexx" || f == "vertexy" || f == "startwidth" || f == "endwidth";
            if (perVertex && static_cast<std::size_t>(vertex) >= p->v.size())
                return fail(bad("The polyline has " + std::to_string(p->v.size()) + " vertices."));
            if (f == "closed")
                p->closed = v != 0;
            else if (f == "vertexx")
                p->v[static_cast<std::size_t>(vertex)].p.x = v;
            else if (f == "vertexy")
                p->v[static_cast<std::size_t>(vertex)].p.y = v;
            else if (f == "startwidth" || f == "endwidth") {
                if (v < 0)
                    return fail(bad("Width cannot be negative."));
                PolyVertex& pv = p->v[static_cast<std::size_t>(vertex)];
                (f == "startwidth" ? pv.startWidth : pv.endWidth) = v;
                // Segment widths win over the global width, as in AutoCAD.
                if (p->constantWidth != 0) {
                    for (PolyVertex& o : p->v)
                        if (&o != &pv)
                            o.startWidth = o.endWidth = p->constantWidth;
                    (f == "startwidth" ? pv.endWidth : pv.startWidth) = p->constantWidth;
                    p->constantWidth = 0;
                }
            } else if (f == "width") {
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
        } else if (auto* d = entityAs<DimensionData>(e)) {
            if (f == "textx" || f == "texty") {
                (f == "textx" ? d->textPos.x : d->textPos.y) = v;
                d->textPosUser = true;
            } else if (f == "textrotation") {
                d->textRotated = true;
                d->textRotation = rad(v);
            } else
                return fail(bad("A dimension has no editable field \"" + f + "\"."));
        } else if (auto* tb = entityAs<TableData>(e)) {
            if (f == "x")
                tb->origin.x = v;
            else if (f == "y")
                tb->origin.y = v;
            else if (f == "rotation")
                tb->rotation = rad(v);
            else if (f == "textheight") {
                if (v <= 0)
                    return fail(bad("Height must be greater than zero."));
                tb->textHeight = v;
            } else if (f == "margin") {
                if (v < 0)
                    return fail(bad("The margin cannot be negative."));
                tb->margin = v;
            } else
                return fail(bad("A table has no editable field \"" + f + "\"."));
        } else if (auto* mld = entityAs<MLeaderData>(e)) {
            if (f == "landingx" || f == "landingy") {
                // The note and its dogleg travel with the landing.
                const Vec2 before = mld->landing;
                (f == "landingx" ? mld->landing.x : mld->landing.y) = v;
                mld->contentPos = mld->contentPos + (mld->landing - before);
            } else if (f == "textheight") {
                if (v < 0)
                    return fail(bad("Height cannot be negative."));
                mld->textHeight = v;
            } else if (f == "blockscale") {
                if (v <= 0)
                    return fail(bad("The scale must be greater than zero."));
                mld->blockScale = v;
            } else if (f == "blockrotation")
                mld->blockRotation = rad(v);
            else
                return fail(bad("A multileader has no editable field \"" + f + "\"."));
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

// ---- text fields ------------------------------------------------------------------------------

namespace {

struct JustRow {
    const char* name;
    HAlign h;
    VAlign v;
    Attachment a;
};
const JustRow kJustify[] = {
    {"Left", HAlign::Left, VAlign::Baseline, Attachment::TopLeft},
    {"Center", HAlign::Center, VAlign::Baseline, Attachment::TopCenter},
    {"Right", HAlign::Right, VAlign::Baseline, Attachment::TopRight},
    {"Aligned", HAlign::Aligned, VAlign::Baseline, Attachment::TopLeft},
    {"Middle", HAlign::Middle, VAlign::Middle, Attachment::MiddleCenter},
    {"Fit", HAlign::Fit, VAlign::Baseline, Attachment::TopLeft},
    {"Top Left", HAlign::Left, VAlign::Top, Attachment::TopLeft},
    {"Top Center", HAlign::Center, VAlign::Top, Attachment::TopCenter},
    {"Top Right", HAlign::Right, VAlign::Top, Attachment::TopRight},
    {"Middle Left", HAlign::Left, VAlign::Middle, Attachment::MiddleLeft},
    {"Middle Center", HAlign::Center, VAlign::Middle, Attachment::MiddleCenter},
    {"Middle Right", HAlign::Right, VAlign::Middle, Attachment::MiddleRight},
    {"Bottom Left", HAlign::Left, VAlign::Bottom, Attachment::BottomLeft},
    {"Bottom Center", HAlign::Center, VAlign::Bottom, Attachment::BottomCenter},
    {"Bottom Right", HAlign::Right, VAlign::Bottom, Attachment::BottomRight},
};

std::string lowerWord(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> styleNames(const Document& doc) {
    std::vector<std::string> out;
    for (const auto& [id, st] : doc.textStyles())
        out.push_back(st.name);
    return out;
}

std::string styleNameOf(const Document& doc, TextStyleId id) {
    if (const TextStyle* st = doc.textStyle(id.value ? id : doc.header().currentTextStyle))
        return st->name;
    return "Standard";
}

std::vector<std::string> dimStyleNames(const Document& doc) {
    std::vector<std::string> out;
    for (const auto& [id, st] : doc.dimStyles())
        out.push_back(st.name);
    return out;
}

std::string dimStyleNameOf(const Document& doc, DimStyleId id) {
    return dim::styleOf(doc, id).name;
}

}  // namespace

std::vector<StringField> stringFields(const Document& doc, const Entity& e) {
    std::vector<StringField> out;
    if (const auto* t = entityAs<TextData>(e)) {
        out.push_back({"contents", "Contents", t->utf8, true, {}});
        out.push_back({"style", "Style", styleNameOf(doc, t->style), true, styleNames(doc)});
        std::string j = "Left";
        for (const JustRow& r : kJustify) {
            const bool special =
                r.h == HAlign::Aligned || r.h == HAlign::Middle || r.h == HAlign::Fit;
            if (special ? t->h == r.h : (t->h == r.h && t->v == r.v)) {
                j = r.name;
                break;
            }
        }
        std::vector<std::string> names;
        for (const JustRow& r : kJustify)
            names.push_back(r.name);
        out.push_back({"justify", "Justify", j, true, names});
    } else if (const auto* m = entityAs<MTextData>(e)) {
        out.push_back({"contents", "Contents", m->utf8, true, {}});
        out.push_back({"style", "Style", styleNameOf(doc, m->style), true, styleNames(doc)});
        std::string j = "Top Left";
        std::vector<std::string> names;
        for (const JustRow& r : kJustify) {
            if (r.h == HAlign::Aligned || r.h == HAlign::Fit || r.h == HAlign::Middle ||
                r.v == VAlign::Baseline)
                continue;
            names.push_back(r.name);
            if (r.a == m->attach)
                j = r.name;
        }
        out.push_back({"attachment", "Attachment", j, true, names});
    } else if (const auto* in = entityAs<InsertData>(e)) {
        const BlockDef* b = doc.block(in->block);
        out.push_back({"name", "Block name", b ? b->name : "?", false, {}});
        // The attribute values, one row each, as AutoCAD's Attributes section.
        for (const AttribValue& a : in->attribs)
            out.push_back({"att:" + lowerWord(a.tag), a.tag, a.value, true, {}});
    } else if (const auto* d = entityAs<DimensionData>(e)) {
        out.push_back({"textoverride", "Text override", d->textOverride, true, {}});
        out.push_back(
            {"dimstyle", "Dim style", dimStyleNameOf(doc, d->style), true, dimStyleNames(doc)});
    } else if (const auto* ld = entityAs<LeaderData>(e)) {
        out.push_back({"contents", "Contents", ld->utf8, true, {}});
        out.push_back(
            {"dimstyle", "Dim style", dimStyleNameOf(doc, ld->style), true, dimStyleNames(doc)});
    } else if (const auto* ml = entityAs<MLeaderData>(e)) {
        if (ml->content == MLeaderData::Content::Text)
            out.push_back({"contents", "Contents", ml->utf8, true, {}});
        // The block's attribute values, as for an Insert.
        for (const AttribValue& a : ml->attribs)
            out.push_back({"att:" + lowerWord(a.tag), a.tag, a.value, true, {}});
        out.push_back(
            {"dimstyle", "Style", dimStyleNameOf(doc, ml->style), true, dimStyleNames(doc)});
    } else if (const auto* tb = entityAs<TableData>(e)) {
        out.push_back({"style", "Text style", styleNameOf(doc, tb->style), true, styleNames(doc)});
    }
    return out;
}

Result<void> setStringField(const Document& doc, Transaction& tx, EntityId id, const std::string& f,
                            const std::string& value) {
    const Entity* cur = doc.find(id);
    if (!cur)
        return makeError(ErrorCode::NotFound, "No such object.");
    auto fields = stringFields(doc, *cur);
    auto it =
        std::find_if(fields.begin(), fields.end(), [&](const StringField& x) { return x.id == f; });
    if (it == fields.end())
        return makeError(ErrorCode::InvalidArgument,
                         std::string(kindLabel(kind(*cur))) + " has no field \"" + f + "\".");
    if (!it->editable)
        return makeError(ErrorCode::InvalidArgument, "\"" + f + "\" is read-only.");
    std::string chosen = value;
    if (!it->choices.empty()) {
        const std::string want = lowerWord(value);
        const std::string* hit = nullptr;
        for (const std::string& c : it->choices)
            if (lowerWord(c) == want)
                hit = &c;
        if (!hit)
            return makeError(ErrorCode::InvalidArgument,
                             "\"" + value + "\" is not a choice for " + it->label + ".");
        chosen = *hit;
    }
    if (f == "contents" && chosen.empty() && kind(*cur) != EntityKind::Leader &&
        kind(*cur) != EntityKind::MLeader)
        return makeError(ErrorCode::InvalidArgument, "The text cannot be empty.");
    Entity next = *cur;
    auto dimStyleId = [&](DimStyleId& out) -> Result<void> {
        const DimStyle* st = doc.dimStyleByName(chosen);
        if (!st)
            return makeError(ErrorCode::NotFound, "No such dimension style.");
        out = st->id;
        return {};
    };
    Result<void> sub{};
    if (auto* t = entityAs<TextData>(next)) {
        if (f == "contents") {
            t->utf8 = chosen;
        } else if (f == "style") {
            const TextStyle* st = doc.textStyleByName(chosen);
            if (!st)
                return makeError(ErrorCode::NotFound, "No such text style.");
            t->style = st->id;
        } else if (f == "justify") {
            for (const JustRow& r : kJustify) {
                if (chosen != r.name)
                    continue;
                text::TextLayout before = text::layoutEntity(doc, *cur);
                if (t->h == HAlign::Aligned || t->h == HAlign::Fit) {
                    // Keep its look: Aligned text grows with its width, Fit text stretches.
                    TextData plain = *t;
                    plain.h = HAlign::Left;
                    plain.v = VAlign::Baseline;
                    plain.alignPoint = plain.position;
                    const auto pc = text::layoutText(doc, plain).frameCorners();
                    const double natural = distance(pc[0], pc[1]);
                    const Vec2 span = t->alignPoint - t->position;
                    const double D = length(span);
                    if (natural > tol::kLinear && D > tol::kLinear) {
                        if (t->h == HAlign::Aligned)
                            t->height *= D / natural;
                        else
                            t->widthFactor *= D / natural;
                        t->rotation = std::atan2(span.y, span.x);
                    }
                }
                // Aligned and Fit span the text from where it now starts to where it ends.
                const bool toSpan = r.h == HAlign::Aligned || r.h == HAlign::Fit;
                t->h = toSpan ? HAlign::Left : r.h;
                t->v = r.v;
                t->position = t->alignPoint = {0, 0};
                text::TextLayout after = text::layoutEntity(doc, next);
                if (!before.empty() && !after.empty())
                    t->position = t->alignPoint =
                        before.frameCorners()[0] - after.frameCorners()[0];
                else
                    t->position = t->alignPoint = entityAs<TextData>(*cur)->position;
                if (toSpan && !after.empty()) {
                    const auto c = after.frameCorners();
                    t->h = r.h;
                    t->alignPoint = t->position + (c[1] - c[0]);
                }
                break;
            }
        }
    } else if (auto* m = entityAs<MTextData>(next)) {
        if (f == "contents") {
            m->utf8 = chosen;
        } else if (f == "style") {
            const TextStyle* st = doc.textStyleByName(chosen);
            if (!st)
                return makeError(ErrorCode::NotFound, "No such text style.");
            m->style = st->id;
        } else if (f == "attachment") {
            for (const JustRow& r : kJustify) {
                if (chosen != r.name)
                    continue;
                text::TextLayout before = text::layoutEntity(doc, *cur);
                m->attach = r.a;
                m->position = {0, 0};
                text::TextLayout after = text::layoutEntity(doc, next);
                if (!before.empty() && !after.empty())
                    m->position = before.frameCorners()[0] - after.frameCorners()[0];
                else
                    m->position = entityAs<MTextData>(*cur)->position;
                break;
            }
        }
    } else if (auto* in = entityAs<InsertData>(next)) {
        for (AttribValue& a : in->attribs)
            if ("att:" + lowerWord(a.tag) == f)
                a.value = chosen;
    } else if (auto* d = entityAs<DimensionData>(next)) {
        if (f == "textoverride")
            d->textOverride = chosen;
        else if (f == "dimstyle")
            sub = dimStyleId(d->style);
    } else if (auto* ld = entityAs<LeaderData>(next)) {
        if (f == "contents")
            ld->utf8 = chosen;
        else if (f == "dimstyle")
            sub = dimStyleId(ld->style);
    } else if (auto* ml = entityAs<MLeaderData>(next)) {
        if (f == "contents")
            ml->utf8 = chosen;
        else if (f == "dimstyle")
            sub = dimStyleId(ml->style);
        for (AttribValue& a : ml->attribs)
            if ("att:" + lowerWord(a.tag) == f)
                a.value = chosen;
    } else if (auto* tb = entityAs<TableData>(next)) {
        const TextStyle* st = doc.textStyleByName(chosen);
        if (!st)
            return makeError(ErrorCode::NotFound, "No such text style.");
        tb->style = st->id;
    }
    if (!sub)
        return sub.error();
    auto r = tx.modify(id, [&](Entity& x) { x = std::move(next); });
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
