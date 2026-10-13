#include <cmath>

#include "tbcad/dim/dim.h"
#include "tbcad/text/text.h"

namespace tbcad::dim {

namespace {

const DimStyle& styleForEntity(const Document& doc, const Entity& e) {
    if (const auto* d = entityAs<DimensionData>(e))
        return styleOf(doc, d->style);
    if (const auto* l = entityAs<LeaderData>(e))
        return styleOf(doc, l->style);
    return styleOf(doc, {});
}

Color roleColor(const DimStyle& st, Role r) {
    switch (r) {
        case Role::DimLine:
            return st.dimLineColor;
        case Role::ExtLine:
            return st.extLineColor;
        case Role::Text:
            return st.textColor;
    }
    return Color::byBlock();
}

// A copy of the entity's common properties with the style's colour for `role` (ByBlock in the
// style means "the dimension's own colour").
EntityCommon commonFor(const Entity& e, const DimStyle& st, Role role) {
    EntityCommon c = e.common;
    c.id = {};
    Color col = roleColor(st, role);
    if (col.kind != ColorKind::ByBlock)
        c.color = col;
    return c;
}

Entity textEntity(const Entity& e, const DimStyle& st, const Layout::Text& t, TextStyleId style) {
    Entity out;
    out.common = commonFor(e, st, Role::Text);
    if (t.mtext) {
        MTextData m;
        m.position = t.center;
        m.height = t.height;
        m.rotation = t.rotation;
        m.attach = t.attachRight ? Attachment::MiddleRight : Attachment::MiddleLeft;
        m.style = style;
        m.utf8 = t.utf8;
        out.data = Indirect<MTextData>(std::move(m));
    } else {
        TextData td;
        td.position = td.alignPoint = t.center;
        td.height = t.height;
        td.rotation = t.rotation;
        td.h = HAlign::Center;
        td.v = VAlign::Middle;
        td.style = style;
        td.utf8 = t.utf8;
        out.data = Indirect<TextData>(std::move(td));
    }
    return out;
}

TextStyleId textStyleOf(const DimStyle& st) {
    return st.textStyle;
}

Layout layoutOf(const Document& doc, const Entity& e, double tol) {
    if (const auto* d = entityAs<DimensionData>(e))
        return layoutDimension(doc, *d, tol);
    if (const auto* l = entityAs<LeaderData>(e))
        return layoutLeader(doc, *l, tol);
    return {};
}

}  // namespace

void buildDimensionDisplay(const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
                           DisplayList& out) {
    const DimStyle& st = styleForEntity(doc, e);
    const double tol = ctx.tolerance();
    Layout L = layoutOf(doc, e, tol);

    auto styleOfRole = [&](Role r) {
        Entity tmp;
        tmp.common = commonFor(e, st, r);
        return geom::resolveStyle(doc, tmp, ctx);
    };
    const DisplayStyle lineStyle = styleOfRole(Role::DimLine);
    const DisplayStyle extStyle = styleOfRole(Role::ExtLine);

    for (const Layout::Line& l : L.lines) {
        DisplayPolyline pl;
        pl.pts = l.pts;
        pl.style = l.role == Role::ExtLine ? extStyle : lineStyle;
        out.items.emplace_back(std::move(pl));
    }
    for (const Layout::Arc& a : L.arcs) {
        DisplayPolyline pl;
        pl.pts = arcPoints(a, tol);
        pl.style = a.role == Role::ExtLine ? extStyle : lineStyle;
        out.items.emplace_back(std::move(pl));
    }
    for (const Layout::Arrow& a : L.arrows) {
        DisplayFill f;
        f.rings.push_back(a.ring);
        f.style = lineStyle;
        f.style.pattern = nullptr;
        out.items.emplace_back(std::move(f));
    }
    if (L.text.valid())
        text::buildTextDisplay(doc, textEntity(e, st, L.text, textStyleOf(st)), ctx, out);
}

void registerDisplay() {
    geom::DisplayRegistry::set(EntityKind::Dimension, buildDimensionDisplay);
    geom::DisplayRegistry::set(EntityKind::Leader, buildDimensionDisplay);
}

Result<std::vector<Entity>> explodeDimension(const Document& doc, const Entity& e,
                                             double tolerance) {
    if (kind(e) != EntityKind::Dimension && kind(e) != EntityKind::Leader)
        return makeError(ErrorCode::InvalidArgument, "not a dimension or leader");
    const DimStyle& st = styleForEntity(doc, e);
    Layout L = layoutOf(doc, e, tolerance);
    std::vector<Entity> out;
    auto make = [&](Role role, EntityData data) {
        Entity n;
        n.common = commonFor(e, st, role);
        n.data = std::move(data);
        out.push_back(std::move(n));
    };
    for (const Layout::Line& l : L.lines) {
        if (l.pts.size() == 2) {
            make(l.role, LineData{l.pts[0], l.pts[1]});
        } else {
            PolylineData pl;
            for (const Vec2& p : l.pts)
                pl.v.push_back({p, 0, 0, 0});
            make(l.role, std::move(pl));
        }
    }
    for (const Layout::Arc& a : L.arcs)
        make(a.role, ArcData{a.center, a.radius, a.startAngle, a.endAngle});
    for (const Layout::Arrow& a : L.arrows) {
        HatchData h;
        HatchLoop loop;
        HatchPolylineEdge edge;
        for (const Vec2& p : a.ring)
            edge.v.push_back({p, 0, 0, 0});
        edge.closed = true;
        loop.edges.push_back(std::move(edge));
        h.loops.push_back(std::move(loop));
        h.solid = true;
        make(Role::DimLine, Indirect<HatchData>(std::move(h)));
    }
    if (L.text.valid()) {
        Entity t = textEntity(e, st, L.text, textStyleOf(st));
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace tbcad::dim
