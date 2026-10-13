#include <algorithm>
#include <cmath>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"

namespace tbcad::block_cmds {

namespace {

const BlockDef* findAssemblyBlock(cmd::CommandContext& ctx, const std::string& name) {
    const BlockDef* def = ctx.doc().blockByName(name);
    if (!def) {
        ctx.message("There is no block called \"" + name + "\"");
        return nullptr;
    }
    if (!def->xrefPath.empty()) {
        ctx.message("An external reference cannot be made an assembly");
        return nullptr;
    }
    return def;
}

std::string kindName(AsmParam::Kind k) {
    switch (k) {
        case AsmParam::Kind::Number:
            return "number";
        case AsmParam::Kind::Integer:
            return "integer";
        case AsmParam::Kind::Text:
            return "text";
        case AsmParam::Kind::Choice:
            return "choice";
    }
    return "";
}

std::string show(const AsmParam& p, const AsmValue& v) {
    if (p.kind == AsmParam::Kind::Text || p.kind == AsmParam::Kind::Choice)
        return "\"" + v.text + "\"";
    return expr::Value::ofNumber(v.number).toString();
}

// ASMDEF: defines the parameters, bindings and presets of an assembly block.
class AsmDefCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ASMDEF"; }
    std::string_view description() const override {
        return "Add a parameter, formula binding or preset to an assembly block";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto blockName = co_await ctx.getString("Enter block name");
        if (!blockName.ok())
            co_return;
        const BlockDef* def = findAssemblyBlock(ctx, cmd::trim(blockName.text));
        if (!def)
            co_return;
        const BlockId block = def->id;
        auto what = co_await ctx.getKeyword(
            "Option [Parameter/Bind/Preset/Check] <Parameter>",
            cmd::KeywordOptions{.keywords = {"Parameter", "Bind", "Preset", "Check"},
                                .defaultKeyword = "Parameter"});
        if (!what.ok())
            co_return;

        if (what.keyword == "Check") {
            auto problems = blocks::asmValidate(ctx.doc(), *def);
            if (problems.empty())
                ctx.message("The assembly is sound");
            for (const std::string& s : problems)
                ctx.message(s);
            co_return;
        }

        if (what.keyword == "Parameter") {
            auto pname = co_await ctx.getString("Enter parameter name");
            if (!pname.ok())
                co_return;
            AsmParam p;
            p.name = cmd::trim(pname.text);
            if (p.name.empty() || (def->assembly && blocks::asmFind(*def->assembly, p.name) >= 0)) {
                ctx.message("The name is empty or already used");
                co_return;
            }
            auto type = co_await ctx.getKeyword(
                "Type [Number/Integer/Text/Choice] <Number>",
                cmd::KeywordOptions{.keywords = {"Number", "Integer", "Text", "Choice"},
                                    .defaultKeyword = "Number"});
            if (!type.ok())
                co_return;
            if (type.keyword == "Number" || type.keyword == "Integer") {
                p.kind = type.keyword == "Number" ? AsmParam::Kind::Number
                                                  : AsmParam::Kind::Integer;
                auto d = co_await ctx.getReal("Default value <0>",
                                              cmd::RealOptions{.defaultValue = 0.0});
                if (!d.ok())
                    co_return;
                auto lo = co_await ctx.getReal("Minimum, equal to the maximum for no limit <0>",
                                               cmd::RealOptions{.defaultValue = 0.0});
                if (!lo.ok())
                    co_return;
                auto hi = co_await ctx.getReal("Maximum <0>", cmd::RealOptions{.defaultValue = 0.0});
                if (!hi.ok())
                    co_return;
                p.defaultNumber = d.value;
                if (hi.value > lo.value) {
                    p.min = lo.value;
                    p.max = hi.value;
                }
            } else if (type.keyword == "Text") {
                p.kind = AsmParam::Kind::Text;
                auto d = co_await ctx.getString("Default text",
                                                cmd::StringOptions{.allowEmpty = true});
                if (d.status == cmd::Status::Cancel)
                    co_return;
                p.defaultText = d.text;
            } else {
                p.kind = AsmParam::Kind::Choice;
                for (;;) {
                    auto c = co_await ctx.getString(
                        p.choices.empty() ? "Enter the first choice (the default)"
                                          : "Enter the next choice, Enter to finish",
                        cmd::StringOptions{.allowEmpty = !p.choices.empty()});
                    if (!c.ok() && c.status != cmd::Status::None)
                        co_return;
                    const std::string t = cmd::trim(c.text);
                    if (t.empty())
                        break;
                    p.choices.push_back(t);
                }
                if (p.choices.empty()) {
                    ctx.message("A choice parameter needs at least one choice");
                    co_return;
                }
                p.defaultText = p.choices.front();
            }
            const std::string added = p.name;
            (void)ctx.tx().modifyBlock(block, [&](BlockDef& d) {
                if (!d.assembly)
                    d.assembly = AssemblyDef{};
                d.assembly->params.push_back(std::move(p));
            });
            ctx.message("Added the parameter " + added);
            ctx.setUndoLabel("Add assembly parameter");
            co_return;
        }

        if (!def->assembly || def->assembly->params.empty()) {
            ctx.message("Add a parameter first (ASMDEF Parameter)");
            co_return;
        }

        if (what.keyword == "Bind") {
            auto kind = co_await ctx.getKeyword(
                "Drive [Dynamic/Attribute/Nested/Array] <Dynamic>",
                cmd::KeywordOptions{.keywords = {"Dynamic", "Attribute", "Nested", "Array"},
                                    .defaultKeyword = "Dynamic"});
            if (!kind.ok())
                co_return;
            AsmBinding b;
            if (kind.keyword == "Dynamic")
                b.kind = AsmBinding::Kind::DynParam;
            else if (kind.keyword == "Attribute")
                b.kind = AsmBinding::Kind::Attribute;
            else if (kind.keyword == "Nested")
                b.kind = AsmBinding::Kind::Nested;
            else
                b.kind = AsmBinding::Kind::ArrayField;
            if (b.kind == AsmBinding::Kind::Nested || b.kind == AsmBinding::Kind::ArrayField) {
                auto idx = co_await ctx.getInteger(
                    "Number of the nested insert in the block (1 = first object)",
                    cmd::IntOptions{.defaultValue = 1});
                if (!idx.ok())
                    co_return;
                if (idx.value < 1 || static_cast<std::size_t>(idx.value) > def->entities.size()) {
                    ctx.message("The block has no such object");
                    co_return;
                }
                b.entity = def->entities[static_cast<std::size_t>(idx.value) - 1];
                const Entity* e = ctx.doc().find(b.entity);
                if (!e || !entityAs<InsertData>(*e)) {
                    ctx.message("That object is not an inserted block");
                    co_return;
                }
            }
            const char* prompt = b.kind == AsmBinding::Kind::DynParam   ? "Dynamic parameter name"
                                 : b.kind == AsmBinding::Kind::Attribute ? "Attribute tag"
                                 : b.kind == AsmBinding::Kind::Nested
                                     ? "Parameter name of the nested block"
                                     : "Array field (rows, columns, count, rowSpacing, "
                                       "columnSpacing, pathSpacing)";
            auto target = co_await ctx.getString(prompt);
            if (!target.ok())
                co_return;
            auto formula = co_await ctx.getString("Enter the formula");
            if (!formula.ok())
                co_return;
            b.target = cmd::trim(target.text);
            b.formula = cmd::trim(formula.text);
            if (!expr::Expression::parse(b.formula).ok()) {
                ctx.message("The formula cannot be read");
                co_return;
            }
            (void)ctx.tx().modifyBlock(
                block, [&](BlockDef& d) { d.assembly->bindings.push_back(std::move(b)); });
            ctx.message("Added the binding");
            ctx.setUndoLabel("Add assembly binding");
            co_return;
        }

        // Preset: a value for every parameter.
        auto pname = co_await ctx.getString("Enter preset name");
        if (!pname.ok())
            co_return;
        AsmPreset preset;
        preset.name = cmd::trim(pname.text);
        if (preset.name.empty()) {
            ctx.message("The name is empty");
            co_return;
        }
        const AssemblyDef asmDef = *def->assembly;
        for (const AsmParam& p : asmDef.params) {
            AsmValue v{p.name, false, p.defaultNumber, p.defaultText};
            if (p.kind == AsmParam::Kind::Number || p.kind == AsmParam::Kind::Integer) {
                auto r = co_await ctx.getReal(p.name + " <" + show(p, v) + ">",
                                              cmd::RealOptions{.defaultValue = p.defaultNumber});
                if (!r.ok())
                    co_return;
                v.number = r.value;
            } else {
                v.isText = true;
                auto r = co_await ctx.getString(p.name + " <" + p.defaultText + ">",
                                                cmd::StringOptions{.allowEmpty = true});
                if (r.status == cmd::Status::Cancel)
                    co_return;
                v.text = cmd::trim(r.text).empty() ? p.defaultText : cmd::trim(r.text);
            }
            preset.values.push_back(std::move(v));
        }
        (void)ctx.tx().modifyBlock(block, [&](BlockDef& d) {
            auto& list = d.assembly->presets;
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [&](const AsmPreset& q) { return q.name == preset.name; }),
                       list.end());
            list.push_back(std::move(preset));
        });
        ctx.message("Saved the preset");
        ctx.setUndoLabel("Add assembly preset");
    }
};

