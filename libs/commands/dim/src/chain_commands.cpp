// DIMCONTINUE, DIMBASELINE, DIMSPACE and DIMTEDIT: chains of linear dimensions and dimension text
// editing.
#include <algorithm>
#include <cmath>

#include "dim_internal.h"
#include "tbcad/core/dim_assoc.h"
#include "tbcad/dim/dim.h"

namespace tbcad::dimcmd {

namespace {

bool isLengthDim(const Entity& e) {
    const auto* d = entityAs<DimensionData>(e);
    return d && (d->kind == DimKind::Linear || d->kind == DimKind::Aligned);
}

bool isAnyDim(const Entity& e) {
    return kind(e) == EntityKind::Dimension;
}

// The most recently drawn linear or aligned dimension of model space.
EntityId lastLengthDim(const Document& doc) {
    const auto& ms = doc.modelSpace();
    for (auto it = ms.rbegin(); it != ms.rend(); ++it) {
        const Entity* e = doc.find(*it);
        if (e && isLengthDim(*e))
            return *it;
    }
    return EntityId{};
}

// Unit vector across the dimension line and the side (+1/-1) the line is on.
Vec2 normalOf(const DimensionData& d) {
    return Vec2{-std::sin(d.rotation), std::cos(d.rotation)};
}

double sideOf(const DimensionData& d) {
    return dot(d.defPts[2] - d.defPts[0], normalOf(d)) >= 0 ? 1.0 : -1.0;
}

class ChainCommand final : public cmd::Command {
public:
    ChainCommand(std::string name, std::string alias, bool baseline, std::string description)
        : name_(std::move(name)),
          alias_(std::move(alias)),
          baseline_(baseline),
          description_(std::move(description)) {}

    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return {alias_}; }
    std::string_view description() const override { return description_; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        EntityId baseId = lastLengthDim(ctx.doc());
        for (;;) {
            if (!baseId) {
                cmd::EntityOptions eo;
                eo.filter = isLengthDim;
                auto o = co_await ctx.getEntity("Select base dimension", eo);
                if (!o.ok())
                    co_return;
                baseId = o.id;
            }
            const Entity* be = ctx.doc().find(baseId);
            const auto* base = be ? entityAs<DimensionData>(*be) : nullptr;
            if (!base || !isLengthDim(*be)) {
                ctx.message("Select a linear or aligned dimension.");
                co_return;
            }
            DimensionData prev = *base;
            const DimStyle& st = dim::styleOf(ctx.doc(), prev.style);
            const double k = st.overallScale > 0 ? st.overallScale : 1.0;
            const double spacing = st.textHeight * 2.5 * k;

            Vec2 origin = baseline_ ? prev.defPts[0] : prev.defPts[1];
            auto* c = &ctx;
            const bool baseline = baseline_;
            auto build = [=](Vec2 second) {
                DimensionData d = prev;
                d.defPts[0] = origin;
                d.defPts[1] = second;
                d.defPts[2] = baseline ? prev.defPts[2] + normalOf(prev) * (sideOf(prev) * spacing)
                                       : prev.defPts[2];
                d.textPosUser = false;
                d.textOverride.clear();
                return d;
            };
            cmd::PointOptions po;
            po.base = prev.defPts[1];
            po.keywords = {"Select"};
            po.preview = [c, build](Vec2 cursor, DisplayList& out) {
                DimensionData d = build(cursor);
                dim::refresh(c->doc(), d);
                Entity e;
                e.data = Indirect<DimensionData>(std::move(d));
                previewEntity(*c, std::move(e), out);
            };
            auto next =
                co_await ctx.getPoint("Specify second extension line origin or [Select]", po);
            if (next.status == Status::Keyword) {
                baseId = EntityId{};
                continue;
            }
            if (!next.ok())
                co_return;
            DimensionData d = build(next.point);
            if (distance(d.defPts[0], d.defPts[1]) < tol::kLinear) {
                ctx.message("The extension line origins coincide.");
                continue;
            }
            linkDimension(ctx.doc(), d, ctx.tx().defaultOwner());
            dim::refresh(ctx.doc(), d);
            Entity e;
            e.data = Indirect<DimensionData>(std::move(d));
            applyAnnotativeDefault(ctx.doc().header(), e);
            ctx.setUndoLabel(std::string(ctx.commandName()));
            auto id = ctx.tx().add(std::move(e));
            if (!id) {
                ctx.message(id.error().message);
                co_return;
            }
            // The next one chains from the dimension just made.
            baseId = id.value();
        }
    }

private:
    std::string name_, alias_;
    bool baseline_;
    std::string description_;
};

// Signed distance of a length dimension's line along `n` (a unit vector across the dimension).
double lineOffset(const DimensionData& d, Vec2 n) {
    return dot(d.defPts[2], n);
}

class SpaceCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMSPACE"; }
    std::vector<std::string> defaultAliases() const override { return {"DSP"}; }
    std::string_view description() const override {
        return "Even out the spacing between parallel linear dimensions";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = isLengthDim;
        auto base = co_await ctx.getEntity("Select base dimension", eo);
        if (!base.ok())
            co_return;
        const Entity* be = ctx.doc().find(base.id);
        const auto* b = be ? entityAs<DimensionData>(*be) : nullptr;
        if (!b)
            co_return;
        const DimensionData baseDim = *b;
        const Vec2 n = normalOf(baseDim);
        const double baseOff = lineOffset(baseDim, n);

        cmd::SelectionOptions so;
        so.allowPreselection = false;
        so.filter = isLengthDim;
        auto sel = co_await ctx.getSelection("Select dimensions to space", so);
        if (!sel.ok())
            co_return;

        struct Item {
            EntityId id;
            double off;
        };
        std::vector<Item> items;
        for (EntityId id : sel.ids) {
            if (id == base.id)
                continue;
            const Entity* e = ctx.doc().find(id);
            const auto* d = e ? entityAs<DimensionData>(*e) : nullptr;
            if (!d || !isLengthDim(*e))
                continue;
            const Vec2 dn = normalOf(*d);
            if (std::abs(cross(dn, n)) > 1e-6)
                continue;  // not parallel to the base
            const double sign = dot(dn, n) >= 0 ? 1.0 : -1.0;
            items.push_back({id, sign * lineOffset(*d, dn)});
        }
        if (items.empty()) {
            ctx.message("No dimensions parallel to the base dimension were selected.");
            co_return;
        }

        const DimStyle& st = dim::styleOf(ctx.doc(), baseDim.style);
        const double autoGap = 2.0 * st.textHeight * (st.overallScale > 0 ? st.overallScale : 1.0);
        cmd::DistanceOptions dopt;
        dopt.keywords = {"Auto"};
        dopt.defaultValue = autoGap;
        auto gap = co_await ctx.getDistance("Enter value or [Auto] <Auto>", dopt);
        double spacing = autoGap;
        if (gap.status == Status::Ok)
            spacing = gap.value;
        else if (!(gap.status == Status::Keyword && gap.keyword == "Auto"))
            co_return;

        std::vector<Item> above, below;
        for (const Item& it : items)
            (it.off >= baseOff ? above : below).push_back(it);
        std::sort(above.begin(), above.end(),
                  [](const Item& a, const Item& c) { return a.off < c.off; });
        std::sort(below.begin(), below.end(),
                  [](const Item& a, const Item& c) { return a.off > c.off; });

        ctx.setUndoLabel("DIMSPACE");
        auto place = [&](const std::vector<Item>& list, double dir) {
            for (std::size_t i = 0; i < list.size(); ++i) {
                const double target = baseOff + dir * spacing * static_cast<double>(i + 1);
                const double delta = target - list[i].off;
                (void)ctx.tx().modify(list[i].id, [&](Entity& ent) {
                    auto* d = entityAs<DimensionData>(ent);
                    if (!d)
                        return;
                    const Vec2 dn = normalOf(*d);
                    const double sign = dot(dn, n) >= 0 ? 1.0 : -1.0;
                    const Vec2 move = dn * (sign * delta);
                    d->defPts[2] = d->defPts[2] + move;
                    if (d->textPosUser)
                        d->textPos = d->textPos + move;
                    dim::refresh(ctx.doc(), *d);
                });
            }
        };
        place(above, 1.0);
        place(below, -1.0);
        ctx.message("Spaced " + std::to_string(items.size()) + " dimensions.");
    }
};

