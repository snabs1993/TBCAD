#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"

namespace tbcad::block_cmds {

namespace {

// Entities of the block definition that lie completely inside the window.
std::vector<EntityId> objectsInWindow(const Document& doc, const BlockDef& def, Vec2 a, Vec2 b) {
    const BBox2 win{{std::min(a.x, b.x), std::min(a.y, b.y)},
                    {std::max(a.x, b.x), std::max(a.y, b.y)}};
    std::vector<EntityId> out;
    for (EntityId id : def.entities) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        const BBox2 box = geom::bounds(*e, doc);
        if (box.empty())
            continue;
        const double eps = 1e-9;
        if (box.min.x >= win.min.x - eps && box.min.y >= win.min.y - eps &&
            box.max.x <= win.max.x + eps && box.max.y <= win.max.y + eps)
            out.push_back(id);
    }
    return out;
}

const BlockDef* findEditableBlock(cmd::CommandContext& ctx, const std::string& name) {
    const BlockDef* def = ctx.doc().blockByName(name);
    if (!def) {
        ctx.message("There is no block called \"" + name + "\"");
        return nullptr;
    }
    if (!def->xrefPath.empty()) {
        ctx.message("An external reference cannot be made dynamic");
        return nullptr;
    }
    return def;
}

// Inside BEDIT the commands work on the block being edited: points are picked in the drawing and
// taken into block space, objects are picked among the working copies.
struct Target {
    const BlockDef* def = nullptr;
    std::optional<Transform2> toBlock;  // set while editing
    std::string space() const { return toBlock ? "" : " (block coordinates)"; }
    Vec2 map(Vec2 p) const { return toBlock ? toBlock->apply(p) : p; }
};

// The block BPARAM / BACTION work on: the one being edited, or one named at the prompt.
cmd::Task target(cmd::CommandContext& ctx, Target& t) {
    t = {};
    if (const auto edit = detail::blockEdit(ctx.doc(), ctx.host())) {
        if (edit->xref) {
            ctx.message("An external reference cannot be made dynamic");
            co_return;
        }
        auto inv = edit->xf.inverse();
        if (!inv) {
            ctx.message("The block being edited has a zero-scale reference.");
            co_return;
        }
        t.def = ctx.doc().block(edit->block);
        t.toBlock = inv.value();
        co_return;
    }
    auto blockName = co_await ctx.getString("Enter block name");
    if (!blockName.ok())
        co_return;
    t.def = findEditableBlock(ctx, cmd::trim(blockName.text));
}

// Picks working copies and returns the block objects they stand for (saving the edit first, so
// objects drawn in the editor are in the block). Empty when nothing was picked.
cmd::Task pickInEditor(cmd::CommandContext& ctx, std::string prompt, std::vector<EntityId>& out) {
    out.clear();
    const auto edit = detail::blockEdit(ctx.doc(), ctx.host());
    if (!edit)
        co_return;
    const std::unordered_set<EntityId> working(edit->working.begin(), edit->working.end());
    cmd::SelectionOptions so;
    so.filter = [working](const Entity& e) { return working.count(e.common.id) > 0; };
    so.allowPreselection = false;
    auto sel = co_await ctx.getSelection(std::move(prompt), so);
    if (!sel.ok() || sel.ids.empty())
        co_return;
    if (!detail::saveBlockEdit(ctx))
        co_return;
    out = detail::blockIdsOf(ctx.host(), sel.ids);
}

std::string describe(const DynParam& p) {
    switch (p.kind) {
        case DynParam::Kind::Linear:
            return "linear";
        case DynParam::Kind::Rotation:
            return "rotation";
        case DynParam::Kind::Flip:
            return "flip";
        case DynParam::Kind::Visibility:
            return "visibility";
        case DynParam::Kind::Lookup:
            return "lookup";
    }
    return "";
}

// BPARAM: adds a parameter to a block. Points are in the block's own coordinates.
class BParamCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BPARAM"; }
    std::string_view description() const override {
        return "Add a dynamic parameter (linear, rotation, flip, visibility, lookup table) to a "
               "block";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Target t;
        co_await target(ctx, t);
        const BlockDef* def = t.def;
        if (!def)
            co_return;
        const BlockId block = def->id;
        const std::string space = t.space();

        auto type = co_await ctx.getKeyword(
            "Parameter type [Linear/Rotation/Flip/Visibility/Table] <Linear>",
            cmd::KeywordOptions{.keywords = {"Linear", "Rotation", "Flip", "Visibility", "Table"},
                                .defaultKeyword = "Linear"});
        if (!type.ok())
            co_return;
        auto label = co_await ctx.getString("Enter parameter name <" + type.keyword + ">",
                                            cmd::StringOptions{.allowEmpty = true});
        if (label.status == cmd::Status::Cancel)
            co_return;

        DynParam p;
        p.name = cmd::trim(label.text).empty() ? type.keyword : cmd::trim(label.text);
        if (def->dynamic && blocks::dynFind(*def->dynamic, p.name) >= 0) {
            ctx.message("The block already has a parameter called \"" + p.name + "\"");
            co_return;
        }
        if (type.keyword == "Linear") {
            p.kind = DynParam::Kind::Linear;
            auto a = co_await ctx.getPoint("Specify the start point" + space);
            if (!a.ok())
                co_return;
            auto b = co_await ctx.getPoint("Specify the end point" + space,
                                           cmd::PointOptions{.base = a.point});
            if (!b.ok())
                co_return;
            if (tol::isZeroLength(b.point - a.point)) {
                ctx.message("The two points are the same");
                co_return;
            }
            p.base = t.map(a.point);
            p.end = t.map(b.point);
            auto lo = co_await ctx.getReal("Minimum distance, 0 for no limit <0>",
                                           cmd::RealOptions{.defaultValue = 0.0});
            if (!lo.ok())
                co_return;
            auto hi = co_await ctx.getReal("Maximum distance, 0 for no limit <0>",
                                           cmd::RealOptions{.defaultValue = 0.0});
            if (!hi.ok())
                co_return;
            if (hi.value > lo.value) {
                p.min = lo.value;
                p.max = hi.value;
            }
        } else if (type.keyword == "Rotation") {
            p.kind = DynParam::Kind::Rotation;
            auto c = co_await ctx.getPoint("Specify the centre of rotation");
            if (!c.ok())
                co_return;
            auto h = co_await ctx.getPoint("Specify the handle position (at angle 0)",
                                           cmd::PointOptions{.base = c.point});
            if (!h.ok())
                co_return;
            if (tol::isZeroLength(h.point - c.point)) {
                ctx.message("The handle cannot sit on the centre");
                co_return;
            }
            p.base = t.map(c.point);
            p.end = t.map(h.point);
        } else if (type.keyword == "Flip") {
            p.kind = DynParam::Kind::Flip;
            auto a = co_await ctx.getPoint("Specify the first point of the mirror line" + space);
            if (!a.ok())
                co_return;
            auto b = co_await ctx.getPoint("Specify the second point of the mirror line",
                                           cmd::PointOptions{.base = a.point});
            if (!b.ok())
                co_return;
            if (tol::isZeroLength(b.point - a.point)) {
                ctx.message("The two points are the same");
                co_return;
            }
            p.base = t.map(a.point);
            p.end = t.map(b.point);
        } else if (type.keyword == "Table") {
            p.kind = DynParam::Kind::Lookup;
            if (!def->dynamic || def->dynamic->params.empty()) {
                ctx.message("Add the parameters the table drives first (BPARAM)");
                co_return;
            }
            auto at = co_await ctx.getPoint("Specify where the lookup grip sits" + space);
            if (!at.ok())
                co_return;
            p.base = t.map(at.point);
            for (;;) {
                auto n = co_await ctx.getString(
                    p.lookupParams.empty()
                        ? "Enter the name of the first parameter to drive"
                        : "Enter the name of the next parameter, Enter to finish",
                    cmd::StringOptions{.allowEmpty = !p.lookupParams.empty()});
                if (!n.ok() && n.status != cmd::Status::None)
                    co_return;
                const std::string target = cmd::trim(n.text);
                if (target.empty())
                    break;
                const int at = blocks::dynFind(*def->dynamic, target);
                const DynParam* tp =
                    at >= 0 ? &def->dynamic->params[static_cast<std::size_t>(at)] : nullptr;
                if (!tp || tp->kind == DynParam::Kind::Lookup) {
                    ctx.message("There is no parameter called \"" + target +
                                "\" that a table can drive");
                    continue;
                }
                p.lookupParams.push_back(at);
            }
            for (;;) {
                auto st = co_await ctx.getString(
                    p.states.empty() ? "Enter the name of the first row"
                                     : "Enter the name of the next row, Enter to finish",
                    cmd::StringOptions{.allowEmpty = !p.states.empty()});
                if (!st.ok() && st.status != cmd::Status::None)
                    co_return;
                const std::string rowName = cmd::trim(st.text);
                if (rowName.empty())
                    break;
                std::vector<double> vals;
                for (int at : p.lookupParams) {
                    const DynParam& tp = def->dynamic->params[static_cast<std::size_t>(at)];
                    auto v =
                        co_await ctx.getReal("Value of " + tp.name + " in \"" + rowName + "\"");
                    if (!v.ok())
                        co_return;
                    vals.push_back(v.value);
                }
                p.states.push_back(rowName);
                p.stateObjects.emplace_back();
                p.lookupValues.push_back(std::move(vals));
            }
            if (p.states.size() < 2) {
                ctx.message("A lookup parameter needs at least two rows");
                co_return;
            }
        } else {
            p.kind = DynParam::Kind::Visibility;
            if (def->dynamic)
                for (const DynParam& q : def->dynamic->params)
                    if (q.kind == DynParam::Kind::Visibility) {
                        ctx.message("A block can have only one visibility parameter");
                        co_return;
                    }
            for (;;) {
                auto st = co_await ctx.getString(
                    p.states.empty() ? "Enter the name of the first visibility state"
                                     : "Enter the name of the next state, Enter to finish",
                    cmd::StringOptions{.allowEmpty = !p.states.empty()});
                if (!st.ok() && st.status != cmd::Status::None)
                    co_return;
                const std::string stateName = cmd::trim(st.text);
                if (stateName.empty())
                    break;
                std::vector<EntityId> shown;
                if (t.toBlock) {
                    co_await pickInEditor(ctx, "Select the objects shown in \"" + stateName + "\"",
                                          shown);
                } else {
                    auto c1 = co_await ctx.getPoint(
                        "Specify first corner of the objects shown in \"" + stateName + "\"");
                    if (!c1.ok())
                        co_return;
                    auto c2 = co_await ctx.getPoint("Specify opposite corner",
                                                    cmd::PointOptions{.base = c1.point});
                    if (!c2.ok())
                        co_return;
                    shown = objectsInWindow(ctx.doc(), *ctx.doc().block(block), c1.point, c2.point);
                }
                p.states.push_back(stateName);
                p.stateObjects.push_back(std::move(shown));
            }
            if (p.states.size() < 2) {
                ctx.message("A visibility parameter needs at least two states");
                co_return;
            }
        }
        const std::string kindName = describe(p);
        (void)ctx.tx().modifyBlock(block, [&](BlockDef& d) {
            if (!d.dynamic)
                d.dynamic = DynamicDef{};
            d.dynamic->params.push_back(std::move(p));
        });
        ctx.message("Added the " + kindName + " parameter");
        ctx.setUndoLabel("Add parameter");
    }
};

