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
                    " Constant=" + yn(info.constant) + " Verify=" + yn(info.verify) +
                    " Preset=" + yn(info.preset) + " Lock=" + yn(info.lockPosition) +
                    " [Invisible/Constant/Verify/Preset/Lock position/Done] <Done>",
                cmd::KeywordOptions{
                    .keywords = {"Invisible", "Constant", "Verify", "Preset", "Lock", "Done"},
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
            else if (m.keyword == "Lock")
                info.lockPosition = !info.lockPosition;
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
    std::vector<std::string> defaultAliases() const override {
        return {"ATE", "EATTEDIT", "ATTIPEDIT"};
    }
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

// The attribute definitions of a block, in order.
std::vector<const TextData*> attDefs(const Document& doc, BlockId block) {
    std::vector<const TextData*> out;
    if (const BlockDef* d = doc.block(block))
        for (EntityId id : d->entities)
            if (const Entity* e = doc.find(id))
                if (const auto* t = entityAs<TextData>(*e); t && t->attdef)
                    out.push_back(t);
    return out;
}

// Asks for a block as AutoCAD's ATTSYNC and BATTMAN do: an insert picked in the drawing, a block
// name typed at the prompt, Name (then the name), or ? (lists the blocks with attributes and asks
// again). `out` stays null when the user cancels.
cmd::Task chooseBlock(cmd::CommandContext& ctx, BlockId& out) {
    Document& doc = ctx.doc();
    out = {};
    for (;;) {
        cmd::EntityOptions eo;
        eo.keywords = {"?", "Name"};
        // A block name typed at the prompt works as well as the Name option.
        for (const auto& [id, b] : doc.blocks())
            if (!b.name.empty() && b.name.front() != '*' && eo.keywords.size() < 400)
                eo.keywords.push_back(b.name);
        eo.filter = [&doc](const Entity& e) {
            const auto* ins = entityAs<InsertData>(e);
            return ins && doc.block(ins->block);
        };
        auto pick = co_await ctx.getEntity("Select a block reference or [?/Name]", eo);
        if (pick.status == cmd::Status::Keyword) {
            if (pick.keyword == "?") {
                std::size_t n = 0;
                for (const auto& [id, b] : doc.blocks())
                    if (!b.name.empty() && b.name.front() != '*' && !attDefs(doc, id).empty()) {
                        ctx.message("  " + b.name + " (" + std::to_string(attDefs(doc, id).size()) +
                                    " attributes)");
                        ++n;
                    }
                if (n == 0)
                    ctx.message("No block in this drawing has attributes.");
                continue;
            }
            std::string wanted = pick.keyword;
            if (wanted == "Name") {
                auto name = co_await ctx.getString("Enter block name");
                if (!name.ok())
                    co_return;
                wanted = cmd::trim(name.text);
            }
            const BlockDef* def = doc.blockByName(wanted);
            if (!def) {
                ctx.message("No block named \"" + wanted + "\"");
                continue;
            }
            out = def->id;
            co_return;
        }
        if (!pick.ok())
            co_return;
        const Entity* e = doc.find(pick.id);
        if (const auto* ins = e ? entityAs<InsertData>(*e) : nullptr) {
            out = ins->block;
            co_return;
        }
    }
}

// ATTSYNC: after a block definition changed, brings its inserts in line with its attributes.
// The block is picked through one of its inserts or named (? lists the blocks with attributes).
class AttSyncCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ATTSYNC"; }
    std::vector<std::string> defaultAliases() const override { return {"SYNCHRONIZE"}; }
    std::string_view description() const override {
        return "Update the inserts of a block to its current attributes";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        BlockId block;
        co_await chooseBlock(ctx, block);
        const BlockDef* def = doc.block(block);
        if (!def)
            co_return;
        if (attDefs(doc, block).empty()) {
            ctx.message("The block \"" + def->name + "\" has no attributes.");
            co_return;
        }
        std::size_t n = blocks::syncInserts(ctx.tx(), doc, block);
        ctx.setUndoLabel("Sync attributes");
        ctx.message(std::to_string(n) + (n == 1 ? " insert" : " inserts") + " of \"" + def->name +
                    "\" updated");
    }
};

