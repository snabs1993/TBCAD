#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "tbcad/geom/geom.h"
#include "tbcad/transform/apply.h"
#include "tbcad/transform/grips.h"
#include "tbcad/transform/stretch.h"
#include "tbcad/transform/transform.h"

namespace tbcad::transform {

namespace {

using cmd::AngleOptions;
using cmd::CommandContext;
using cmd::DistanceOptions;
using cmd::KeywordOptions;
using cmd::PointOptions;
using cmd::SelectionOptions;
using cmd::Status;
using cmd::Task;

constexpr double kEps = 1e-12;

// Asks for the objects (pre-selection first); `out` stays empty when the user gave up.
Task askObjects(CommandContext& ctx, std::vector<EntityId>& out) {
    auto sel = co_await ctx.getSelection("Select objects");
    if (sel.ok())
        out = std::move(sel.ids);
}

void report(CommandContext& ctx, const ApplyReport& rep) {
    if (rep.failed > 0)
        ctx.message(std::to_string(rep.failed) +
                    " object(s) could not be transformed: " + rep.firstError);
}

// Rubber-band preview of the selection under a transform that depends on the cursor.
template <class MakeTransform>
cmd::PreviewFn previewOf(CommandContext& ctx, const std::vector<EntityId>& ids,
                         MakeTransform make) {
    return [&ctx, &ids, make](Vec2 cursor, DisplayList& out) {
        auto t = make(cursor);
        if (t)
            buildPreview(ctx.doc(), ids, *t, ctx.host().view().scale, out);
    };
}

// A zero displacement moves nothing and, for COPY, stacks duplicates on the originals.
bool noDisplacement(CommandContext& ctx, Vec2 d) {
    if (std::abs(d.x) > 1e-12 || std::abs(d.y) > 1e-12)
        return false;
    ctx.message("The displacement is zero: nothing to do.");
    return true;
}

// COPY > Array: a row of copies. A second point sets the spacing between neighbours, Fit makes
// it the position of the last copy.
Task arrayCopies(CommandContext& ctx, const std::vector<EntityId>& ids, Vec2 base) {
    cmd::IntOptions io;
    io.min = 2;
    io.max = 100000;
    io.defaultValue = 5;
    auto count = co_await ctx.getInteger("Enter the number of items to array <5>", io);
    if (!count.ok())
        co_return;
    const long long n = count.value;
    bool fit = false;
    for (;;) {
        PointOptions o;
        o.base = base;
        o.keywords = {"Fit"};
        o.allowNone = false;
        o.preview = [&ctx, &ids, base, n, &fit](Vec2 c, DisplayList& out) {
            const Vec2 step = fit ? (c - base) * (1.0 / static_cast<double>(n - 1)) : c - base;
            const double px = ctx.host().view().scale;
            if (n > 50) {  // keep the preview light: first and last copy only
                buildPreview(ctx.doc(), ids, Transform2::translate(step), px, out);
                buildPreview(ctx.doc(), ids, Transform2::translate(step * double(n - 1)), px, out);
                return;
            }
            for (long long k = 1; k < n; ++k)
                buildPreview(ctx.doc(), ids, Transform2::translate(step * double(k)), px, out);
        };
        auto to = co_await ctx.getPoint(
            fit ? "Specify second point to fit the array between" : "Specify second point or [Fit]",
            std::move(o));
        if (to.status == Status::Keyword && to.keyword == "Fit") {
            fit = true;
            continue;
        }
        if (!to.ok())
            co_return;
        const Vec2 step =
            fit ? (to.point - base) * (1.0 / static_cast<double>(n - 1)) : to.point - base;
        if (noDisplacement(ctx, step))
            continue;
        for (long long k = 1; k < n; ++k)
            report(ctx, applyTransform(ctx, ids, Transform2::translate(step * double(k)), true));
        co_return;
    }
}

class MoveCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MOVE"; }
    std::vector<std::string> defaultAliases() const override { return {"M"}; }
    std::string_view description() const override { return "Move objects"; }

