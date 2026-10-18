#include <cmath>
#include <cstdlib>
#include <sstream>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/text/annotation_size.h"

namespace tbcad::block_cmds {

namespace {

constexpr const char* kHeightSetting = "attdef.height";

std::string formatNumber(double v) {
    std::ostringstream o;
    o << v;
    return o.str();
}

std::string settingOf(cmd::IHost& host, const char* key) {
    return host.setting(key).value_or("");
}

// ATTDEF [ATT]: defines an attribute, a piece of text that each insert of the block fills in.
// In the window it opens the Define Attribute dialog, which fills the "attdef.*" settings and runs
// ATTDEF again to pick the start point; -ATTDEF (and any host without a window) asks everything on
// the command line.
class AttDefCommand final : public cmd::Command {
public:
    explicit AttDefCommand(bool lineOnly) : lineOnly_(lineOnly) {}
    std::string_view name() const override { return lineOnly_ ? "-ATTDEF" : "ATTDEF"; }
    std::vector<std::string> defaultAliases() const override {
        return lineOnly_ ? std::vector<std::string>{} : std::vector<std::string>{"ATT"};
    }
    std::string_view description() const override { return "Define a block attribute"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        AttDefInfo info;
        const bool dialog = !lineOnly_ && ctx.host().dialogsAvailable();
        if (dialog && settingOf(ctx.host(), "tmp.attdef.go") != "1") {
            ctx.host().openPanel("attdef");
            ctx.message(
                "Use the Define Attribute panel (tag, prompt, default), then press OK and pick the "
                "start point. Type -ATTDEF to answer on the command line instead.");
            co_return;
        }
        if (dialog) {
            ctx.host().setSetting("tmp.attdef.go", "0");
            info.tag = blocks::normalizeTag(cmd::trim(settingOf(ctx.host(), "tmp.attdef.tag")));
            info.invisible = settingOf(ctx.host(), "tmp.attdef.invisible") == "1";
            info.constant = settingOf(ctx.host(), "tmp.attdef.constant") == "1";
            info.verify = settingOf(ctx.host(), "tmp.attdef.verify") == "1";
            info.preset = settingOf(ctx.host(), "tmp.attdef.preset") == "1";
            info.prompt =
                info.constant ? std::string() : settingOf(ctx.host(), "tmp.attdef.prompt");
            if (info.prompt.empty() && !info.constant)
                info.prompt = info.tag;
            info.defaultValue = settingOf(ctx.host(), "tmp.attdef.default");
            if (auto ok = blocks::validateTag(info.tag); !ok.ok()) {
                ctx.message(ok.error().message);
                co_return;
            }
        }
        for (; !dialog;) {
            auto m = co_await ctx.getKeyword(
                "Attribute mode: Invisible=" + yn(info.invisible) +
                    " Constant=" + yn(info.constant) + " Verify=" + yn(info.verify) + " Preset=" +
                    yn(info.preset) + " [Invisible/Constant/Verify/Preset/Done] <Done>",
                cmd::KeywordOptions{
                    .keywords = {"Invisible", "Constant", "Verify", "Preset", "Done"},
                    .defaultKeyword = "Done"});
            if (!m.ok())
                co_return;
            if (m.keyword == "Done")
                break;
            if (m.keyword == "Invisible")
                info.invisible = !info.invisible;
            else if (m.keyword == "Constant")
                info.constant = !info.constant;
            else if (m.keyword == "Verify")
                info.verify = !info.verify;
            else if (m.keyword == "Preset")
                info.preset = !info.preset;
        }

        for (; !dialog;) {
            auto t = co_await ctx.getString("Enter attribute tag");
            if (!t.ok())
                co_return;
            info.tag = blocks::normalizeTag(cmd::trim(t.text));
            auto ok = blocks::validateTag(info.tag);
            if (ok.ok())
                break;
            ctx.message(ok.error().message);
        }

        if (!dialog && !info.constant) {
            auto p = co_await ctx.getString("Enter attribute prompt <" + info.tag + ">",
                                            cmd::StringOptions{.allowEmpty = true});
            if (p.status == cmd::Status::Cancel)
                co_return;
            info.prompt = p.text.empty() ? info.tag : p.text;
        }
        if (!dialog) {
            auto d = co_await ctx.getString(
                info.constant ? "Enter the constant value" : "Enter default attribute value",
                cmd::StringOptions{.allowEmpty = true});
            if (d.status == cmd::Status::Cancel)
                co_return;
            info.defaultValue = d.text;
        }

        auto pos = co_await ctx.getPoint("Specify start point of the attribute text");
        if (!pos.ok())
            co_return;

        // Sized like new TEXT: 2.5 mm on a sheet-sized drawing, larger in a big plan or a far-out
        // view. Only a height the user typed is remembered.
        constexpr double kBaseHeight = 2.5;
        const double autoHeight =
            kBaseHeight * text::suggestedAnnotationScale(
                              ctx.doc(), ctx.host().view().scale,
                              static_cast<bool>(ctx.host().drawingOwner()), kBaseHeight);
        double height = autoHeight;
        if (auto last = ctx.host().setting(kHeightSetting))
            if (const double v = std::strtod(last->c_str(), nullptr); v > 0)
                height = v;
        double heightValue = height, rotation = 0.0;
        if (dialog) {  // the dialog already asked
            const std::string r = settingOf(ctx.host(), "tmp.attdef.rotation");
            rotation = std::strtod(r.c_str(), nullptr) * kPi / 180.0;
        } else {
            auto h = co_await ctx.getReal("Specify text height <" + formatNumber(height) + ">",
                                          cmd::RealOptions{.defaultValue = height, .min = 1e-9});
            if (!h.ok())
                co_return;
            auto a = co_await ctx.getAngle("Specify rotation angle <0>",
                                           cmd::AngleOptions{.defaultValue = 0.0});
            if (!a.ok())
                co_return;
            heightValue = h.value;
            rotation = a.value;
            ctx.host().setSetting(kHeightSetting, std::fabs(h.value - autoHeight) < 1e-9
                                                      ? std::string()
                                                      : formatNumber(h.value));
        }

        TextData text;
        text.position = pos.point;
        text.height = heightValue;
        text.rotation = normalizeAngle(rotation);
        text.utf8 = info.tag;
        text.attdef = info;
        Entity e;
        e.data = Indirect<TextData>(std::move(text));
        applyCurrentProperties(ctx.doc().header(), e);
        auto added = ctx.tx().add(std::move(e));
        if (!added) {
            ctx.message(added.error().message);
            co_return;
        }
        // Not left selected: BLOCK would take the lone selected attribute as its pre-selection
        // and never ask for the others.
        ctx.setUndoLabel("Define attribute " + info.tag);
        ctx.message("Attribute " + info.tag +
                    " defined. Select it with the other objects in BLOCK.");
    }

private:
    static std::string yn(bool v) { return v ? "Yes" : "No"; }
    bool lineOnly_;
};

// ATTEDIT [ATE]: changes the attribute values of one block insert.
class AttEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ATTEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"ATE"}; }
    std::string_view description() const override { return "Edit the attribute values of a block"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        EntityId target{};
        if (ctx.host().selection().size() == 1) {
            const Entity* e = doc.find(ctx.host().selection().front());
            if (e && hasAttributes(doc, *e))
                target = e->common.id;
        }
        if (!target) {
            auto pick =
                co_await ctx.getEntity("Select a block with attributes",
                                       cmd::EntityOptions{.filter = [&doc](const Entity& e) {
                                           return detail::editable(doc, e) && hasAttributes(doc, e);
                                       }});
            if (!pick.ok())
                co_return;
            target = pick.id;
        }
        const Entity* entity = doc.find(target);
        const auto* ins = entity ? entityAs<InsertData>(*entity) : nullptr;
        if (!ins)
            co_return;
        const BlockId block = ins->block;

        std::vector<AttribValue> values = blocks::syncedAttribs(doc, *ins);
        if (ctx.host().dialogsAvailable()) {
            // One small dialog (the one INSERT uses) instead of a prompt for each value.
            std::string fields;
            for (const Entity* def : blocks::attributeDefinitions(doc, block)) {
                const AttDefInfo& info = *entityAs<TextData>(*def)->attdef;
                if (info.constant)
                    continue;
                for (const AttribValue& v : values)
                    if (v.tag == info.tag)
                        fields += v.tag + "\x1f" + (info.prompt.empty() ? info.tag : info.prompt) +
                                  "\x1f" + v.value + "\x1e";
            }
            if (!fields.empty()) {
                const BlockDef* bd = doc.block(block);
                ctx.host().setSetting("tmp.insatt.block", bd ? bd->name : std::string());
                ctx.host().setSetting("tmp.insatt.fields", fields);
                ctx.host().setSetting("tmp.insatt.result", "");
                ctx.host().openPanel("attvalues");
                auto r = co_await ctx.getString(
                    "Attribute values: fill in the panel and press OK, or Enter to keep them",
                    cmd::StringOptions{.allowEmpty = true});
                ctx.host().closePanel("attvalues");
                if (!r.ok())
                    co_return;
                const std::string result = ctx.host().setting("tmp.insatt.result").value_or("");
                std::size_t at = 0;
                while (at < result.size()) {
                    const std::size_t end = result.find('\x1e', at);
                    const std::string rec = result.substr(at, end - at);
                    const std::size_t sep = rec.find('\x1f');
                    if (sep != std::string::npos)
                        for (AttribValue& v : values)
                            if (v.tag == rec.substr(0, sep))
                                v.value = rec.substr(sep + 1);
                    if (end == std::string::npos)
                        break;
                    at = end + 1;
                }
                (void)ctx.tx().modify(
                    target, [&](Entity& e) { entityAs<InsertData>(e)->attribs = values; });
                ctx.setUndoLabel("Edit attributes");
                co_return;
            }
        }
        for (const Entity* def : blocks::attributeDefinitions(doc, block)) {
            const AttDefInfo& info = *entityAs<TextData>(*def)->attdef;
            if (info.constant)
                continue;
            for (AttribValue& v : values) {
                if (v.tag != info.tag)
                    continue;
                auto r = co_await ctx.getString(
                    (info.prompt.empty() ? info.tag : info.prompt) + " <" + v.value + ">",
                    cmd::StringOptions{.defaultValue = v.value, .allowEmpty = true});
                if (!r.ok())
                    co_return;
                v.value = r.text;
            }
        }
        (void)ctx.tx().modify(target,
                              [&](Entity& e) { entityAs<InsertData>(e)->attribs = values; });
        ctx.setUndoLabel("Edit attributes");
    }