// QDIM: one chain of linear dimensions through the key points of the selected objects.
class QuickDimCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "QDIM"; }
    std::string_view description() const override {
        return "Dimension the key points of the selected objects in one go";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.allowPreselection = true;
        auto sel = co_await ctx.getSelection("Select geometry to dimension", so);
        if (!sel.ok())
            co_return;
        std::vector<Vec2> pts;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            if (const auto* l = entityAs<LineData>(*e)) {
                pts.push_back(l->a);
                pts.push_back(l->b);
            } else if (const auto* p = entityAs<PolylineData>(*e)) {
                for (const auto& v : p->v)
                    pts.push_back(v.p);
            } else if (const auto* c = entityAs<CircleData>(*e)) {
                pts.push_back(c->center);
            } else if (const auto* a = entityAs<ArcData>(*e)) {
                pts.push_back(a->center);
            } else if (const auto* pt = entityAs<PointData>(*e)) {
                pts.push_back(pt->p);
            }
        }
        BBox2 box;
        for (const Vec2& p : pts)
            box.expand(p);
        if (pts.size() < 2 || box.empty()) {
            ctx.message("Nothing to dimension.");
            co_return;
        }
        bool baseline = false;
        cmd::PointResult at;
        for (;;) {
            cmd::PointOptions po;
            po.keywords = {"Continuous", "Baseline", "Radius", "Diameter"};
            at = co_await ctx.getPoint(
                std::string("Specify dimension line position or [Continuous/Baseline/Radius/"
                            "Diameter] <") +
                    (baseline ? "Baseline" : "Continuous") + ">",
                po);
            if (at.status != cmd::Status::Keyword)
                break;
            if (at.keyword == "Continuous" || at.keyword == "Baseline") {
                baseline = at.keyword == "Baseline";
                continue;
            }
            // Radius and Diameter dimension every circle and arc of the selection at once.
            const bool radius = at.keyword == "Radius";
            int made = 0;
            for (EntityId id : sel.ids) {
                const Entity* e = ctx.doc().find(id);
                if (!e)
                    continue;
                Vec2 centre{};
                double r = 0;
                if (const auto* c = entityAs<CircleData>(*e)) {
                    centre = c->center;
                    r = c->radius;
                } else if (const auto* a = entityAs<ArcData>(*e)) {
                    centre = a->center;
                    r = a->radius;
                } else {
                    continue;
                }
                const Vec2 dir{std::cos(kPi / 4), std::sin(kPi / 4)};
                DimensionData d;
                d.kind = radius ? DimKind::Radius : DimKind::Diameter;
                if (radius) {
                    d.defPts = {centre, centre + dir * r, Vec2{}, Vec2{}};
                } else {
                    d.defPts = {centre - dir * r, centre + dir * r, Vec2{}, Vec2{}};
                }
                dim::refresh(ctx.doc(), d);
                if (!addDim(ctx, std::move(d)))
                    co_return;
                ++made;
            }
            ctx.message(made ? std::to_string(made) + " dimensions created."
                             : "No circles or arcs in the selection.");
            co_return;
        }
        if (!at.ok())
            co_return;
        // Outside the box vertically: horizontal dimensions; otherwise vertical ones.
        const bool horizontal = at.point.y > box.max.y || at.point.y < box.min.y;
        const double rotation = horizontal ? 0.0 : kPi / 2;
        auto key = [&](Vec2 p) { return horizontal ? p.x : p.y; };
        std::sort(pts.begin(), pts.end(), [&](Vec2 a, Vec2 b) { return key(a) < key(b); });
        std::vector<Vec2> uniq;
        for (const Vec2& p : pts)
            if (uniq.empty() || std::abs(key(p) - key(uniq.back())) > tol::kLinear)
                uniq.push_back(p);
        if (uniq.size() < 2) {
            ctx.message("The key points are all at the same place along that direction.");
            co_return;
        }
        const DimStyle& st = dim::styleOf(ctx.doc(), ctx.doc().header().currentDimStyle);
        const double k = st.overallScale > 0 ? st.overallScale : 1.0;
        const double spacing = st.textHeight * 2.5 * k;
        const Vec2 normal{-std::sin(rotation), std::cos(rotation)};
        const double side = dot(at.point - box.center(), normal) >= 0 ? 1.0 : -1.0;
        for (std::size_t i = 0; i + 1 < uniq.size(); ++i) {
            DimensionData d;
            d.kind = DimKind::Linear;
            d.rotation = rotation;
            if (baseline) {
                // Every dimension starts at the first key point; each sits one step further out.
                d.defPts = {uniq[0], uniq[i + 1], at.point + normal * (side * spacing * double(i)),
                            Vec2{}};
            } else {
                d.defPts = {uniq[i], uniq[i + 1], at.point, Vec2{}};
            }
            dim::refresh(ctx.doc(), d);
            if (!addDim(ctx, std::move(d)))
                co_return;
        }
        ctx.message(std::to_string(uniq.size() - 1) + " dimensions created.");
    }
};

class TextEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMTEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"DTE"}; }
    std::string_view description() const override {
        return "Move dimension text, or reset it to its automatic place";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = isAnyDim;
        auto o = co_await ctx.getEntity("Select dimension", eo);
        if (!o.ok())
            co_return;
        const Entity* e = ctx.doc().find(o.id);
        const auto* d0 = e ? entityAs<DimensionData>(*e) : nullptr;
        if (!d0)
            co_return;
        const DimensionData original = *d0;
        const EntityId id = o.id;

        cmd::PointOptions po;
        po.keywords = {"Home"};
        auto* c = &ctx;
        po.preview = [c, original](Vec2 cursor, DisplayList& out) {
            DimensionData d = original;
            d.textPos = cursor;
            d.textPosUser = true;
            dim::refresh(c->doc(), d);
            Entity e;
            e.data = Indirect<DimensionData>(std::move(d));
            previewEntity(*c, std::move(e), out);
        };
        auto where = co_await ctx.getPoint("Specify new text location or [Home]", po);
        const bool home = where.status == Status::Keyword && where.keyword == "Home";
        if (!where.ok() && !home)
            co_return;
        ctx.setUndoLabel("DIMTEDIT");
        auto r = ctx.tx().modify(id, [&](Entity& ent) {
            auto* d = entityAs<DimensionData>(ent);
            if (!d)
                return;
            if (home) {
                d->textPosUser = false;
            } else {
                d->textPos = where.point;
                d->textPosUser = true;
            }
            dim::refresh(ctx.doc(), *d);
        });
        if (!r)
            ctx.message(r.error().message);
    }
};

class EditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"DED"}; }
    std::string_view description() const override {
        return "Replace the text of dimensions, return them to the measured text and place, "
               "rotate the text or lean the extension lines";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::KeywordOptions ko;
        ko.keywords = {"Home", "New", "Rotate", "Oblique"};
        ko.defaultKeyword = "Home";
        auto k = co_await ctx.getKeyword(
            "Enter type of dimension editing [Home/New/Rotate/Oblique] <Home>", ko);
        if (!k.ok())
            co_return;
        const bool home = k.keyword == "Home";
        const bool rotate = k.keyword == "Rotate";
        const bool oblique = k.keyword == "Oblique";
        std::string text;
        double angle = 0;
        if (k.keyword == "New") {
            cmd::StringOptions so;
            so.defaultValue = "<>";
            auto s = co_await ctx.getString("Dimension text, <> is the measurement", so);
            if (!s.ok())
                co_return;
            text = s.text == "<>" ? std::string() : s.text;
        } else if (rotate) {
            auto a = co_await ctx.getAngle("Specify angle for dimension text, 0 = default",
                                           cmd::AngleOptions{.defaultValue = 0.0});
            if (!a.ok())
                co_return;
            angle = a.value;
        } else if (oblique) {
            auto a =
                co_await ctx.getAngle("Enter obliquing angle of the extension lines, Enter = none",
                                      cmd::AngleOptions{.defaultValue = std::nan("")});
            if (a.status == cmd::Status::Cancel)
                co_return;
            angle = a.ok() ? a.value : std::nan("");
        }
        cmd::SelectionOptions so;
        so.allowPreselection = false;
        so.filter = isAnyDim;
        auto sel = co_await ctx.getSelection("Select dimensions", so);
        if (!sel.ok())
            co_return;
        ctx.setUndoLabel("DIMEDIT");
        std::size_t skipped = 0;
        for (EntityId id : sel.ids) {
            bool done = true;
            auto r = ctx.tx().modify(id, [&](Entity& ent) {
                auto* d = entityAs<DimensionData>(ent);
                if (!d)
                    return;
                if (home) {
                    d->textPosUser = false;
                    d->textOverride.clear();
                    d->textRotated = false;
                    d->textRotation = 0;
                } else if (rotate) {
                    d->textRotated = std::abs(angle) > 1e-9;
                    d->textRotation = d->textRotated ? angle : 0;
                } else if (oblique) {
                    if (d->kind != DimKind::Linear && d->kind != DimKind::Aligned) {
                        done = false;
                        return;
                    }
                    // The angle is measured from the X axis; the lean is from the perpendicular
                    // of the dimension line.
                    double skew = 0;
                    if (!std::isnan(angle)) {
                        const Vec2 u = d->kind == DimKind::Linear
                                           ? Vec2{std::cos(d->rotation), std::sin(d->rotation)}
                                           : d->defPts[1] - d->defPts[0];
                        const double perp = std::atan2(u.y, u.x) + kPi / 2;
                        skew = std::remainder(angle - perp, kPi);
                    }
                    if (std::abs(skew) > 85.0 * kPi / 180) {
                        done = false;
                        return;
                    }
                    d->oblique = skew;
                } else {
                    d->textOverride = text;
                }
                dim::refresh(ctx.doc(), *d);
            });
            if (!r)
                ctx.message(r.error().message);
            else if (!done)
                ++skipped;
        }
        if (skipped)
            ctx.message(std::to_string(skipped) +
                        " dimension(s) skipped: only linear and aligned dimensions lean, and the "
                        "angle must stay within 85 degrees of the dimension's own perpendicular.");
    }
};