    Task run(CommandContext& ctx) override {
        std::vector<EntityId> ids;
        co_await askObjects(ctx, ids);
        if (ids.empty())
            co_return;
        PointOptions bo;
        bo.keywords = {"Displacement"};
        auto base = co_await ctx.getPoint("Specify base point or [Displacement]", std::move(bo));
        if (base.status == Status::Keyword) {
            auto d = co_await ctx.getPoint("Specify displacement");
            if (d.ok() && !noDisplacement(ctx, d.point))
                report(ctx, applyTransform(ctx, ids, Transform2::translate(d.point), false));
            co_return;
        }
        if (!base.ok())
            co_return;
        PointOptions o;
        o.base = base.point;
        o.preview = previewOf(ctx, ids, [b = base.point](Vec2 c) -> std::optional<Transform2> {
            return Transform2::translate(c - b);
        });
        auto to = co_await ctx.getPoint("Specify second point or <use first point as displacement>",
                                        std::move(o));
        Vec2 delta;
        if (to.status == Status::None)
            delta = base.point;  // Enter: the base point is the displacement itself
        else if (to.ok())
            delta = to.point - base.point;
        else
            co_return;
        if (noDisplacement(ctx, delta))
            co_return;
        report(ctx, applyTransform(ctx, ids, Transform2::translate(delta), false));
    }
};

class CopyCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "COPY"; }
    std::vector<std::string> defaultAliases() const override { return {"CO", "CP"}; }
    std::string_view description() const override { return "Copy objects (repeats until Enter)"; }

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();  // Esc after some copies keeps them
        std::vector<EntityId> ids;
        co_await askObjects(ctx, ids);
        if (ids.empty())
            co_return;
        // COPYMODE, as in AutoCAD: Multiple repeats until Enter, Single makes one copy.
        bool single = ctx.host().setting("copy.mode").value_or("multiple") == "single";
        ctx.message(std::string("Current settings: Copy mode = ") +
                    (single ? "Single" : "Multiple"));
        cmd::PointResult base;
        for (;;) {
            PointOptions bo;
            bo.keywords = {"Displacement", "mOde"};
            base =
                co_await ctx.getPoint("Specify base point or [Displacement/mOde]", std::move(bo));
            if (base.status == Status::Keyword && base.keyword == "mOde") {
                KeywordOptions ko;
                ko.keywords = {"Single", "Multiple"};
                ko.defaultKeyword = single ? "Single" : "Multiple";
                auto m = co_await ctx.getKeyword("Enter a copy mode option [Single/Multiple]",
                                                 std::move(ko));
                if (m.ok()) {
                    single = m.keyword == "Single";
                    ctx.host().setSetting("copy.mode", single ? "single" : "multiple");
                }
                continue;
            }
            break;
        }
        if (base.status == Status::Keyword) {
            auto d = co_await ctx.getPoint("Specify displacement");
            if (d.ok() && !noDisplacement(ctx, d.point))
                report(ctx, applyTransform(ctx, ids, Transform2::translate(d.point), true));
            co_return;
        }
        if (!base.ok())
            co_return;
        bool first = true;
        int copies = 0;
        for (;;) {
            PointOptions o;
            o.base = base.point;
            o.keywords = {"Array", "Exit"};
            if (copies > 0)
                o.keywords.push_back("Undo");
            o.preview = previewOf(ctx, ids, [b = base.point](Vec2 c) -> std::optional<Transform2> {
                return Transform2::translate(c - b);
            });
            auto to = co_await ctx.getPoint(
                first ? "Specify second point or [Array] <use first point as displacement>"
                      : "Specify second point or [Array/Exit/Undo] (" + std::to_string(copies) +
                            (copies == 1 ? " copy" : " copies") + " made, Enter to finish)",
                std::move(o));
            if (to.status == Status::Keyword && to.keyword == "Undo") {
                // Takes back the last copy.
                if (ctx.rollbackToCheckpoint())
                    --copies;
                continue;
            }
            if (to.status == Status::Keyword && to.keyword == "Array") {
                co_await arrayCopies(ctx, ids, base.point);
                co_return;
            }
            if (to.status == Status::None && first) {
                if (noDisplacement(ctx, base.point))
                    co_return;
                report(ctx, applyTransform(ctx, ids, Transform2::translate(base.point), true));
                co_return;
            }
            if (!to.ok())
                co_return;  // Enter, Exit or Esc ends the repeat
            if (noDisplacement(ctx, to.point - base.point))
                continue;  // a copy on top of the original: ask again
            first = false;
            ++copies;
            ctx.checkpoint();
            report(ctx,
                   applyTransform(ctx, ids, Transform2::translate(to.point - base.point), true));
            if (single)
                co_return;
        }
    }
};

