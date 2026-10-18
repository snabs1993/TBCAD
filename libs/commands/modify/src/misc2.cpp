#include <algorithm>
#include <cmath>

#include "commands.h"
#include "tbcad/text/text.h"
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

// ---- JUSTIFYTEXT --------------------------------------------------------------------------------

struct Justify {
    HAlign h = HAlign::Left;
    VAlign v = VAlign::Baseline;
    Attachment attach = Attachment::TopLeft;
};

std::optional<Justify> justifyFor(std::string_view k) {
    Justify j;
    struct Row {
        const char* key;
        HAlign h;
        VAlign v;
        Attachment a;
    };
    static const Row rows[] = {
        {"Left", HAlign::Left, VAlign::Baseline, Attachment::TopLeft},
        {"Center", HAlign::Center, VAlign::Baseline, Attachment::TopCenter},
        {"Right", HAlign::Right, VAlign::Baseline, Attachment::TopRight},
        {"Middle", HAlign::Middle, VAlign::Middle, Attachment::MiddleCenter},
        {"TL", HAlign::Left, VAlign::Top, Attachment::TopLeft},
        {"TC", HAlign::Center, VAlign::Top, Attachment::TopCenter},
        {"TR", HAlign::Right, VAlign::Top, Attachment::TopRight},
        {"ML", HAlign::Left, VAlign::Middle, Attachment::MiddleLeft},
        {"MC", HAlign::Center, VAlign::Middle, Attachment::MiddleCenter},
        {"MR", HAlign::Right, VAlign::Middle, Attachment::MiddleRight},
        {"BL", HAlign::Left, VAlign::Bottom, Attachment::BottomLeft},
        {"BC", HAlign::Center, VAlign::Bottom, Attachment::BottomCenter},
        {"BR", HAlign::Right, VAlign::Bottom, Attachment::BottomRight},
    };
    for (const Row& r : rows) {
        if (k == r.key) {
            j.h = r.h;
            j.v = r.v;
            j.attach = r.a;
            return j;
        }
    }
    return std::nullopt;
}

class JustifyTextCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "JUSTIFYTEXT"; }
    std::string_view description() const override {
        return "Change the justification of text without moving it";
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
        cmd::KeywordOptions ko;
        ko.keywords = {"Left", "Center", "Right", "Middle", "TL", "TC", "TR",
                       "ML",   "MC",     "MR",    "BL",     "BC", "BR"};
        auto k = co_await ctx.getKeyword(
            "Enter a justification [Left/Center/Right/Middle/TL/TC/TR/ML/MC/MR/BL/BC/BR]", ko);
        if (!k.ok())
            co_return;
        auto j = justifyFor(k.keyword);
        if (!j)
            co_return;
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            text::TextLayout before = text::layoutEntity(ctx.doc(), *e);
            if (before.empty())
                continue;
            Entity moved = *e;
            if (auto* t = entityAs<TextData>(moved)) {
                if (t->h == HAlign::Aligned || t->h == HAlign::Fit)
                    continue;
                t->h = j->h;
                t->v = j->v;
                t->position = t->alignPoint = {0, 0};
            } else if (auto* m = entityAs<MTextData>(moved)) {
                m->attach = j->attach;
                m->position = {0, 0};
            }
            text::TextLayout after = text::layoutEntity(ctx.doc(), moved);
            if (after.empty())
                continue;
            Vec2 shift = before.frameCorners()[0] - after.frameCorners()[0];
            if (auto* t = entityAs<TextData>(moved))
                t->position = t->alignPoint = shift;
            else if (auto* m = entityAs<MTextData>(moved))
                m->position = shift;
            if (replaceEntity(ctx.tx(), id, moved))
                ++n;
        }
        ctx.message(std::to_string(n) + " text objects justified.");
    }
};

// ---- SPLINEDIT ----------------------------------------------------------------------------------

class SplineEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SPLINEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"SPE"}; }
    std::string_view description() const override {
        return "Edit the fit points or control vertices of a spline";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        using modify::SplineFitOp;
        ctx.commitOnCancel();
        ctx.setUndoLabel("SPLINEDIT");
        cmd::EntityOptions eo;
        eo.filter = [&doc = ctx.doc()](const Entity& e) {
            return isEditable(doc, e) && kind(e) == EntityKind::Spline;
        };
        auto pick = co_await ctx.getEntity("Select spline", eo);
        if (!pick.ok())
            co_return;
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Add",  "Delete", "Move",     "Weight",  "Close",
                           "Open", "Purge",  "Polyline", "Reverse", "Exit"};
            ko.defaultKeyword = "Exit";
            auto k = co_await ctx.getKeyword(
                "Enter an option [Add/Delete/Move/Weight/Close/Open/Purge/Polyline/Reverse/Exit] "
                "<Exit>",
                ko);
            if (!k.ok() || k.keyword == "Exit")
                co_return;
            const Entity* e0 = ctx.doc().find(pick.id);
            if (!e0)
                co_return;
            const auto* sd = entityAs<SplineData>(*e0);
            if (!sd) {
                ctx.message("The object is no longer a spline.");
                co_return;
            }
            // Splines with fit points are edited by those points; the others by their vertices.
            const bool byFit = sd->fit.size() >= 2;
            Result<modify::EntityEdit> edit = Error{ErrorCode::InvalidArgument, "Unsupported"};
            bool endAfter = false;
            if (k.keyword == "Reverse") {
                edit = modify::reverseEntity(*e0);
            } else if (k.keyword == "Purge") {
                edit = modify::editSplineCv(*e0, modify::SplineCvOp::Purge);
            } else if (k.keyword == "Polyline") {
                cmd::RealOptions ro;
                ro.defaultValue = 0.1;
                ro.min = 1e-6;
                auto tolerance =
                    co_await ctx.getReal("Specify the largest gap to the curve <0.1>", ro);
                if (!tolerance.ok())
                    continue;
                edit = modify::editSplineCv(*e0, modify::SplineCvOp::ToPolyline, {}, {},
                                            tolerance.value);
                endAfter = true;
            } else if (k.keyword == "Close" || k.keyword == "Open") {
                edit = byFit ? modify::editSplineFit(*e0, k.keyword == "Close" ? SplineFitOp::Close
                                                                               : SplineFitOp::Open)
                             : modify::editSplineCv(*e0, k.keyword == "Close"
                                                             ? modify::SplineCvOp::Close
                                                             : modify::SplineCvOp::Open);
            } else {
                auto a = co_await ctx.getPoint(k.keyword == "Add"
                                                   ? "Specify a point near the curve"
                                                   : (byFit && k.keyword != "Weight"
                                                          ? "Specify a point near the fit point"
                                                          : "Specify a point near the vertex"));
                if (!a.ok())
                    continue;
                Vec2 to{};
                double weight = 1;
                if (k.keyword == "Move") {
                    auto b = co_await ctx.getPoint("Specify the new location");
                    if (!b.ok())
                        continue;
                    to = b.point;
                } else if (k.keyword == "Weight") {
                    cmd::RealOptions ro;
                    ro.defaultValue = 1.0;
                    ro.min = 1e-6;
                    auto w = co_await ctx.getReal("Enter the new weight <1>", ro);
                    if (!w.ok())
                        continue;
                    weight = w.value;
                }
                e0 = ctx.doc().find(pick.id);
                if (!e0)
                    co_return;
                if (k.keyword == "Weight") {
                    edit =
                        modify::editSplineCv(*e0, modify::SplineCvOp::Weight, a.point, to, weight);
                } else if (byFit) {
                    SplineFitOp op = k.keyword == "Add"      ? SplineFitOp::Add
                                     : k.keyword == "Delete" ? SplineFitOp::Delete
                                                             : SplineFitOp::Move;
                    edit = modify::editSplineFit(*e0, op, a.point, to);
                } else {
                    modify::SplineCvOp op = k.keyword == "Add"      ? modify::SplineCvOp::Add
                                            : k.keyword == "Delete" ? modify::SplineCvOp::Delete
                                                                    : modify::SplineCvOp::Move;
                    edit = modify::editSplineCv(*e0, op, a.point, to);
                }
            }
            if (!edit) {
                ctx.message(edit.error().message);
            } else {
                (void)applyEdit(ctx.tx(), pick.id, std::move(edit.value()));
                if (endAfter)
                    co_return;
            }
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeJustifyText() {
    return std::make_unique<JustifyTextCommand>();
}
std::unique_ptr<cmd::Command> makeSplineEdit() {
    return std::make_unique<SplineEditCommand>();
}
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