private:
    static bool hasAttributes(const Document& doc, const Entity& e) {
        const auto* ins = entityAs<InsertData>(e);
        return ins && blocks::hasEditableAttributes(doc, ins->block);
    }
};

// ATTSYNC: after a block definition changed, brings its inserts in line with its attributes.
class AttSyncCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ATTSYNC"; }
    std::vector<std::string> defaultAliases() const override { return {"SYNCHRONIZE"}; }
    std::string_view description() const override {
        return "Update the inserts of a block to its current attributes";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const BlockDef* def = nullptr;
        for (;;) {
            auto n = co_await ctx.getString("Enter block name");
            if (!n.ok())
                co_return;
            def = doc.blockByName(cmd::trim(n.text));
            if (def)
                break;
            ctx.message("No block named \"" + cmd::trim(n.text) + "\"");
        }
        std::size_t n = blocks::syncInserts(ctx.tx(), doc, def->id);
        ctx.setUndoLabel("Sync attributes");
        ctx.message(std::to_string(n) + (n == 1 ? " insert" : " inserts") + " of \"" + def->name +
                    "\" updated");
    }
};

// BATTMAN: the attribute definitions of a block, listed and edited one at a time (tag, prompt,
// default value, invisible). The inserts are brought in line afterwards, like ATTSYNC.
class BattManCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BATTMAN"; }
    std::string_view description() const override {
        return "List and edit the attribute definitions of a block";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const BlockDef* def = nullptr;
        for (;;) {
            auto n = co_await ctx.getString("Enter block name");
            if (!n.ok())
                co_return;
            def = doc.blockByName(cmd::trim(n.text));
            if (def)
                break;
            ctx.message("No block named \"" + cmd::trim(n.text) + "\"");
        }
        const BlockId block = def->id;
        list(ctx, *def);
        bool edited = false;
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Edit", "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword("Attributes [Edit/Done] <Done>", ko);
            if (!k.ok() || k.keyword == "Done")
                break;
            auto tag = co_await ctx.getString("Tag of the attribute to edit");
            if (!tag.ok())
                co_return;
            const EntityId id = find(doc, block, cmd::toUpperAscii(cmd::trim(tag.text)));
            if (!id) {
                ctx.message("That block has no attribute with the tag " + tag.text + ".");
                continue;
            }
            co_await editOne(ctx, id, edited);
        }
        if (edited) {
            const BlockDef* d = ctx.doc().block(block);
            if (d)
                blocks::syncInserts(ctx.tx(), ctx.doc(), block);
            ctx.setUndoLabel("BATTMAN");
        }
    }