// ROTATE and SCALE offer the last value as the default, the way AutoCAD does.
static double lastValue(CommandContext& ctx, const char* key, double fallback) {
    const auto v = ctx.host().setting(key);
    if (!v)
        return fallback;
    char* end = nullptr;
    const double d = std::strtod(v->c_str(), &end);
    return end != v->c_str() && std::isfinite(d) ? d : fallback;
}

static void rememberValue(CommandContext& ctx, const char* key, double value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", value);
    ctx.host().setSetting(key, buf);
}

class RotateCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ROTATE"; }
    std::vector<std::string> defaultAliases() const override { return {"RO"}; }
    std::string_view description() const override { return "Rotate objects about a base point"; }

    Task run(CommandContext& ctx) override {
        std::vector<EntityId> ids;
        co_await askObjects(ctx, ids);
        if (ids.empty())
            co_return;
        auto base = co_await ctx.getPoint("Specify base point");
        if (!base.ok())
            co_return;
        const Vec2 b = base.point;
        bool copy = false;
        double angle = 0;
        const double lastAngle = lastValue(ctx, "rotate.angle", 0);
        for (;;) {
            AngleOptions o;
            o.base = b;
            o.keywords = {"Copy", "Reference"};
            o.defaultValue = lastAngle;
            o.preview = previewOf(ctx, ids, [b](Vec2 c) -> std::optional<Transform2> {
                return Transform2::rotate(std::atan2(c.y - b.y, c.x - b.x), b);
            });
            auto r = co_await ctx.getAngle(copy ? "Specify rotation angle or [Reference] (copy)"
                                                : "Specify rotation angle or [Copy/Reference]",
                                           std::move(o));
            if (r.status == Status::Keyword && r.keyword == "Copy") {
                copy = true;
                ctx.message("Rotating a copy of the selected objects.");
                continue;
            }
            if (r.status == Status::Keyword && r.keyword == "Reference") {
                // An angle, or two points that give the direction (the way AutoCAD asks); Enter
                // on the second point measures the first one from the base point.
                double refAngle = 0;
                for (bool have = false; !have;) {
                    PointOptions ro;
                    ro.base = b;
                    ro.freeText = true;
                    auto first = co_await ctx.getPoint(
                        "Specify the reference angle <0> or first point", std::move(ro));
                    if (first.status == Status::Cancel)
                        co_return;
                    if (first.status == Status::None) {
                        have = true;
                    } else if (first.status == Status::Keyword) {
                        auto ang = parseAngle(first.keyword);
                        if (ang) {
                            refAngle = ang.value();
                            have = true;
                        } else {
                            ctx.message("Enter an angle or pick two points.");
                        }
                    } else {
                        PointOptions so;
                        so.base = first.point;
                        auto second = co_await ctx.getPoint("Specify second point", std::move(so));
                        if (second.status == Status::Cancel)
                            co_return;
                        const Vec2 from = second.status == Status::Ok ? first.point : b;
                        const Vec2 to = second.status == Status::Ok ? second.point : first.point;
                        refAngle = std::atan2(to.y - from.y, to.x - from.x);
                        have = true;
                    }
                }
                AngleOptions no;
                no.base = b;
                no.preview =
                    previewOf(ctx, ids, [b, refAngle](Vec2 c) -> std::optional<Transform2> {
                        return Transform2::rotate(std::atan2(c.y - b.y, c.x - b.x) - refAngle, b);
                    });
                no.keywords = {"Points"};
                auto next =
                    co_await ctx.getAngle("Specify the new angle or [Points]", std::move(no));
                double newAngle = next.value;
                if (next.status == Status::Keyword) {
                    // as AutoCAD: the new angle is the direction from one point to another
                    auto p1 = co_await ctx.getPoint("Specify first point");
                    if (!p1.ok())
                        co_return;
                    PointOptions so;
                    so.base = p1.point;
                    auto p2 = co_await ctx.getPoint("Specify second point", std::move(so));
                    if (!p2.ok())
                        co_return;
                    if (tol::isZeroLength(p2.point - p1.point)) {
                        ctx.message("The two points must differ.");
                        co_return;
                    }
                    newAngle = std::atan2(p2.point.y - p1.point.y, p2.point.x - p1.point.x);
                } else if (!next.ok()) {
                    co_return;
                }
                angle = newAngle - refAngle;
                break;
            }
            if (!r.ok())
                co_return;
            angle = r.value;
            break;
        }
        rememberValue(ctx, "rotate.angle", angle);
        report(ctx, applyTransform(ctx, ids, Transform2::rotate(angle, b), copy));
    }
};

class ScaleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SCALE"; }
    std::vector<std::string> defaultAliases() const override { return {"SC"}; }
    std::string_view description() const override { return "Scale objects uniformly"; }

    Task run(CommandContext& ctx) override {
        std::vector<EntityId> ids;
        co_await askObjects(ctx, ids);
        if (ids.empty())
            co_return;
        auto base = co_await ctx.getPoint("Specify base point");
        if (!base.ok())
            co_return;
        const Vec2 b = base.point;
        bool copy = false;
        double factor = 1;
        const double lastFactor = lastValue(ctx, "scale.factor", 1);
        for (;;) {
            DistanceOptions o;
            o.base = b;
            o.keywords = {"Copy", "Reference"};
            o.allowZero = false;
            if (lastFactor > 0)
                o.defaultValue = lastFactor;
            o.preview = previewOf(ctx, ids, [b](Vec2 c) -> std::optional<Transform2> {
                const double f = distance(c, b);
                if (f < kEps)
                    return std::nullopt;
                return Transform2::scale(f, f, b);
            });
            auto r = co_await ctx.getDistance(copy ? "Specify scale factor or [Reference] (copy)"
                                                   : "Specify scale factor or [Copy/Reference]",
                                              std::move(o));
            if (r.status == Status::Keyword && r.keyword == "Copy") {
                copy = true;
                ctx.message("Scaling a copy of the selected objects.");
                continue;
            }
            if (r.status == Status::Keyword && r.keyword == "Reference") {
                // A number, or two points whose distance is the reference length (the way AutoCAD
                // asks); Enter on the second point measures the first one from the base point.
                double refLen = 0;
                while (refLen <= 0) {
                    PointOptions ro;
                    ro.base = b;
                    ro.freeText = true;
                    auto first = co_await ctx.getPoint(
                        "Specify reference length <1> or first point", std::move(ro));
                    if (first.status == Status::Cancel)
                        co_return;
                    if (first.status == Status::None) {
                        refLen = 1;
                    } else if (first.status == Status::Keyword) {
                        auto len = parseLength(first.keyword, ctx.doc().header().units);
                        if (len && len.value() > 0)
                            refLen = len.value();
                        else
                            ctx.message("Enter a positive length or pick two points.");
                    } else {
                        PointOptions so;
                        so.base = first.point;
                        auto second = co_await ctx.getPoint("Specify second point", std::move(so));
                        if (second.status == Status::Cancel)
                            co_return;
                        refLen = second.status == Status::Ok ? distance(first.point, second.point)
                                                             : distance(b, first.point);
                        if (refLen <= 0) {
                            refLen = 0;
                            ctx.message("The reference length must not be zero.");
                        }
                    }
                }
                DistanceOptions no;
                no.base = b;
                no.allowZero = false;
                no.preview = previewOf(ctx, ids, [b, refLen](Vec2 c) -> std::optional<Transform2> {
                    const double f = distance(c, b) / refLen;
                    if (f < kEps)
                        return std::nullopt;
                    return Transform2::scale(f, f, b);
                });
                no.keywords = {"Points"};
                auto next =
                    co_await ctx.getDistance("Specify new length or [Points]", std::move(no));
                double newLen = next.value;
                if (next.status == Status::Keyword) {
                    // as AutoCAD: the new length is the distance between two points
                    auto p1 = co_await ctx.getPoint("Specify first point");
                    if (!p1.ok())
                        co_return;
                    PointOptions so;
                    so.base = p1.point;
                    auto p2 = co_await ctx.getPoint("Specify second point", std::move(so));
                    if (!p2.ok())
                        co_return;
                    newLen = distance(p1.point, p2.point);
                } else if (!next.ok()) {
                    co_return;
                }
                factor = newLen / refLen;
                break;
            }
            if (!r.ok())
                co_return;
            factor = r.value;
            break;
        }
        if (!(factor > kEps)) {
            ctx.message("The scale factor must be greater than zero.");
            co_return;
        }
        rememberValue(ctx, "scale.factor", factor);
        report(ctx, applyTransform(ctx, ids, Transform2::scale(factor, factor, b), copy));
    }
};

class MirrorCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MIRROR"; }
    std::vector<std::string> defaultAliases() const override { return {"MI"}; }
    std::string_view description() const override {
        return "Mirror objects about a line (text stays readable unless MIRRTEXT is 1)";
    }

    Task run(CommandContext& ctx) override {
        std::vector<EntityId> ids;
        co_await askObjects(ctx, ids);
        if (ids.empty())
            co_return;
        auto a = co_await ctx.getPoint("Specify first point of mirror line");
        if (!a.ok())
            co_return;
        Vec2 b;
        for (;;) {
            PointOptions o;
            o.base = a.point;
            o.preview = previewOf(ctx, ids, [p = a.point](Vec2 c) -> std::optional<Transform2> {
                auto m = Transform2::mirror(p, c);
                if (!m)
                    return std::nullopt;
                return m.value();
            });
            auto r = co_await ctx.getPoint("Specify second point of mirror line", std::move(o));
            if (!r.ok())
                co_return;
            if (distance(r.point, a.point) < kEps) {
                ctx.message("The two points of the mirror line must differ.");
                continue;
            }
            b = r.point;
            break;
        }
        auto m = Transform2::mirror(a.point, b);
        if (!m) {
            ctx.message(m.error().message);
            co_return;
        }
        KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto erase = co_await ctx.getKeyword("Erase source objects? [Yes/No] <No>", std::move(ko));
        if (!erase.ok())
            co_return;
        report(ctx, applyTransform(ctx, ids, m.value(), erase.keyword != "Yes"));
    }
};

// Entities a crossing window can stretch: visible, unlocked, not inside a block definition.
std::vector<EntityId> stretchCandidates(CommandContext& ctx, const BBox2& window) {
    const Document& doc = ctx.doc();
    std::vector<EntityId> out;
    auto consider = [&](const Entity& e, bool test) {
        if (!e.common.visible || doc.ownerBlock(e.common.id))
            return;
        const EffectiveLayer layer = doc.effective(e.common);
        if (!layer.visible() || layer.locked)
            return;
        if (test && !window.intersects(geom::bounds(e, doc)))
            return;
        out.push_back(e.common.id);
    };
    if (auto ids = ctx.host().entitiesInWindow(window)) {
        for (EntityId id : *ids)
            if (const Entity* e = doc.find(id))
                consider(*e, true);
    } else {
        doc.forEachEntity([&](const Entity& e) { consider(e, true); });
    }
    return out;
}

class StretchCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "STRETCH"; }
    std::vector<std::string> defaultAliases() const override { return {"S"}; }
    std::string_view description() const override {
        return "Stretch the vertices inside a crossing window";
    }

    Task run(CommandContext& ctx) override {
        ctx.message(
            "Select with a crossing window or polygon: only the end points and vertices inside "
            "it move.");
        // The first area, then at the base point prompt Window / CPolygon add more areas and
        // Remove takes the objects an area crosses out again (AutoCAD collects several).
        std::vector<StretchRegion> regions;
        std::vector<EntityId> ids;
        std::unordered_set<EntityId> seen;
        {
            PointOptions fo;
            fo.keywords = {"CPolygon"};
            auto c1 = co_await ctx.getPoint(
                "Specify first corner of the crossing window or [CPolygon]", std::move(fo));
            if (!c1.ok() && !(c1.status == Status::Keyword && c1.keyword == "CPolygon"))
                co_return;
            StretchRegion region;
            bool ok = false;
            co_await readRegion(ctx, c1.status == Status::Keyword, c1.point, region, ok);
            if (!ok)
                co_return;
            addRegion(ctx, std::move(region), regions, ids, seen);
        }
        auto stretchWith = [&](const std::vector<EntityId>& which, Vec2 d, std::string* err) {
            return stretchByRegions(ctx.doc(), which, regions, d, err);
        };
        // Only objects with an end point or vertex inside an area move; say how many do. The
        // ones that move are kept: the preview and the final stretch work on those only.
        std::vector<EntityId> moving;
        auto count = [&] {
            moving.clear();
            for (const auto& r : stretchWith(ids, {1, 0}, nullptr))
                moving.push_back(r.first);
        };
        count();
        if (ids.empty()) {
            ctx.message("Nothing to stretch in the window.");
            co_return;
        }
        if (moving.empty()) {
            ctx.message(
                "No end point or vertex lies inside the crossing window; nothing to stretch.");
            co_return;
        }
        ctx.message(std::to_string(moving.size()) + " found in the crossing window.");
        Vec2 delta{};
        for (;;) {
            PointOptions bo;
            bo.keywords = {"Displacement", "Window", "CPolygon", "Remove"};
            auto base = co_await ctx.getPoint(
                "Specify base point or [Displacement/Window/CPolygon/Remove]", std::move(bo));
            if (base.status == Status::Keyword && base.keyword == "Displacement") {
                auto d = co_await ctx.getPoint("Specify displacement");
                if (!d.ok())
                    co_return;
                delta = d.point;
                break;
            }
            if (base.status == Status::Keyword) {
                // Another area to add, or one whose objects leave the selection.
                const bool remove = base.keyword == "Remove";
                bool polygon = base.keyword == "CPolygon";
                Vec2 first{};
                if (remove) {
                    PointOptions ro;
                    ro.keywords = {"CPolygon"};
                    auto r = co_await ctx.getPoint(
                        "Specify first corner of the window to remove objects or [CPolygon]",
                        std::move(ro));
                    if (r.status == Status::Keyword)
                        polygon = true;
                    else if (!r.ok())
                        continue;
                    first = r.point;
                } else if (!polygon) {
                    auto r = co_await ctx.getPoint("Specify first corner of the crossing window");
                    if (!r.ok())
                        continue;
                    first = r.point;
                }
                StretchRegion region;
                bool ok = false;
                co_await readRegion(ctx, polygon, first, region, ok);
                if (!ok)
                    continue;
                if (remove) {
                    const std::vector<EntityId> found = stretchCandidates(ctx, region.window);
                    const std::unordered_set<EntityId> drop(found.begin(), found.end());
                    std::size_t gone = 0;
                    std::erase_if(ids, [&](EntityId i) {
                        const bool d = drop.count(i) != 0;
                        if (d) {
                            seen.erase(i);
                            ++gone;
                        }
                        return d;
                    });
                    ctx.message(std::to_string(gone) + " removed.");
                } else {
                    addRegion(ctx, std::move(region), regions, ids, seen);
                }
                count();
                ctx.message(std::to_string(moving.size()) + " found in the crossing windows.");
                if (moving.empty()) {
                    ctx.message("Nothing left to stretch.");
                    co_return;
                }
                continue;
            }
            if (!base.ok())
                co_return;
            PointOptions o;
            o.base = base.point;
            o.preview = [&ctx, &moving, &stretchWith, b = base.point](Vec2 c, DisplayList& out) {
                geom::DisplayContext dc;
                dc.pixelSizeMm = ctx.host().view().scale > 0 ? ctx.host().view().scale : 1;
                // A drawing-sized selection would rebuild every object on each mouse move; the
                // preview shows the first few thousand.
                constexpr std::size_t kPreviewMax = 4000;
                const std::vector<EntityId> shown(
                    moving.begin(), moving.begin() + static_cast<std::ptrdiff_t>(
                                                         std::min(moving.size(), kPreviewMax)));
                for (auto& [id, e] : stretchWith(shown, c - b, nullptr))
                    geom::buildDisplay(ctx.doc(), e, dc, out);
            };
            auto to = co_await ctx.getPoint(
                "Specify second point or <use first point as displacement>", std::move(o));
            if (to.status == Status::None)
                delta = base.point;
            else if (to.ok())
                delta = to.point - base.point;
            else
                co_return;
            break;
        }
        std::string err;
        std::size_t n = 0;
        for (auto& [id, e] : stretchWith(moving, delta, &err)) {
            Entity moved = std::move(e);
            if (ctx.tx().modify(id, [&](Entity& tgt) { tgt.data = std::move(moved.data); }))
                ++n;
        }
        if (!err.empty())
            ctx.message("Some objects could not be stretched: " + err);
        ctx.message(std::to_string(n) + " object(s) stretched.");
    }