// ASMSET: sets one parameter of an assembly insert, or applies a preset.
class AsmSetCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ASMSET"; }
    std::string_view description() const override {
        return "Set a parameter of an inserted assembly, or apply a preset";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        cmd::EntityOptions eo;
        eo.filter = [&doc](const Entity& e) {
            const auto* ins = entityAs<InsertData>(e);
            const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
            return def && def->assembly;
        };
        auto picked = co_await ctx.getEntity("Select an assembly", eo);
        if (!picked.ok())
            co_return;
        const Entity* e = doc.find(picked.id);
        const auto* ins = entityAs<InsertData>(*e);
        const BlockDef* def = doc.block(ins->block);
        const AssemblyDef& ad = *def->assembly;
        const auto now = blocks::asmEffectiveValues(ad, *ins);
        std::string list;
        for (const AsmParam& p : ad.params)
            list += (list.empty() ? "" : "/") + p.name;
        auto pname = co_await ctx.getString("Parameter [" + list + (ad.presets.empty() ? "" : "/Preset") + "]");
        if (!pname.ok())
            co_return;
        const std::string wanted = cmd::trim(pname.text);

        Result<Entity> changed = makeError(ErrorCode::NotFound, "No such parameter");
        if (!ad.presets.empty() && (wanted == "Preset" || wanted == "preset")) {
            std::string names;
            for (const AsmPreset& p : ad.presets)
                names += (names.empty() ? "" : "/") + p.name;
            auto k = co_await ctx.getKeyword(
                "Preset [" + names + "]",
                cmd::KeywordOptions{.keywords = [&] {
                    std::vector<std::string> v;
                    for (const AsmPreset& p : ad.presets)
                        v.push_back(p.name);
                    return v;
                }()});
            if (!k.ok())
                co_return;
            changed = blocks::asmApplyPreset(doc, *e, k.keyword);
        } else {
            const int pi = blocks::asmFind(ad, wanted);
            if (pi < 0) {
                ctx.message("No such parameter");
                co_return;
            }
            const AsmParam& p = ad.params[static_cast<std::size_t>(pi)];
            const AsmValue& cur = now[static_cast<std::size_t>(pi)];
            expr::Value value;
            if (p.kind == AsmParam::Kind::Number || p.kind == AsmParam::Kind::Integer) {
                auto r = co_await ctx.getReal("Enter " + p.name + " <" + show(p, cur) + ">",
                                              cmd::RealOptions{.defaultValue = cur.number});
                if (!r.ok())
                    co_return;
                value = expr::Value::ofNumber(r.value);
            } else if (p.kind == AsmParam::Kind::Choice) {
                std::string choices;
                for (const std::string& c : p.choices)
                    choices += (choices.empty() ? "" : "/") + c;
                auto r = co_await ctx.getKeyword(
                    p.name + " [" + choices + "]",
                    cmd::KeywordOptions{.keywords = p.choices, .defaultKeyword = cur.text});
                if (!r.ok())
                    co_return;
                value = expr::Value::ofText(r.keyword);
            } else {
                auto r = co_await ctx.getString("Enter " + p.name + " <" + cur.text + ">",
                                                cmd::StringOptions{.allowEmpty = true});
                if (r.status == cmd::Status::Cancel)
                    co_return;
                value = expr::Value::ofText(cmd::trim(r.text).empty() ? cur.text : r.text);
            }
            changed = blocks::asmSet(doc, *e, p.name, value);
        }
        if (!changed) {
            ctx.message(changed.error().message);
            co_return;
        }
        (void)ctx.tx().modify(picked.id,
                              [&](Entity& target) { target = std::move(changed).value(); });
        ctx.setUndoLabel("Set assembly parameter");
    }
};

