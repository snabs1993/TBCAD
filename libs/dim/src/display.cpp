#include <cmath>
#include <optional>

#include "tbcad/dim/dim.h"
#include "tbcad/text/text.h"

namespace tbcad::dim {

namespace {

const DimStyle& styleForEntity(const Document& doc, const Entity& e) {
    if (const auto* d = entityAs<DimensionData>(e))
        return styleOf(doc, d->style);
    if (const auto* l = entityAs<LeaderData>(e))
        return styleOf(doc, l->style);
    if (const auto* m = entityAs<MLeaderData>(e))
        return styleOf(doc, m->style);
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
    c.annotative = false;  // parts are drawn at the scaled size already
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

Layout layoutOf(const Document& doc, const Entity& e, double tol, double annoScale = 0) {
    const double f = annotationFactor(doc.header(), e.common, annoScale);
    if (const auto* d = entityAs<DimensionData>(e))
        return layoutDimension(doc, *d, tol, f);
    if (const auto* l = entityAs<LeaderData>(e))
        return layoutLeader(doc, *l, tol, f);
    if (const auto* m = entityAs<MLeaderData>(e))
        return layoutMLeader(doc, *m, tol, f);
    return {};
}

// The block of a multileader as an insert at the end of the dogleg, scaled with the annotation.
std::optional<Entity> blockContent(const Document& doc, const Entity& e, double annoScale) {
    const auto* m = entityAs<MLeaderData>(e);
    if (!m || m->content != MLeaderData::Content::Block || !doc.block(m->block))
        return std::nullopt;
    const double f = annotationFactor(doc.header(), e.common, annoScale);
    InsertData ins;
    ins.block = m->block;
    ins.position = m->contentPos;
    ins.scale = {m->blockScale * f, m->blockScale * f};
    ins.rotation = m->blockRotation;
    Entity out;
    out.common = e.common;
    out.common.id = {};
    out.common.annotative = false;
    out.data = Indirect<InsertData>(std::move(ins));
    return out;
}

// The display style of the dimension's text, for the fill behind it.
DisplayStyle textStyleFor(const Document& doc, const Entity& e, const DimStyle& st,
                          const geom::DisplayContext& ctx) {
    Entity tmp;
    tmp.common = commonFor(e, st, Role::Text);
    return geom::resolveStyle(doc, tmp, ctx);
}

}  // namespace

void buildDimensionDisplay(const Document& doc, const Entity& e, const geom::DisplayContext& ctx,
                           DisplayList& out) {
    const DimStyle& st = styleForEntity(doc, e);
    const double tol = ctx.tolerance();
    Layout L = layoutOf(doc, e, tol, ctx.annoScale);

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
    // DIMTFILL: the text sits on a box in the drawing background or a chosen colour, hiding
    // the dimension line behind it.
    if (st.textFill != 0 && L.text.valid() && L.text.width > 0 && !L.text.mtext) {
        const Vec2 ux{std::cos(L.text.rotation), std::sin(L.text.rotation)};
        const Vec2 uy{-ux.y, ux.x};
        const double k = std::max(0.0, st.textGap * (st.overallScale > 0 ? st.overallScale : 1.0));
        const double hw = L.text.width / 2 + k, hh = L.text.height / 2 + k;
        DisplayFill bg;
        bg.style = textStyleFor(doc, e, st, ctx);
        bg.style.pattern = nullptr;
        bg.style.color = st.textFill == 2 && st.textFillColor >= 1 && st.textFillColor <= 255
                             ? paletteColor(static_cast<std::uint8_t>(st.textFillColor))
                             : kBackgroundColor;
        const Vec2 m = L.text.center;
        bg.rings.push_back({m - ux * hw - uy * hh, m + ux * hw - uy * hh, m + ux * hw + uy * hh,
                            m - ux * hw + uy * hh});
        out.items.emplace_back(std::move(bg));
    }
    if (L.text.valid())
        text::buildTextDisplay(doc, textEntity(e, st, L.text, textStyleOf(st)), ctx, out);
    if (auto block = blockContent(doc, e, ctx.annoScale))
        geom::buildDisplay(doc, *block, ctx, out);
}

void registerDisplay() {
    geom::DisplayRegistry::set(EntityKind::Dimension, buildDimensionDisplay);
    geom::DisplayRegistry::set(EntityKind::Leader, buildDimensionDisplay);
    geom::DisplayRegistry::set(EntityKind::MLeader, buildDimensionDisplay);
}

Result<std::vector<Entity>> explodeDimension(const Document& doc, const Entity& e,
                                             double tolerance) {
    if (kind(e) != EntityKind::Dimension && kind(e) != EntityKind::Leader &&
        kind(e) != EntityKind::MLeader)
        return makeError(ErrorCode::InvalidArgument, "not a dimension, leader or multileader");
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
    if (auto block = blockContent(doc, e, 0))
        out.push_back(std::move(*block));
    return out;
}

Result<std::vector<Entity>> mleaderAsLeaders(const Document& doc, const Entity& e) {
    const auto* m = entityAs<MLeaderData>(e);
    if (!m)
        return makeError(ErrorCode::InvalidArgument, "not a multileader");
    const DimStyle& st = styleForEntity(doc, e);
    std::vector<Entity> out;
    for (const auto& line : m->lines) {
        if (line.size() < 2)
            continue;
        LeaderData l;
        l.vertices = line;
        l.style = m->style;
        l.textPos = line.back();
        Entity n;
        n.common = e.common;
        n.common.id = {};
        n.data = Indirect<LeaderData>(std::move(l));
        out.push_back(std::move(n));
    }
    // The dogleg is not a LEADER vertex: it joins the landing to the note.
    const Layout L = layoutOf(doc, e, 0.01);
    if (m->dogleg && distance(m->landing, m->contentPos) > 1e-9) {
        Entity n;
        n.common = commonFor(e, st, Role::DimLine);
        n.data = LineData{m->landing, m->contentPos};  // no arrowhead, so a plain line
        out.push_back(std::move(n));
    }
    if (L.text.valid())
        out.push_back(textEntity(e, st, L.text, textStyleOf(st)));
    if (auto block = blockContent(doc, e, 0))
        out.push_back(std::move(*block));
    return out;
}

}  // namespace tbcad::dim
