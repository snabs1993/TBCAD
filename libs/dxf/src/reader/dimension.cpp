// DXF import: DIMENSION. The definition points become DimensionData; the anonymous "*D" block
// that holds the drawn dimension (lines, arrowheads, text) is kept as ProxyData graphics plus
// loose text entities so the dimension displays before the dimension generator exists.
#include <algorithm>
#include <cmath>

#include "importer.h"
#include "tbcad/dxf/encoding.h"
#include "tbcad/geom/entity.h"

namespace tbcad::dxf::detail {

namespace {

constexpr double kDeg = kPi / 180.0;
constexpr int kMaxInsertDepth = 4;

// Appends the points of the arc from a to b described by `bulge` (excluding a, including b).
void bulgeArc(Vec2 a, Vec2 b, double bulge, std::vector<Vec2>& out) {
    if (std::abs(bulge) < 1e-12 || tol::isZeroLength(b - a)) {
        out.push_back(b);
        return;
    }
    double sweep = 4 * std::atan(bulge);
    Vec2 chord = b - a;
    double c = length(chord);
    double radius = c / (2 * std::sin(std::abs(sweep) / 2));
    Vec2 mid = (a + b) * 0.5;
    double h = radius * std::cos(sweep / 2);
    // For a counter-clockwise arc (bulge > 0) the center is to the left of the chord.
    Vec2 center = mid + perp(normalized(chord)) * (bulge > 0 ? h : -h);
    double start = std::atan2(a.y - center.y, a.x - center.x);
    int steps = std::max(2, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 18))));
    for (int i = 1; i <= steps; ++i) {
        double t = start + sweep * i / steps;
        out.push_back(i == steps
                          ? b
                          : Vec2{center.x + radius * std::cos(t), center.y + radius * std::sin(t)});
    }
}

}  // namespace

bool Importer::convDimension(const Record& r, std::vector<Entity>& out, BlockId /*owner*/) {
    const std::int64_t flags = r.integer(70);
    DimensionData d;
    bool isLength = true;
    switch (flags & 7) {
        case 0:
            d.kind = DimKind::Linear;
            d.rotation = r.num(50) * kDeg;
            d.defPts = {P(r, 13, 23), P(r, 14, 24), P(r, 10, 20), Vec2{}};
            break;
        case 1:
            d.kind = DimKind::Aligned;
            d.defPts = {P(r, 13, 23), P(r, 14, 24), P(r, 10, 20), Vec2{}};
            break;
        case 2:
            d.kind = DimKind::Angular2Line;
            d.defPts = {P(r, 13, 23), P(r, 14, 24), P(r, 15, 25), P(r, 10, 20)};
            isLength = false;
            break;
        case 3:
            d.kind = DimKind::Diameter;
            d.defPts = {P(r, 10, 20), P(r, 15, 25), Vec2{}, Vec2{}};
            break;
        case 4:
            d.kind = DimKind::Radius;
            d.defPts = {P(r, 10, 20), P(r, 15, 25), Vec2{}, Vec2{}};
            break;
        case 5:
            d.kind = DimKind::Angular3Point;
            d.defPts = {P(r, 13, 23), P(r, 14, 24), P(r, 15, 25), P(r, 10, 20)};
            isLength = false;
            break;
        default:
            // Ordinate dimensions have no DimensionData form: only the drawn graphics are kept.
            report_.info(code::kBadEntity,
                         "DIMENSION: ordinate dimensions are kept as drawn graphics only", r.line);
            Entity none;
            if (opts_.dimensionGraphics)
                appendDimensionGraphics(r, none, out);
            return true;
    }
    d.textPos = P(r, 11, 21);
    d.textPosUser = (flags & 128) != 0;
    std::string text = stripMText(dec(r.raw(1)));
    if (text != "<>")
        d.textOverride = text;
    d.style = dimStyleFor(r.raw(3));
    d.measurement = r.num(42) * (isLength ? k_ : 1.0);
    if (!(d.measurement > 0) || !std::isfinite(d.measurement)) {
        // Many writers omit the measurement; derive it from the definition points.
        switch (d.kind) {
            case DimKind::Linear: {
                Vec2 dir{std::cos(d.rotation), std::sin(d.rotation)};
                d.measurement = std::abs(dot(d.defPts[1] - d.defPts[0], dir));
                break;
            }
            case DimKind::Aligned:
                d.measurement = distance(d.defPts[0], d.defPts[1]);
                break;
            case DimKind::Radius:
            case DimKind::Diameter:
                d.measurement = distance(d.defPts[0], d.defPts[1]);
                break;
            default:
                d.measurement = 0;
                break;
        }
    }
    Entity e;
    e.data = Indirect<DimensionData>(std::move(d));
    if (opts_.dimensionGraphics)
        appendDimensionGraphics(r, e, out);
    out.insert(out.begin(), std::move(e));
    return true;
}