// BATTMAN: the attribute definitions of a block, listed and edited one at a time, as in
// AutoCAD's Block Attribute Manager: Edit (tag, prompt, default value and the Invisible,
// Constant, Verify and Preset modes), Remove, Up and Down (the order inserts ask for and show the
// values). The inserts are brought in line afterwards, like ATTSYNC; the whole session is one undo
// step.
class BattManCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BATTMAN"; }
    std::string_view description() const override {
        return "List and edit the attribute definitions of a block";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        BlockId block;
        co_await chooseBlock(ctx, block);
        const BlockDef* def = doc.block(block);
        if (!def)
            co_return;
        if (!def->xrefPath.empty()) {
            ctx.message("The attributes of an external reference cannot be edited here.");
            co_return;
        }
        list(ctx, *def);
        bool edited = false;
        std::vector<std::pair<std::string, std::string>> renames;
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Edit", "Remove", "Up", "Down", "List", "Done"};
            ko.defaultKeyword = "Done";
            auto k =
                co_await ctx.getKeyword("Attributes [Edit/Remove/Up/Down/List/Done] <Done>", ko);
            if (!k.ok() || k.keyword == "Done")
                break;
            if (k.keyword == "List") {
                if (const BlockDef* d = ctx.doc().block(block))
                    list(ctx, *d);
                continue;
            }
            auto tag = co_await ctx.getString(std::string("Tag of the attribute to ") +
                                              (k.keyword == "Edit"     ? "edit"
                                               : k.keyword == "Remove" ? "remove"
                                                                       : "move"));
            if (!tag.ok())
                co_return;
            const EntityId id = find(doc, block, cmd::toUpperAscii(cmd::trim(tag.text)));
            if (!id) {
                ctx.message("That block has no attribute with the tag " + tag.text + ".");
                continue;
            }
            if (k.keyword == "Edit") {
                co_await editOne(ctx, id, edited, renames);
            } else if (k.keyword == "Remove") {
                if (auto r = ctx.tx().erase(id); r) {
                    edited = true;
                    ctx.message("Removed " + cmd::toUpperAscii(cmd::trim(tag.text)) + ".");
                } else {
                    ctx.message(r.error().message);
                }
            } else if (move(ctx, block, id, k.keyword == "Up")) {
                edited = true;
            }
        }
        if (edited) {
            if (ctx.doc().block(block)) {
                const std::size_t n = blocks::syncInserts(ctx.tx(), ctx.doc(), block, renames);
                if (n > 0)
                    ctx.message(std::to_string(n) + (n == 1 ? " insert" : " inserts") +
                                " updated.");
            }
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

    static BlockId blockOf(const Document& doc, EntityId id) { return doc.ownerBlock(id); }

    // Swaps the attribute with the one before (up) or after it among the block's attributes; the
    // other objects of the block keep their places.
    static bool move(cmd::CommandContext& ctx, BlockId block, EntityId id, bool up) {
        const BlockDef* d = ctx.doc().block(block);
        if (!d)
            return false;
        std::vector<EntityId> order = d->entities;
        std::vector<std::size_t> slots;  // positions of the attribute definitions
        for (std::size_t i = 0; i < order.size(); ++i)
            if (const Entity* e = ctx.doc().find(order[i]))
                if (const auto* t = entityAs<TextData>(*e); t && t->attdef)
                    slots.push_back(i);
        std::size_t k = 0;
        while (k < slots.size() && order[slots[k]] != id)
            ++k;
        if (k == slots.size())
            return false;
        if (up ? k == 0 : k + 1 == slots.size()) {
            ctx.message(std::string("That attribute is already ") + (up ? "first." : "last."));
            return false;
        }
        std::swap(order[slots[k]], order[slots[up ? k - 1 : k + 1]]);
        if (auto r = ctx.tx().setDrawOrder(order, block); !r) {
            ctx.message(r.error().message);
            return false;
        }
        return true;
    }

    static void list(cmd::CommandContext& ctx, const BlockDef& d) {
        std::size_t n = 0;
        for (const TextData* t : attDefs(ctx.doc(), d.id)) {
            const AttDefInfo& a = *t->attdef;
            std::string modes;
            for (auto [on, name] :
                 {std::pair{a.invisible, "invisible"}, std::pair{a.constant, "constant"},
                  std::pair{a.verify, "verify"}, std::pair{a.preset, "preset"},
                  std::pair{a.lockPosition, "locked"}})
                if (on)
                    modes += std::string("  ") + name;
            ctx.message(a.tag + "  prompt \"" + a.prompt + "\"  default \"" + a.defaultValue +
                        "\"" + modes);
            ++n;
        }
        if (n == 0)
            ctx.message("The block \"" + d.name + "\" has no attributes.");
    }

    static cmd::Task editOne(cmd::CommandContext& ctx, EntityId id, bool& edited,
                             std::vector<std::pair<std::string, std::string>>& renames) {
        cmd::KeywordOptions ko;
        ko.keywords = {"Tag", "Prompt", "Default", "Invisible", "Constant", "Verify", "Preset"};
        auto k = co_await ctx.getKeyword(
            "Change [Tag/Prompt/Default/Invisible/Constant/Verify/Preset]", ko);
        if (!k.ok())
            co_return;
        const std::string which = k.keyword;
        const bool isMode =
            which == "Invisible" || which == "Constant" || which == "Verify" || which == "Preset";
        std::string text;
        bool flag = false;
        if (isMode) {
            cmd::KeywordOptions yn;
            yn.keywords = {"Yes", "No"};
            auto v = co_await ctx.getKeyword(which + " [Yes/No]", yn);
            if (!v.ok())
                co_return;
            flag = v.keyword == "Yes";
        } else {
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto v = co_await ctx.getString("New " + which, so);
            if (v.status == cmd::Status::Cancel)
                co_return;
            text = v.ok() ? v.text : std::string();
        }
        if (which == "Tag") {
            text = cmd::toUpperAscii(cmd::trim(text));
            if (text.empty() || text.find(' ') != std::string::npos) {
                ctx.message("A tag is one word.");
                co_return;
            }
            // (two attributes of a block cannot share a tag)
            const EntityId other = find(ctx.doc(), blockOf(ctx.doc(), id), text);
            if (other && other != id) {
                ctx.message("The block already has an attribute with the tag " + text + ".");
                co_return;
            }
        }
        auto r = ctx.tx().modify(id, [&](Entity& e) {
            auto* t = entityAs<TextData>(e);
            if (!t || !t->attdef)
                return;
            AttDefInfo& a = *t->attdef;
            if (which == "Tag") {
                if (a.tag != text)
                    renames.emplace_back(a.tag, text);
                a.tag = text;
            } else if (which == "Prompt") {
                a.prompt = text;
            } else if (which == "Default") {
                a.defaultValue = text;
            } else if (which == "Invisible") {
                a.invisible = flag;
            } else if (which == "Constant") {
                // a constant attribute is never asked for, so it cannot also verify or preset
                a.constant = flag;
                if (flag)
                    a.verify = a.preset = false;
            } else if (which == "Verify") {
                a.verify = flag && !a.constant;
            } else {
                a.preset = flag && !a.constant;
            }
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