// ASMINFO: lists the parameters and their values of an assembly insert.
class AsmInfoCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ASMINFO"; }
    std::string_view description() const override {
        return "List the parameters, values and problems of an inserted assembly";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        cmd::EntityOptions eo;
        eo.filter = [&doc](const Entity& e) {
            const auto* ins = entityAs<InsertData>(e);
            const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
            return def && def->assembly;
        };
        auto picked = co_await ctx.getEntity("Select an assembly", eo);
        if (!picked.ok())
            co_return;
        const auto* ins = entityAs<InsertData>(*doc.find(picked.id));
        const BlockDef* def = doc.block(ins->block);
        const auto values = blocks::asmEffectiveValues(*def->assembly, *ins);
        ctx.message("Assembly " + def->name);
        for (std::size_t i = 0; i < values.size(); ++i) {
            const AsmParam& p = def->assembly->params[i];
            ctx.message("  " + p.name + " (" + kindName(p.kind) + ") = " + show(p, values[i]));
        }
        const auto res = blocks::resolveAssembly(doc, *def, *ins);
        for (const std::string& err : res.errors)
            ctx.message("  problem: " + err);
    }
};

// -ASMSET: sets a parameter on every selected assembly insert (or applies a preset). The
// properties panel runs this command, so one panel edit is one undo step.
//   -ASMSET <parameter> <value>      -ASMSET @preset <name>
class AsmSetSelectedCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-ASMSET"; }
    std::string_view description() const override {
        return "Change a parameter of the selected assemblies from the command line";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("Set assembly parameter");
        Document& doc = ctx.doc();
        auto sel = co_await ctx.getSelection("Select assemblies");
        if (!sel.ok() || sel.ids.empty())
            co_return;
        auto pname = co_await ctx.getString("Parameter");
        if (!pname.ok())
            co_return;
        auto value = co_await ctx.getString("Value", cmd::StringOptions{.allowEmpty = true});
        if (value.status == cmd::Status::Cancel)
            co_return;
        const std::string name = cmd::trim(pname.text);
        std::size_t changed = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = doc.find(id);
            const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
            const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
            if (!def || !def->assembly)
                continue;
            Result<Entity> r = makeError(ErrorCode::NotFound, "");
            if (name == "@preset") {
                r = blocks::asmApplyPreset(doc, *e, cmd::trim(value.text));
            } else {
                const int pi = blocks::asmFind(*def->assembly, name);
                if (pi < 0)
                    continue;
                const AsmParam& p = def->assembly->params[static_cast<std::size_t>(pi)];
                if (p.kind == AsmParam::Kind::Number || p.kind == AsmParam::Kind::Integer) {
                    auto parsed = expr::Expression::parse(value.text);
                    auto v = parsed ? parsed.value().eval([](std::string_view) {
                        return std::optional<expr::Value>{};
                    })
                                    : Result<expr::Value>(parsed.error());
                    if (!v || v.value().isText) {
                        ctx.message("\"" + value.text + "\" is not a number");
                        continue;
                    }
                    r = blocks::asmSet(doc, *e, p.name, v.value());
                } else {
                    r = blocks::asmSet(doc, *e, p.name, expr::Value::ofText(value.text));
                }
            }
            if (!r) {
                ctx.message(r.error().message);
                continue;
            }
            (void)ctx.tx().modify(id, [&](Entity& target) { target = std::move(r).value(); });
            ++changed;
        }
        if (changed == 0)
            ctx.message("Nothing changed");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeAsmDefCommand() {
    return std::make_unique<AsmDefCommand>();
}
std::unique_ptr<cmd::Command> makeAsmSetCommand() {
    return std::make_unique<AsmSetCommand>();
}
std::unique_ptr<cmd::Command> makeAsmSetSelectedCommand() {
    return std::make_unique<AsmSetSelectedCommand>();
}
std::unique_ptr<cmd::Command> makeAsmInfoCommand() {
    return std::make_unique<AsmInfoCommand>();
}

}  // namespace tbcad::block_cmds