private:
    // Reads a crossing window from its first corner, or a polygon (Enter closes it).
    static Task readRegion(CommandContext& ctx, bool polygon, Vec2 first, StretchRegion& region,
                           bool& ok) {
        ok = false;
        if (polygon) {
            for (;;) {
                PointOptions po;
                if (!region.polygon.empty())
                    po.base = region.polygon.back();
                auto p = co_await ctx.getPoint(region.polygon.size() < 3
                                                   ? "Specify next polygon point"
                                                   : "Specify next polygon point <Enter to finish>",
                                               std::move(po));
                if (p.status == Status::None && region.polygon.size() >= 3)
                    break;
                if (!p.ok())
                    co_return;
                region.polygon.push_back(p.point);
            }
            for (Vec2 v : region.polygon)
                region.window.expand(v);
            ok = true;
            co_return;
        }
        PointOptions co;
        co.base = first;
        co.preview = [first](Vec2 c, DisplayList& out) {  // the crossing window
            DisplayPolyline pl;
            pl.closed = true;
            pl.pts = {first, {c.x, first.y}, c, {first.x, c.y}};
            out.items.emplace_back(std::move(pl));
        };
        auto c2 = co_await ctx.getPoint("Specify opposite corner", std::move(co));
        if (!c2.ok())
            co_return;
        region.window.expand(first);
        region.window.expand(c2.point);
        ok = true;
    }

    static void addRegion(CommandContext& ctx, StretchRegion region,
                          std::vector<StretchRegion>& regions, std::vector<EntityId>& ids,
                          std::unordered_set<EntityId>& seen) {
        for (EntityId id : stretchCandidates(ctx, region.window))
            if (seen.insert(id).second)
                ids.push_back(id);
        regions.push_back(std::move(region));
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<MoveCommand>());
    (void)registry.add(std::make_unique<CopyCommand>());
    (void)registry.add(std::make_unique<RotateCommand>());
    (void)registry.add(std::make_unique<ScaleCommand>());
    (void)registry.add(std::make_unique<MirrorCommand>());
    (void)registry.add(std::make_unique<StretchCommand>());
    registerGripCommand(registry);
}

}  // namespace tbcad::transform