// DIMBREAK [DBR]: opens gaps in the dimension and extension lines where other objects cross them,
// or closes the gaps again. The gaps are stored with the dimension (DimensionData::breaks).
class BreakCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMBREAK"; }
    std::vector<std::string> defaultAliases() const override { return {"DBR"}; }
    std::string_view description() const override {
        return "Break dimension and extension lines where other objects cross them";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::SelectionOptions so;
        so.filter = isAnyDim;
        auto dims = co_await ctx.getSelection("Select dimensions to add or remove breaks", so);
        if (!dims.ok() || dims.ids.empty())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Auto", "Remove"};
        ko.defaultKeyword = "Auto";
        auto mode = co_await ctx.getKeyword("Break [Auto/Remove] <Auto>", ko);
        if (!mode.ok())
            co_return;

        std::vector<EntityId> cutters;
        if (mode.keyword == "Auto") {
            cmd::SelectionOptions co;
            co.allowPreselection = false;
            co.keywords = {"All"};
            auto picked = co_await ctx.getSelection(
                "Select objects to break dimensions with or [All] <All>", co);
            if (picked.status == Status::Cancel)
                co_return;
            if (picked.ok() && !picked.ids.empty())
                cutters = picked.ids;
            else
                cutters.assign(ctx.doc().modelSpace().begin(), ctx.doc().modelSpace().end());
        }

        ctx.setUndoLabel("DIMBREAK");
        std::size_t changed = 0, gaps = 0;
        for (EntityId id : dims.ids) {
            const Entity* e = ctx.doc().find(id);
            const auto* d0 = e ? entityAs<DimensionData>(*e) : nullptr;
            if (!d0)
                continue;
            std::vector<DimBreak> found;
            if (mode.keyword == "Auto") {
                DimensionData bare = *d0;
                bare.breaks.clear();
                const dim::Layout L = dim::layoutDimension(ctx.doc(), bare);
                const DimStyle& st = dim::styleOf(ctx.doc(), d0->style);
                const double gap = 1.5 * st.arrowSize * (st.overallScale > 0 ? st.overallScale : 1);
                for (const dim::Layout::Line& line : L.lines) {
                    if (line.pts.size() != 2 || line.role == dim::Role::Text)
                        continue;
                    const Vec2 a = line.pts[0], dir = line.pts[1] - a;
                    const double len = std::hypot(dir.x, dir.y);
                    if (!(len > 1e-9))
                        continue;
                    Entity seg;
                    seg.data = LineData{line.pts[0], line.pts[1]};
                    for (EntityId cid : cutters) {
                        const Entity* c = ctx.doc().find(cid);
                        if (!c || cid == id || kind(*c) == EntityKind::Dimension)
                            continue;
                        std::vector<geom::EntityIntersection> hits;
                        if (!geom::intersectEntities(seg, *c, hits))
                            continue;
                        for (const geom::EntityIntersection& h : hits) {
                            const double t = h.tA;
                            if (t < 1e-6 || t > 1 - 1e-6)
                                continue;
                            const double half = gap / 2 / len;
                            DimBreak b;
                            b.a = a + dir * std::max(0.0, t - half);
                            b.b = a + dir * std::min(1.0, t + half);
                            if (std::find(found.begin(), found.end(), b) == found.end())
                                found.push_back(b);
                        }
                    }
                }
            }
            auto r = ctx.tx().modify(id, [&](Entity& ent) {
                auto* d = entityAs<DimensionData>(ent);
                if (!d)
                    return;
                if (mode.keyword == "Remove")
                    d->breaks.clear();
                else
                    d->breaks = found;
            });
            if (!r) {
                ctx.message(r.error().message);
                continue;
            }
            ++changed;
            gaps += found.size();
        }
        if (mode.keyword == "Remove")
            ctx.message(std::to_string(changed) + " dimension(s): breaks removed.");
        else
            ctx.message(std::to_string(gaps) + " break(s) in " + std::to_string(changed) +
                        " dimension(s).");
    }
};

class JogLineCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMJOGLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"DJL"}; }
    std::string_view description() const override {
        return "Add or remove a jog in the dimension line of a linear dimension";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using cmd::Status;
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) {
            const auto* d = entityAs<DimensionData>(e);
            return d && (d->kind == DimKind::Linear || d->kind == DimKind::Aligned);
        };
        auto pick = co_await ctx.getEntity("Select a linear or aligned dimension", eo);
        if (!pick.ok())
            co_return;
        const Entity* e = ctx.doc().find(pick.id);
        const auto* d0 = e ? entityAs<DimensionData>(*e) : nullptr;
        if (!d0)
            co_return;
        // The dimension line as drawn without jogs: the longest straight piece takes the jog.
        DimensionData bare = *d0;
        bare.jogs.clear();
        bare.breaks.clear();
        const dim::Layout L = dim::layoutDimension(ctx.doc(), bare);
        Vec2 a{}, b{};
        double best = 0;
        for (const dim::Layout::Line& line : L.lines) {
            if (line.role != dim::Role::DimLine || line.pts.size() != 2)
                continue;
            const double len = distance(line.pts[0], line.pts[1]);
            if (len > best) {
                best = len;
                a = line.pts[0];
                b = line.pts[1];
            }
        }
        if (!(best > 0)) {
            ctx.message("This dimension has no dimension line to put a jog in.");
            co_return;
        }
        cmd::PointOptions po;
        po.allowNone = true;
        po.keywords = {"Remove"};
        auto where = co_await ctx.getPoint(
            "Specify jog location on the dimension line or [Remove] (Enter = middle)", po);
        if (where.status == Status::Cancel)
            co_return;
        const bool remove = where.status == Status::Keyword;
        Vec2 at = (a + b) * 0.5;
        if (where.ok()) {
            const Vec2 u = (b - a) * (1 / best);
            const double s = std::clamp(dot(where.point - a, u), 0.0, best);
            at = a + u * s;
        }
        ctx.setUndoLabel("DIMJOGLINE");
        auto r = ctx.tx().modify(pick.id, [&](Entity& ent) {
            auto* d = entityAs<DimensionData>(ent);
            if (!d)
                return;
            if (remove)
                d->jogs.clear();
            else
                d->jogs.push_back(at);
        });
        if (!r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.message(remove ? "Jog lines removed." : "Jog line added.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeEditCommand() {
    return std::make_unique<EditCommand>();
}

std::unique_ptr<cmd::Command> makeContinueCommand() {
    return std::make_unique<ChainCommand>("DIMCONTINUE", "DCO", false,
                                          "Continue a chain of dimensions from the last one");
}

std::unique_ptr<cmd::Command> makeBaselineCommand() {
    return std::make_unique<ChainCommand>("DIMBASELINE", "DBA", true,
                                          "Stack dimensions from the same first extension line");
}

std::unique_ptr<cmd::Command> makeSpaceCommand() {
    return std::make_unique<SpaceCommand>();
}

std::unique_ptr<cmd::Command> makeQuickDimCommand() {
    return std::make_unique<QuickDimCommand>();
}

std::unique_ptr<cmd::Command> makeJogLineCommand() {
    return std::make_unique<JogLineCommand>();
}

std::unique_ptr<cmd::Command> makeBreakCommand() {
    return std::make_unique<BreakCommand>();
}

std::unique_ptr<cmd::Command> makeTextEditCommand() {
    return std::make_unique<TextEditCommand>();
}

}  // namespace tbcad::dimcmd
