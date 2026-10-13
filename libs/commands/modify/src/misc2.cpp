#include <algorithm>
#include <cmath>

#include "commands.h"
#include "util.h"

namespace tbcad::modifycmd {

namespace {

// ---- BLEND --------------------------------------------------------------------------------------

class BlendCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BLEND"; }
    std::vector<std::string> defaultAliases() const override { return {"BLE"}; }
    std::string_view description() const override {
        return "Join two open curves with a tangent spline";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("BLEND");
        cmd::EntityOptions eo;
        eo.filter = [&doc = ctx.doc()](const Entity& e) {
            if (!isEditable(doc, e))
                return false;
            auto k = kind(e);
            return k == EntityKind::Line || k == EntityKind::Arc || k == EntityKind::Polyline ||
                   k == EntityKind::Spline;
        };
        auto a = co_await ctx.getEntity("Select first object near the end to blend", eo);
        if (!a.ok())
            co_return;
        auto b = co_await ctx.getEntity("Select second object near the end to blend", eo);
        if (!b.ok())
            co_return;
        const Entity* ea = ctx.doc().find(a.id);
        const Entity* eb = ctx.doc().find(b.id);
        if (!ea || !eb || a.id == b.id) {
            ctx.message("Select two different objects.");
            co_return;
        }
        auto sp = modify::blendCurves(*ea, a.pickPoint, *eb, b.pickPoint);
        if (!sp) {
            ctx.message(sp.error().message);
            co_return;
        }
        if (auto r = ctx.tx().add(std::move(sp.value())); !r)
            ctx.message(r.error().message);
    }
};

// ---- EXTRIM -------------------------------------------------------------------------------------

class ExtrimCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "EXTRIM"; }
    std::string_view description() const override {
        return "Trim everything on one side of a cutting edge";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("EXTRIM");
        cmd::EntityOptions eo;
        eo.filter = [&doc = ctx.doc()](const Entity& e) { return isUsableEdge(doc, e); };
        auto edge = co_await ctx.getEntity("Select a cutting edge", eo);
        if (!edge.ok())
            co_return;
        auto side = co_await ctx.getPoint("Specify the side to trim");
        if (!side.ok())
            co_return;
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc(), edge = edge.id](const Entity& e) {
            return e.common.id != edge && isEditable(doc, e) && modify::isCurveEntity(e);
        };
        auto sel = co_await ctx.getSelection("Select objects to trim", so);
        if (!sel.ok())
            co_return;
        const Entity* cutter = ctx.doc().find(edge.id);
        if (!cutter)
            co_return;
        Entity edgeCopy = *cutter;
        std::size_t trimmed = 0;
        std::string firstError;
        for (EntityId id : sel.ids) {
            if (id == edge.id)
                continue;
            const Entity* t = ctx.doc().find(id);
            if (!t)
                continue;
            auto edit = modify::extrimEntity(*t, edgeCopy, side.point);
            if (!edit) {
                if (firstError.empty())
                    firstError = edit.error().message;
                continue;
            }
            if (!edit.value().modified && !edit.value().eraseSource)
                continue;
            if (applyEdit(ctx.tx(), id, std::move(edit.value())))
                ++trimmed;
        }
        if (trimmed)
            ctx.message(std::to_string(trimmed) + " objects trimmed.");
        else
            ctx.message(firstError.empty() ? "Nothing crosses the cutting edge on that side."
                                           : firstError);
    }
};

// ---- SCALETEXT ----------------------------------------------------------------------------------

class ScaleTextCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SCALETEXT"; }
    std::string_view description() const override {
        return "Change the height of text without moving it";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc()](const Entity& e) {
            return isModifiable(doc, e) &&
                   (kind(e) == EntityKind::Text || kind(e) == EntityKind::MText);
        };
        auto sel = co_await ctx.getSelection("Select text objects", so);
        if (!sel.ok())
            co_return;
        cmd::RealOptions ro;
        ro.keywords = {"New height", "Match"};
        ro.defaultValue = 1.0;
        ro.min = 1e-6;
        auto r = co_await ctx.getReal("Specify scale factor or [New height/Match object] <1>", ro);
        if (r.status == cmd::Status::Cancel)
            co_return;
        double factor = 0, newHeight = 0;
        bool absolute = false;
        if (r.status == cmd::Status::Keyword && r.keyword == "New height") {
            cmd::DistanceOptions d;
            d.allowZero = false;
            auto h = co_await ctx.getDistance("Specify new height", d);
            if (!h.ok())
                co_return;
            newHeight = h.value;
            absolute = true;
        } else if (r.status == cmd::Status::Keyword && r.keyword == "Match") {
            cmd::EntityOptions eo;
            eo.filter = so.filter;
            auto m = co_await ctx.getEntity("Select text object with the height to match", eo);
            if (!m.ok())
                co_return;
            const Entity* me = ctx.doc().find(m.id);
            if (!me)
                co_return;
            if (const auto* t = entityAs<TextData>(*me))
                newHeight = t->height;
            else if (const auto* mt = entityAs<MTextData>(*me))
                newHeight = mt->height;
            absolute = true;
        } else {
            factor = r.value;
        }
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            auto r2 = ctx.tx().modify(id, [&](Entity& e) {
                if (auto* t = entityAs<TextData>(e))
                    t->height = absolute ? newHeight : t->height * factor;
                else if (auto* mt = entityAs<MTextData>(e))
                    mt->height = absolute ? newHeight : mt->height * factor;
            });
            if (r2)
                ++n;
        }
        ctx.message(std::to_string(n) + " text objects changed.");
    }
};

// ---- SETBYLAYER ---------------------------------------------------------------------------------

class SetByLayerCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SETBYLAYER"; }
    std::string_view description() const override {
        return "Set colour, linetype and lineweight of objects back to ByLayer";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc()](const Entity& e) { return isModifiable(doc, e); };
        auto sel = co_await ctx.getSelection("Select objects to set ByLayer", so);
        if (!sel.ok())
            co_return;
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            auto r = ctx.tx().modify(id, [](Entity& e) {
                e.common.color = Color::byLayer();
                e.common.linetype = kLinetypeByLayer;
                e.common.lineweight = LineWeight::byLayer();
            });
            if (r)
                ++n;
        }
        ctx.message(std::to_string(n) + " objects set to ByLayer.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeBlend() {
    return std::make_unique<BlendCommand>();
}
std::unique_ptr<cmd::Command> makeExtrim() {
    return std::make_unique<ExtrimCommand>();
}
std::unique_ptr<cmd::Command> makeScaleText() {
    return std::make_unique<ScaleTextCommand>();
}
std::unique_ptr<cmd::Command> makeSetByLayer() {
    return std::make_unique<SetByLayerCommand>();
}

}  // namespace tbcad::modifycmd