private:
    static EntityId find(const Document& doc, BlockId block, const std::string& tag) {
        const BlockDef* d = doc.block(block);
        if (!d)
            return {};
        for (EntityId id : d->entities)
            if (const Entity* e = doc.find(id))
                if (const auto* t = entityAs<TextData>(*e); t && t->attdef && t->attdef->tag == tag)
                    return id;
        return {};
    }

    static void list(cmd::CommandContext& ctx, const BlockDef& d) {
        std::size_t n = 0;
        for (EntityId id : d.entities)
            if (const Entity* e = ctx.doc().find(id))
                if (const auto* t = entityAs<TextData>(*e); t && t->attdef) {
                    ctx.message(t->attdef->tag + "  prompt \"" + t->attdef->prompt +
                                "\"  default \"" + t->attdef->defaultValue + "\"" +
                                (t->attdef->invisible ? "  invisible" : ""));
                    ++n;
                }
        if (n == 0)
            ctx.message("The block \"" + d.name + "\" has no attributes.");
    }

    static cmd::Task editOne(cmd::CommandContext& ctx, EntityId id, bool& edited) {
        cmd::KeywordOptions ko;
        ko.keywords = {"Tag", "Prompt", "Default", "Invisible"};
        auto k = co_await ctx.getKeyword("Change [Tag/Prompt/Default/Invisible]", ko);
        if (!k.ok())
            co_return;
        std::string text;
        bool flag = false;
        if (k.keyword == "Invisible") {
            cmd::KeywordOptions yn;
            yn.keywords = {"Yes", "No"};
            auto v = co_await ctx.getKeyword("Invisible [Yes/No]", yn);
            if (!v.ok())
                co_return;
            flag = v.keyword == "Yes";
        } else {
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto v = co_await ctx.getString("New " + k.keyword, so);
            if (v.status == cmd::Status::Cancel)
                co_return;
            text = v.ok() ? v.text : std::string();
        }
        const std::string which = k.keyword;
        if (which == "Tag") {
            text = cmd::toUpperAscii(cmd::trim(text));
            if (text.empty() || text.find(' ') != std::string::npos) {
                ctx.message("A tag is one word.");
                co_return;
            }
        }
        auto r = ctx.tx().modify(id, [&](Entity& e) {
            auto* t = entityAs<TextData>(e);
            if (!t || !t->attdef)
                return;
            if (which == "Tag")
                t->attdef->tag = text;
            else if (which == "Prompt")
                t->attdef->prompt = text;
            else if (which == "Default")
                t->attdef->defaultValue = text;
            else
                t->attdef->invisible = flag;
        });
        if (r)
            edited = true;
        else
            ctx.message(r.error().message);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeAttDefCommand() {
    return std::make_unique<AttDefCommand>(false);
}

std::unique_ptr<cmd::Command> makeAttDefLineCommand() {
    return std::make_unique<AttDefCommand>(true);
}
std::unique_ptr<cmd::Command> makeAttEditCommand() {
    return std::make_unique<AttEditCommand>();
}
std::unique_ptr<cmd::Command> makeBattManCommand() {
    return std::make_unique<BattManCommand>();
}
std::unique_ptr<cmd::Command> makeAttSyncCommand() {
    return std::make_unique<AttSyncCommand>();
}

}  // namespace tbcad::block_cmds
