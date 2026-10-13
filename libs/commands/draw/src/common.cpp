#include "common.h"

namespace tbcad::draw::detail {

std::string promptWith(std::string_view text, const std::vector<std::string>& keywords) {
    std::string s(text);
    if (keywords.empty())
        return s;
    s += " [";
    for (std::size_t i = 0; i < keywords.size(); ++i) {
        if (i)
            s += '/';
        s += keywords[i];
    }
    s += ']';
    return s;
}

// New objects take the layer and the current colour, linetype and lineweight from the header.
void applyCurrentProperties(const Document& doc, Entity& e) {
    tbcad::applyCurrentProperties(doc.header(), e);
}

void previewEntity(cmd::CommandContext& ctx, Entity e, DisplayList& out) {
    applyCurrentProperties(ctx.doc(), e);
    geom::DisplayContext dc;
    const double scale = ctx.host().view().scale;
    if (scale > 0)
        dc.pixelSizeMm = scale;
    try {
        geom::buildDisplay(ctx.doc(), e, dc, out);
    } catch (...) {
        // a half-defined preview (zero radius ...) simply shows nothing
    }
}

void previewLine(cmd::CommandContext& ctx, Vec2 a, Vec2 b, DisplayList& out) {
    previewEntity(ctx, makeEntity(LineData{a, b}), out);
}

bool addEntity(cmd::CommandContext& ctx, EntityData data) {
    Entity e{{}, std::move(data)};
    applyCurrentProperties(ctx.doc(), e);
    auto id = ctx.tx().add(std::move(e));
    if (!id) {
        ctx.message(id.error().message);
        return false;
    }
    return true;
}

double signedAngle(double from, double to) {
    double d = normalizeAngle(to - from);
    return d > kPi ? d - kTwoPi : d;
}

std::optional<ArcData> arcBySweep(Vec2 center, Vec2 startPoint, double sweep) {
    const double r = distance(center, startPoint);
    const double a0 = std::atan2(startPoint.y - center.y, startPoint.x - center.x);
    if (!(r > tol::kLinear) || std::abs(sweep) < 1e-9 || std::abs(sweep) >= kTwoPi - 1e-9)
        return std::nullopt;
    ArcData d;
    d.center = center;
    d.radius = r;
    if (sweep > 0) {
        d.startAngle = normalizeAngle(a0);
        d.endAngle = normalizeAngle(a0 + sweep);
    } else {
        d.startAngle = normalizeAngle(a0 + sweep);
        d.endAngle = normalizeAngle(a0);
    }
    return d;
}

std::optional<double> bulgeFromTangent(Vec2 a, Vec2 b, double tangent) {
    const Vec2 chord = b - a;
    if (tol::isZeroLength(chord))
        return std::nullopt;
    const double alpha = signedAngle(tangent, std::atan2(chord.y, chord.x));
    if (std::abs(alpha) > kPi - 1e-6)
        return std::nullopt;
    return std::tan(alpha / 2);
}

std::optional<double> bulgeFromIncluded(Vec2 a, Vec2 b, double included) {
    if (tol::isZeroLength(b - a) || std::abs(included) < 1e-9 ||
        std::abs(included) >= kTwoPi - 1e-9)
        return std::nullopt;
    return std::tan(included / 4);
}

double endTangent(Vec2 a, Vec2 b, double bulge) {
    const Vec2 chord = b - a;
    const double phi = std::atan2(chord.y, chord.x);
    return phi + 2 * std::atan(bulge);
}

std::optional<Vec2> centerFromIncluded(Vec2 a, Vec2 b, double included) {
    const Vec2 chord = b - a;
    const double c = length(chord);
    if (!(c > tol::kLinear) || std::abs(included) < 1e-9 || std::abs(included) >= kTwoPi - 1e-9)
        return std::nullopt;
    const Vec2 u = chord / c;
    const double h = (c / 2) / std::tan(included / 2);
    return lerp(a, b, 0.5) + perp(u) * h;
}

std::optional<Continuation> lastLineOrArc(const Document& doc) {
    const Entity* best = nullptr;
    doc.forEachEntity([&](const Entity& e) {
        const EntityKind k = kind(e);
        if (k != EntityKind::Line && k != EntityKind::Arc)
            return;
        if (doc.ownerBlock(e.common.id) || !doc.effective(e.common).live())
            return;
        if (!best || e.common.id.value > best->common.id.value)
            best = &e;
    });
    if (!best)
        return std::nullopt;
    if (const auto* l = std::get_if<LineData>(&best->data)) {
        const Vec2 d = l->b - l->a;
        return Continuation{l->b, std::atan2(d.y, d.x)};
    }
    const auto& a = std::get<ArcData>(best->data);
    const Vec2 end = a.center + Vec2{std::cos(a.endAngle), std::sin(a.endAngle)} * a.radius;
    return Continuation{end, a.endAngle + kPi / 2};
}

}  // namespace tbcad::draw::detail