void Importer::appendDimensionGraphics(const Record& r, const Entity& /*dim*/,
                                       std::vector<Entity>& out) {
    auto it = blockByFold_.end();
    {
        std::string name = dec(r.raw(2));
        for (char& c : name) {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        it = blockByFold_.find(name);
    }
    const BlockDef* def = it == blockByFold_.end() ? nullptr : doc_->block(it->second);
    if (!def || def->entities.empty()) {
        report_.info(code::kNoDimensionBlock,
                     "DIMENSION: the drawn dimension block \"" + dec(r.raw(2)) +
                         "\" is missing; no graphics kept",
                     r.line);
        return;
    }

    const EntityCommon dc = commonOf(r);
    Entity styleHolder;
    styleHolder.common = dc;
    styleHolder.data = PointData{};
    geom::DisplayContext ctx;
    ctx.chordTolMm = 0.01;
    DisplayStyle dimStyle = geom::resolveStyle(*doc_, styleHolder, ctx);
    ctx.byBlockColor = dimStyle.color;
    ctx.byBlockLineweightMm = dimStyle.lineweight;
    ctx.byBlockPattern = dimStyle.pattern;

    std::vector<Entity> shapes, texts;
    struct Walker {
        Importer& self;
        std::vector<Entity>& shapes;
        std::vector<Entity>& texts;
        void run(const BlockDef& block, const std::optional<Transform2>& t, int depth) {
            for (EntityId id : block.entities) {
                const Entity* e = self.doc_->find(id);
                if (!e)
                    continue;
                if (const auto* ins = entityAs<InsertData>(*e)) {
                    const BlockDef* nested = self.doc_->block(ins->block);
                    if (!nested || depth >= kMaxInsertDepth)
                        continue;
                    Transform2 local =
                        compose(Transform2::translate(ins->position),
                                compose(Transform2::rotate(ins->rotation),
                                        compose(Transform2::scale(ins->scale.x, ins->scale.y),
                                                Transform2::translate(-nested->base))));
                    run(*nested, t ? compose(*t, local) : local, depth + 1);
                    continue;
                }
                Entity c = *e;
                if (t) {
                    tbcad::Result<Entity> moved = geom::transformed(c, *t);
                    if (!moved)
                        continue;
                    c = std::move(moved.value());
                }
                EntityKind k = kind(c);
                (k == EntityKind::Text || k == EntityKind::MText ? texts : shapes)
                    .push_back(std::move(c));
            }
        }
    };
    Walker{*this, shapes, texts}.run(*def, std::nullopt, 0);

    ProxyData proxy;
    proxy.originalType = "DIMENSION";
    for (Entity& s : shapes) {
        s.common.layer = dc.layer;
        if (s.common.color.kind == ColorKind::ByBlock)
            s.common.color = dc.color;
        if (const auto* h = entityAs<HatchData>(s)) {
            DisplayFill fill;
            fill.style = geom::resolveStyle(*doc_, s, ctx);
            for (const HatchLoop& loop : h->loops) {
                std::vector<Vec2> ring;
                for (const HatchEdge& edge : loop.edges) {
                    if (const auto* pe = std::get_if<HatchPolylineEdge>(&edge)) {
                        for (std::size_t i = 0; i < pe->v.size(); ++i) {
                            if (i == 0)
                                ring.push_back(pe->v[0].p);
                            std::size_t j = i + 1;
                            if (j < pe->v.size())
                                bulgeArc(pe->v[i].p, pe->v[j].p, pe->v[i].bulge, ring);
                            else if (pe->closed && pe->v.size() > 1)
                                bulgeArc(pe->v[i].p, pe->v[0].p, pe->v[i].bulge, ring);
                        }
                    } else if (const auto* le = std::get_if<HatchLineEdge>(&edge)) {
                        if (ring.empty())
                            ring.push_back(le->a);
                        ring.push_back(le->b);
                    }
                }
                if (ring.size() >= 3)
                    fill.rings.push_back(std::move(ring));
            }
            if (!fill.rings.empty())
                proxy.graphics.items.emplace_back(std::move(fill));
        } else {
            geom::buildDisplay(*doc_, s, ctx, proxy.graphics);
        }
    }
    if (!proxy.graphics.items.empty()) {
        Entity pe;
        pe.common = dc;
        pe.data = Indirect<ProxyData>(std::move(proxy));
        out.push_back(std::move(pe));
    }
    for (Entity& t : texts) {
        t.common.layer = dc.layer;
        if (t.common.color.kind == ColorKind::ByBlock)
            t.common.color = dc.color;
        out.push_back(std::move(t));
    }
}

}  // namespace tbcad::dxf::detail