// BACTION: adds an action driven by a parameter, acting on the block objects inside a window.
class BActionCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BACTION"; }
    std::string_view description() const override {
        return "Add a dynamic action (stretch, move, scale, rotate, flip) to a block";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Target t;
        co_await target(ctx, t);
        const BlockDef* def = t.def;
        if (!def)
            co_return;
        if (!def->dynamic || def->dynamic->params.empty()) {
            ctx.message("Add a parameter first (BPARAM)");
            co_return;
        }
        const BlockId block = def->id;
        auto pname = co_await ctx.getString("Enter parameter name");
        if (!pname.ok())
            co_return;
        const int pi = blocks::dynFind(*def->dynamic, cmd::trim(pname.text));
        if (pi < 0) {
            ctx.message("The block has no parameter called \"" + cmd::trim(pname.text) + "\"");
            co_return;
        }
        const DynParam param = def->dynamic->params[static_cast<std::size_t>(pi)];

        DynAction a;
        a.param = pi;
        switch (param.kind) {
            case DynParam::Kind::Linear: {
                auto k = co_await ctx.getKeyword(
                    "Action [Stretch/Move/Scale] <Stretch>",
                    cmd::KeywordOptions{.keywords = {"Stretch", "Move", "Scale"},
                                        .defaultKeyword = "Stretch"});
                if (!k.ok())
                    co_return;
                a.kind = k.keyword == "Move"    ? DynAction::Kind::Move
                         : k.keyword == "Scale" ? DynAction::Kind::Scale
                                                : DynAction::Kind::Stretch;
                break;
            }
            case DynParam::Kind::Rotation:
                a.kind = DynAction::Kind::Rotate;
                break;
            case DynParam::Kind::Flip:
                a.kind = DynAction::Kind::Flip;
                break;
            case DynParam::Kind::Visibility:
                ctx.message("A visibility parameter needs no action");
                co_return;
            case DynParam::Kind::Lookup:
                ctx.message("A lookup parameter needs no action: its rows set other parameters");
                co_return;
        }
        if (a.kind == DynAction::Kind::Stretch) {
            auto f1 = co_await ctx.getPoint("Specify first corner of the stretch frame");
            if (!f1.ok())
                co_return;
            auto f2 = co_await ctx.getPoint("Specify opposite corner of the stretch frame",
                                            cmd::PointOptions{.base = f1.point});
            if (!f2.ok())
                co_return;
            const Vec2 m1 = t.map(f1.point), m2 = t.map(f2.point);
            a.frameMin = {std::min(m1.x, m2.x), std::min(m1.y, m2.y)};
            a.frameMax = {std::max(m1.x, m2.x), std::max(m1.y, m2.y)};
        }
        if (t.toBlock) {
            co_await pickInEditor(ctx, "Select the objects the action changes", a.objects);
            if (a.objects.empty())
                co_return;
        } else {
            auto o1 =
                co_await ctx.getPoint("Specify first corner of the objects the action changes");
            if (!o1.ok())
                co_return;
            auto o2 = co_await ctx.getPoint("Specify opposite corner",
                                            cmd::PointOptions{.base = o1.point});
            if (!o2.ok())
                co_return;
            a.objects = objectsInWindow(ctx.doc(), *def, o1.point, o2.point);
            if (a.objects.empty()) {
                ctx.message("No object of the block lies completely inside that window");
                co_return;
            }
        }
        const std::size_t n = a.objects.size();
        (void)ctx.tx().modifyBlock(
            block, [&](BlockDef& d) { d.dynamic->actions.push_back(std::move(a)); });
        ctx.message("Added an action on " + detail::objects(n));
        ctx.setUndoLabel("Add action");
    }
};

