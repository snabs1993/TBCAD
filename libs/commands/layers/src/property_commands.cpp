#include <algorithm>

#include "commands.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layers/linetype_file.h"
#include "tbcad/layers/properties.h"

namespace tbcad::layers {

namespace {

using cmd::CommandContext;
using cmd::Task;

std::optional<EntityKind> kindFromName(std::string_view name) {
    std::string up = cmd::toUpperAscii(name);
    for (int i = 0; i <= static_cast<int>(EntityKind::MLine); ++i) {
        auto k = static_cast<EntityKind>(i);
        if (cmd::toUpperAscii(kindLabel(k)) == up)
            return k;
    }
    return std::nullopt;
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// -PROP: edits the common and geometry properties of the selection. The properties panel runs
// this command, so a whole panel edit (any number of objects) is one undo step.
//
//   -PROP [Only <type>] Layer <name> | Color <c> | Linetype <name> | LTScale <n> |
//         Lineweight <w> | Set <field> <value>
class PropCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-PROP"; }
    std::vector<std::string> defaultAliases() const override { return {"-PR"}; }
    std::string_view description() const override {
        return "Change properties of the selected objects from the command line";
    }

    Task run(CommandContext& ctx) override {
        ctx.setUndoLabel("PROPERTIES");
        auto sel = co_await ctx.getSelection("Select objects");
        if (!sel.ok() || sel.ids.empty())
            co_return;
        std::optional<EntityKind> only;
        cmd::KeywordOptions opts;
        opts.keywords = {"Only", "Layer", "Color", "Linetype", "LTScale", "Lineweight", "Set"};
        while (true) {
            auto k = co_await ctx.getKeyword(
                "Property [Only/Layer/Color/Linetype/LTScale/"
                "Lineweight/Set]",
                opts);
            if (!k.ok())
                co_return;
            auto v = co_await ctx.getString(k.keyword == "Only"  ? "Object type"
                                            : k.keyword == "Set" ? "Field name"
                                                                 : "New value");
            if (!v.ok())
                co_return;
            if (k.keyword == "Only") {
                if (v.text == "*" || lowerAscii(v.text) == "all") {
                    only.reset();
                } else if (auto kind = kindFromName(v.text)) {
                    only = kind;
                } else {
                    ctx.message("Unknown object type \"" + v.text + "\".");
                }
                continue;
            }
            if (k.keyword == "Set") {
                auto value = co_await ctx.getString("Value");
                if (!value.ok())
                    co_return;
                setField(ctx, sel.ids, only, lowerAscii(v.text), value.text);
                continue;
            }
            CommonEdit edit;
            bool good = true;
            if (k.keyword == "Layer") {
                auto l = resolveLayer(ctx.doc(), v.text);
                good = bool(l);
                if (good)
                    edit.layer = l.value();
                else
                    ctx.message(l.error().message);
            } else if (k.keyword == "Color") {
                auto c = parseColor(v.text);
                good = bool(c) && c.value().kind != ColorKind::ByParent;
                if (good)
                    edit.color = c.value();
                else
                    ctx.message(c ? "ByParent applies to layers only." : c.error().message);
            } else if (k.keyword == "Linetype") {
                auto lt = parseLinetype(ctx.doc(), v.text);
                good = bool(lt) && lt.value() != kLinetypeByParent;
                if (good)
                    edit.linetype = lt.value();
                else
                    ctx.message(lt ? "ByParent applies to layers only." : lt.error().message);
            } else if (k.keyword == "LTScale") {
                auto n = parseNumberExpr(v.text, /*plain=*/true);
                good = bool(n);
                if (good)
                    edit.linetypeScale = n.value();
                else
                    ctx.message(n.error().message);
            } else {
                auto lw = parseLineweight(v.text);
                good = bool(lw) && !lw.value().isByParent();
                if (good)
                    edit.lineweight = lw.value();
                else
                    ctx.message(lw ? "ByParent applies to layers only." : lw.error().message);
            }
            if (!good)
                continue;
            auto r = applyCommon(ctx.doc(), ctx.tx(), sel.ids, edit, only);
            if (!r) {
                ctx.message(r.error().message);
                continue;
            }
            if (r.value().skippedLocked)
                ctx.message(std::to_string(r.value().skippedLocked) +
                            " object(s) on locked layers were not changed.");
        }
    }

private:
    static void setField(CommandContext& ctx, const std::vector<EntityId>& ids,
                         std::optional<EntityKind> only, const std::string& field,
                         const std::string& text) {
        bool angleLike = field.find("angle") != std::string::npos;
        bool flag = field == "closed";
        double value = 0;
        if (flag) {
            std::string t = lowerAscii(text);
            value = (t == "yes" || t == "true" || t == "1" || t == "on" || t == "y") ? 1 : 0;
        } else {
            auto n = parseNumberExpr(text, angleLike);
            if (!n) {
                ctx.message(n.error().message);
                return;
            }
            value = n.value();
        }
        std::size_t changed = 0;
        for (EntityId id : ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || (only && kind(*e) != *only))
                continue;
            if (ctx.doc().effective(e->common).locked)
                continue;
            auto fields = geometryFields(*e);
            auto it = std::find_if(fields.begin(), fields.end(),
                                   [&](const GeometryField& f) { return f.id == field; });
            if (it == fields.end())
                continue;  // other kinds in a mixed selection simply do not have it
            if (!it->editable) {
                ctx.message("\"" + field + "\" is read-only.");
                return;
            }
            auto r = setGeometryField(ctx.tx(), id, field, value);
            if (!r) {
                ctx.message(r.error().message);
                return;
            }
            ++changed;
        }
        if (changed == 0)
            ctx.message("No selected object has an editable field \"" + field + "\".");
    }
};

// -LINETYPE: loads linetypes from TBCAD's linetype file (the built-in set or a file on disk).
//   -LINETYPE Load <names|*>    -LINETYPE File <path> <names|*>    -LINETYPE List
class LinetypeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-LINETYPE"; }
    std::vector<std::string> defaultAliases() const override { return {"-LT"}; }
    std::string_view description() const override { return "Load and list linetypes"; }

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();
        cmd::KeywordOptions opts;
        opts.keywords = {"Load", "File", "List"};
        while (true) {
            auto k = co_await ctx.getKeyword("Linetype option", opts);
            if (!k.ok())
                co_return;
            if (k.keyword == "List") {
                for (const auto& [id, lt] : ctx.doc().linetypes())
                    ctx.message(lt.name + (lt.description.empty() ? "" : "  " + lt.description));
                continue;
            }
            LinetypeFile file;
            if (k.keyword == "File") {
                auto path = co_await ctx.getString("Linetype file");
                if (!path.ok())
                    co_return;
                auto f = readLinetypeFile(std::filesystem::path(path.text));
                if (!f) {
                    ctx.message(f.error().message);
                    continue;
                }
                file = std::move(f.value());
            } else {
                file = parseLinetypes(builtinLinetypeText());
            }
            for (const std::string& p : file.problems)
                ctx.message("Linetype file: " + p);
            auto names = co_await ctx.getString("Linetype name(s), separated by commas, * for all");
            if (!names.ok())
                co_return;
            std::vector<std::string> list;
            std::size_t pos = 0;
            while (pos <= names.text.size()) {
                std::size_t next = names.text.find(',', pos);
                std::string part = cmd::trim(names.text.substr(
                    pos, next == std::string::npos ? std::string::npos : next - pos));
                if (!part.empty())
                    list.push_back(part);
                if (next == std::string::npos)
                    break;
                pos = next + 1;
            }
            auto r = loadLinetypes(ctx.doc(), ctx.tx(), file, list, LoadMode::SkipExisting);
            if (!r) {
                ctx.message(r.error().message);
                continue;
            }
            ctx.message(r.value().empty()
                            ? "Nothing to load: those linetypes are already loaded."
                            : "Loaded " + std::to_string(r.value().size()) + " linetype(s).");
        }
    }
};

