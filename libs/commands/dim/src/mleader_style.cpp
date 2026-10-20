#include "dim_internal.h"
#include "tbcad/dim/dim.h"

// Multileader styles. A style is a name, a content type and a companion dimension style
// ("MLEADER-<name>") that carries the arrowhead, the landing gap and the text style. The
// content type lives in the document header's custom map, so it is saved and undoable.
namespace tbcad::dimcmd {

namespace {

constexpr std::string_view kPrefix = "tbcad.mleaderstyle.";
constexpr const char* kCurrentKey = "tbcad.mleadercurrent";

std::string companionName(const std::string& style) {
    return "MLEADER-" + style;
}

bool validContent(std::string_view c) {
    return c == "Text" || c == "Block" || c == "None";
}

std::string canonContent(const std::string& v) {
    const std::string u = cmd::toUpperAscii(v);
    if (u == "TEXT")
        return "Text";
    if (u == "BLOCK")
        return "Block";
    if (u == "NONE")
        return "None";
    return {};
}

std::string styleKey(const std::string& name) {
    return std::string(kPrefix) + name;
}

std::vector<std::string> styleNames(const Document& doc) {
    std::vector<std::string> out;
    for (const auto& [k, v] : doc.header().custom)
        if (k.rfind(kPrefix, 0) == 0)
            out.push_back(k.substr(kPrefix.size()));
    return out;
}

std::string currentName(const Document& doc) {
    auto it = doc.header().custom.find(kCurrentKey);
    if (it == doc.header().custom.end() || !doc.header().custom.count(styleKey(it->second)))
        return {};
    return it->second;
}

class MLeaderStyleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-MLEADERSTYLE"; }
    std::string_view description() const override {
        return "Create, select, rename, edit or delete multileader styles (arrow, landing gap, "
               "text style, content type)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("Multileader style");
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Current", "New", "Rename", "Delete", "Edit", "List", "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword("Multileader style option", ko);
            if (!k.ok() || k.keyword == "Done")
                co_return;
            const std::string cur = currentName(ctx.doc());
            if (k.keyword == "List") {
                ctx.message(std::string(cur.empty() ? "* " : "  ") + "Standard");
                for (const std::string& n : styleNames(ctx.doc()))
                    ctx.message((n == cur ? "* " : "  ") + n + "  [" +
                                ctx.doc().header().custom.at(styleKey(n)) + "]");
                continue;
            }
            cmd::StringOptions so;
            auto n = co_await ctx.getString(
                k.keyword == "New" ? "Name of the new style" : "Multileader style name", so);
            if (!n.ok())
                continue;
            const std::string name = cmd::trim(n.text);
            const bool exists = ctx.doc().header().custom.count(styleKey(name)) != 0;
            const bool isStandard = name == "Standard";

            if (k.keyword == "New") {
                if (name.empty() || exists || isStandard ||
                    ctx.doc().dimStyleByName(companionName(name))) {
                    ctx.message("That name is empty or already used.");
                    continue;
                }
                // Copy the arrow, gap and text style of the style in use.
                const MLeaderStyle base = currentMLeaderStyle(ctx.doc());
                DimStyle row = dim::styleOf(ctx.doc(), base.dim);
                row.id = {};
                row.name = companionName(name);
                auto id = ctx.tx().addDimStyle(std::move(row));
                if (!id) {
                    ctx.message(id.error().message);
                    continue;
                }
                check(ctx, ctx.tx().modifyHeader([&](Header& h) {
                    h.custom[styleKey(name)] = base.content;
                    h.custom[kCurrentKey] = name;
                }));
                continue;
            }
            if (!exists && !(isStandard && k.keyword == "Current")) {
                ctx.message(isStandard ? "The Standard style cannot be changed."
                                       : "No multileader style named \"" + name + "\".");
                continue;
            }
            if (k.keyword == "Current") {
                check(ctx, ctx.tx().modifyHeader([&](Header& h) {
                    if (isStandard)
                        h.custom.erase(kCurrentKey);
                    else
                        h.custom[kCurrentKey] = name;
                }));
            } else if (k.keyword == "Rename") {
                cmd::StringOptions no;
                auto nn = co_await ctx.getString("New name", no);
                if (!nn.ok())
                    continue;
                const std::string to = cmd::trim(nn.text);
                if (to.empty() || to == "Standard" ||
                    ctx.doc().header().custom.count(styleKey(to)) ||
                    ctx.doc().dimStyleByName(companionName(to))) {
                    ctx.message("That name is empty or already used.");
                    continue;
                }
                if (const DimStyle* d = ctx.doc().dimStyleByName(companionName(name)))
                    check(ctx, ctx.tx().modifyDimStyle(
                                   d->id, [&](DimStyle& s) { s.name = companionName(to); }));
                check(ctx, ctx.tx().modifyHeader([&](Header& h) {
                    h.custom[styleKey(to)] = h.custom[styleKey(name)];
                    h.custom.erase(styleKey(name));
                    if (currentName(ctx.doc()) == name)
                        h.custom[kCurrentKey] = to;
                }));
            } else if (k.keyword == "Delete") {
                if (const DimStyle* d = ctx.doc().dimStyleByName(companionName(name))) {
                    auto r = ctx.tx().eraseDimStyle(d->id);
                    if (!r) {
                        ctx.message("Leaders still use \"" + name + "\".");
                        continue;
                    }
                }
                check(ctx, ctx.tx().modifyHeader([&](Header& h) {
                    h.custom.erase(styleKey(name));
                    if (currentName(ctx.doc()) == name)
                        h.custom.erase(kCurrentKey);
                }));
            } else if (k.keyword == "Edit") {
                for (;;) {
                    cmd::KeywordOptions po;
                    po.keywords = {"Arrow",      "ArrowSize", "Gap", "TextStyle",
                                   "TextHeight", "Content",   "Done"};
                    po.defaultKeyword = "Done";
                    auto p = co_await ctx.getKeyword("Property of \"" + name + "\"", po);
                    if (!p.ok() || p.keyword == "Done")
                        break;
                    cmd::StringOptions vo;
                    auto v = co_await ctx.getString(p.keyword + " value", vo);
                    if (!v.ok())
                        continue;
                    const std::string val = cmd::trim(v.text);
                    if (p.keyword == "Content") {
                        const std::string c = canonContent(val);
                        if (!validContent(c))
                            ctx.message("Content must be Text, Block or None.");
                        else
                            check(ctx, ctx.tx().modifyHeader(
                                           [&](Header& h) { h.custom[styleKey(name)] = c; }));
                        continue;
                    }
                    const DimStyle* d = ctx.doc().dimStyleByName(companionName(name));
                    if (!d) {
                        ctx.message("The style has no arrow settings.");
                        continue;
                    }
                    const char* key = p.keyword == "Arrow"        ? "ArrowType"
                                      : p.keyword == "ArrowSize"  ? "ArrowSize"
                                      : p.keyword == "Gap"        ? "TextGap"
                                      : p.keyword == "TextHeight" ? "TextHeight"
                                                                  : "TextStyle";
                    std::string err;
                    auto r = ctx.tx().modifyDimStyle(
                        d->id, [&](DimStyle& s) { err = applyProperty(ctx.doc(), s, key, val); });
                    if (!err.empty())
                        ctx.message(err);
                    else
                        check(ctx, r);
                }
            }
        }
    }

private:
    static void check(cmd::CommandContext& ctx, const Result<void>& r) {
        if (!r)
            ctx.message(r.error().message);
    }
};

class MLeaderStylePanelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MLEADERSTYLE"; }
    std::vector<std::string> defaultAliases() const override { return {"MLS"}; }
    std::string_view description() const override { return "Open the Multileader Style Manager"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("mleaderstyles");
        co_return;
    }
};

}  // namespace

std::vector<std::string> mleaderStyleNames(const Document& doc) {
    return styleNames(doc);
}
std::string currentMLeaderStyleName(const Document& doc) {
    return currentName(doc);
}
std::string mleaderContent(const Document& doc, const std::string& name) {
    auto it = doc.header().custom.find(styleKey(name));
    return it == doc.header().custom.end() ? std::string("Text") : it->second;
}
std::string mleaderCompanionName(const std::string& name) {
    return companionName(name);
}

MLeaderStyle currentMLeaderStyle(const Document& doc) {
    MLeaderStyle out;
    out.dim = doc.header().currentDimStyle;
    out.content = "Text";
    const std::string n = currentName(doc);
    if (n.empty())
        return out;
    out.content = doc.header().custom.at(styleKey(n));
    if (const DimStyle* d = doc.dimStyleByName(companionName(n)))
        out.dim = d->id;
    return out;
}

std::unique_ptr<cmd::Command> makeMLeaderStyleCommand() {
    return std::make_unique<MLeaderStyleCommand>();
}
std::unique_ptr<cmd::Command> makeMLeaderStylePanelCommand() {
    return std::make_unique<MLeaderStylePanelCommand>();
}

}  // namespace tbcad::dimcmd