// DYNSET: sets one parameter of a dynamic insert.
class DynSetCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DYNSET"; }
    std::string_view description() const override {
        return "Set a dynamic parameter of an inserted block";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        cmd::EntityOptions eo;
        eo.filter = [&doc](const Entity& e) {
            const auto* ins = entityAs<InsertData>(e);
            const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
            return def && def->dynamic;
        };
        auto picked = co_await ctx.getEntity("Select a dynamic block", eo);
        if (!picked.ok())
            co_return;
        const Entity* e = doc.find(picked.id);
        const auto* ins = entityAs<InsertData>(*e);
        const BlockDef* def = doc.block(ins->block);
        const std::vector<double> now = blocks::dynValues(*def, *ins);
        std::string list;
        for (const DynParam& p : def->dynamic->params)
            list += (list.empty() ? "" : "/") + p.name;
        auto pname = co_await ctx.getString("Parameter [" + list + "]");
        if (!pname.ok())
            co_return;
        const int pi = blocks::dynFind(*def->dynamic, cmd::trim(pname.text));
        if (pi < 0) {
            ctx.message("No such parameter");
            co_return;
        }
        const DynParam& p = def->dynamic->params[static_cast<std::size_t>(pi)];
        const double cur = now[static_cast<std::size_t>(pi)];
        double value = cur;
        if (p.kind == DynParam::Kind::Linear) {
            auto v = co_await ctx.getReal("Enter the distance <" + std::to_string(cur) + ">",
                                          cmd::RealOptions{.defaultValue = cur});
            if (!v.ok())
                co_return;
            value = v.value;
        } else if (p.kind == DynParam::Kind::Rotation) {
            auto v =
                co_await ctx.getAngle("Enter the angle", cmd::AngleOptions{.defaultValue = cur});
            if (!v.ok())
                co_return;
            value = v.value;
        } else if (p.kind == DynParam::Kind::Flip) {
            auto v = co_await ctx.getKeyword(
                "Flipped? [Yes/No] <" + std::string(cur >= 0.5 ? "Yes" : "No") + ">",
                cmd::KeywordOptions{.keywords = {"Yes", "No"},
                                    .defaultKeyword = cur >= 0.5 ? "Yes" : "No"});
            if (!v.ok())
                co_return;
            value = v.keyword == "Yes" ? 1.0 : 0.0;
        } else {
            std::string states;
            for (const std::string& s : p.states)
                states += (states.empty() ? "" : "/") + s;
            auto v = co_await ctx.getKeyword(
                "State [" + states + "]",
                cmd::KeywordOptions{.keywords = p.states,
                                    .defaultKeyword = p.states[static_cast<std::size_t>(cur)]});
            if (!v.ok())
                co_return;
            for (std::size_t i = 0; i < p.states.size(); ++i)
                if (p.states[i] == v.keyword)
                    value = static_cast<double>(i);
        }
        auto changed = blocks::dynSet(doc, *e, pi, value);
        if (!changed) {
            ctx.message(changed.error().message);
            co_return;
        }
        (void)ctx.tx().modify(picked.id,
                              [&](Entity& target) { target = std::move(changed).value(); });
        ctx.setUndoLabel("Set " + p.name);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeBParamCommand() {
    return std::make_unique<BParamCommand>();
}
std::unique_ptr<cmd::Command> makeBActionCommand() {
    return std::make_unique<BActionCommand>();
}
std::unique_ptr<cmd::Command> makeDynSetCommand() {
    return std::make_unique<DynSetCommand>();
}

}  // namespace tbcad::block_cmds