// -CURRENT: the colour, linetype and lineweight that new objects get (ByLayer by default).
//   -CURRENT Color <c> | Linetype <name> | Lineweight <w> | List
class CurrentCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-CURRENT"; }
    std::vector<std::string> defaultAliases() const override { return {"-CUR"}; }
    std::string_view description() const override {
        return "Set the colour, linetype and lineweight of new objects";
    }

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("CURRENT PROPERTIES");
        cmd::KeywordOptions opts;
        opts.keywords = {"Color", "Linetype", "Lineweight", "List"};
        while (true) {
            auto k = co_await ctx.getKeyword("Current property", opts);
            if (!k.ok())
                co_return;
            if (k.keyword == "List") {
                const Header& h = ctx.doc().header();
                ctx.message("Colour " + colorName(h.currentColor) + ", linetype " +
                            linetypeName(ctx.doc(), h.currentLinetype) + ", lineweight " +
                            lineweightName(h.currentLineweight));
                continue;
            }
            auto v = co_await ctx.getString(
                k.keyword == "Color"      ? "Colour (ByLayer, ByBlock, name, 1-255 or r,g,b)"
                : k.keyword == "Linetype" ? "Linetype (ByLayer, ByBlock or a loaded name)"
                                          : "Lineweight (ByLayer, ByBlock, Default or mm)");
            if (!v.ok())
                co_return;
            Result<void> r;
            if (k.keyword == "Color") {
                auto c = parseColor(v.text);
                if (!c) {
                    ctx.message(c.error().message);
                    continue;
                }
                r = ctx.tx().modifyHeader([&](Header& h) { h.currentColor = c.value(); });
            } else if (k.keyword == "Linetype") {
                auto lt = parseLinetype(ctx.doc(), v.text);
                if (!lt) {
                    ctx.message(lt.error().message);
                    continue;
                }
                r = ctx.tx().modifyHeader([&](Header& h) { h.currentLinetype = lt.value(); });
            } else {
                auto lw = parseLineweight(v.text);
                if (!lw) {
                    ctx.message(lw.error().message);
                    continue;
                }
                r = ctx.tx().modifyHeader([&](Header& h) { h.currentLineweight = lw.value(); });
            }
            if (!r)
                ctx.message(r.error().message);
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeCurrentCommand() {
    return std::make_unique<CurrentCommand>();
}
std::unique_ptr<cmd::Command> makePropCommand() {
    return std::make_unique<PropCommand>();
}
std::unique_ptr<cmd::Command> makeLinetypeCommand() {
    return std::make_unique<LinetypeCommand>();
}

}  // namespace tbcad::layers
