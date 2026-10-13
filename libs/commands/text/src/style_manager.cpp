// -STYLE: text styles from the command line, in the same shape as DIMSTYLE (New, Rename, Delete,
// Current, Edit, List). The Text Styles panel runs these lines, so every panel edit is one undo
// step and shows in the command history. TEXTSTYLES opens the panel.
#include <cmath>
#include <cstdlib>
#include <string>

#include "commands.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/text/text.h"

namespace tbcad::textcmd {

namespace {

using cmd::CommandContext;
using cmd::Task;

bool looksLikeFontFile(const std::string& s) {
    auto ends = [&](const char* ext) {
        const std::size_t n = std::char_traits<char>::length(ext);
        if (s.size() < n)
            return false;
        for (std::size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != ext[i])
                return false;
        return true;
    };
    return ends(".ttf") || ends(".otf") || ends(".ttc") || ends(".tbsf");
}

bool parseNumber(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && !s.empty() && std::isfinite(out);
}

void check(CommandContext& ctx, const Result<void>& r) {
    if (!r)
        ctx.message(r.error().message);
}

// Empty when the value was applied, else the reason.
std::string applyProperty(TextStyle& s, const std::string& key, const std::string& value) {
    auto same = [&](const char* k) { return cmd::toUpperAscii(key) == cmd::toUpperAscii(k); };
    if (same("Font")) {
        s.fontFile.clear();
        s.fontFamily.clear();
        if (value.empty() || cmd::toUpperAscii(value) == "DEFAULT" || value == "Sarabun")
            return {};
        if (looksLikeFontFile(value))
            s.fontFile = value;
        else
            s.fontFamily = value;
        return {};
    }
    double v = 0;
    if (!parseNumber(value, v))
        return "Not a number: " + value;
    if (same("Width")) {
        if (v < 0.01 || v > 100)
            return "The width factor must be between 0.01 and 100.";
        s.widthFactor = v;
        return {};
    }
    if (same("Oblique")) {
        if (v <= -85 || v >= 85)
            return "The oblique angle must be between -85 and 85 degrees.";
        s.oblique = v * kPi / 180.0;
        return {};
    }
    return "Unknown property " + key + " (Font, Width, Oblique).";
}

class StyleManagerCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-STYLE"; }
    std::vector<std::string> defaultAliases() const override { return {"-ST"}; }
    std::string_view description() const override {
        return "Create, rename, edit, select or delete text styles from the command line";
    }

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("Text style");
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Current", "New", "Rename", "Delete", "Edit", "List", "Done"};
            ko.defaultKeyword = "Done";
            auto k = co_await ctx.getKeyword("Text style option", ko);
            if (!k.ok() || k.keyword == "Done")
                co_return;
            if (k.keyword == "List") {
                for (const auto& [id, s] : ctx.doc().textStyles())
                    ctx.message((id == ctx.doc().header().currentTextStyle ? "* " : "  ") + s.name);
                continue;
            }
            cmd::StringOptions so;
            auto n = co_await ctx.getString(
                k.keyword == "New" ? "Name of the new style" : "Text style name", so);
            if (!n.ok())
                continue;
            const std::string name = cmd::trim(n.text);
            const TextStyle* s = ctx.doc().textStyleByName(name);

            if (k.keyword == "New") {
                if (name.empty() || s) {
                    ctx.message("That name is empty or already used.");
                    continue;
                }
                const TextStyle* cur = ctx.doc().textStyle(ctx.doc().header().currentTextStyle);
                TextStyle row = cur ? *cur : TextStyle{};
                row.id = {};
                row.name = name;
                auto id = ctx.tx().addTextStyle(std::move(row));
                if (!id)
                    ctx.message(id.error().message);
                else
                    check(ctx, ctx.tx().modifyHeader(
                                   [i = id.value()](Header& h) { h.currentTextStyle = i; }));
                continue;
            }
            if (!s) {
                ctx.message("No text style named \"" + name + "\".");
                continue;
            }
            const TextStyleId id = s->id;
            if (k.keyword == "Current") {
                check(ctx, ctx.tx().modifyHeader([id](Header& h) { h.currentTextStyle = id; }));
            } else if (k.keyword == "Rename") {
                if (id == kTextStyleStandard) {
                    ctx.message("The Standard style cannot be renamed.");
                    continue;
                }
                cmd::StringOptions no;
                auto nn = co_await ctx.getString("New name", no);
                if (!nn.ok())
                    continue;
                const std::string to = cmd::trim(nn.text);
                if (to.empty() || ctx.doc().textStyleByName(to)) {
                    ctx.message("That name is empty or already used.");
                    continue;
                }
                check(ctx, ctx.tx().modifyTextStyle(id, [&](TextStyle& row) { row.name = to; }));
            } else if (k.keyword == "Delete") {
                if (id == kTextStyleStandard) {
                    ctx.message("The Standard style cannot be deleted.");
                } else if (id == ctx.doc().header().currentTextStyle) {
                    ctx.message("The current style cannot be deleted; make another one current.");
                } else {
                    auto r = ctx.tx().eraseTextStyle(id);
                    if (!r)
                        ctx.message(r.error().code == ErrorCode::Conflict
                                        ? "Text or dimension styles still use \"" + name + "\"."
                                        : r.error().message);
                }
            } else if (k.keyword == "Edit") {
                for (;;) {
                    cmd::KeywordOptions po;
                    po.keywords = {"Font", "Width", "Oblique", "Done"};
                    po.defaultKeyword = "Done";
                    auto p = co_await ctx.getKeyword("Property of \"" + name + "\"", po);
                    if (!p.ok() || p.keyword == "Done")
                        break;
                    cmd::StringOptions vo;
                    auto v = co_await ctx.getString(p.keyword + " value", vo);
                    if (!v.ok())
                        continue;
                    std::string err;
                    auto r = ctx.tx().modifyTextStyle(id, [&](TextStyle& row) {
                        err = applyProperty(row, p.keyword, cmd::trim(v.text));
                    });
                    if (!err.empty()) {
                        ctx.message(err);
                    } else {
                        check(ctx, r);
                        const TextStyle* now = ctx.doc().textStyle(id);
                        if (now && cmd::toUpperAscii(p.keyword) == "FONT" &&
                            (!now->fontFile.empty() || !now->fontFamily.empty()) &&
                            !text::fontAvailable(now->fontFamily, now->fontFile))
                            ctx.message("Font not found; the bundled Sarabun is used instead.");
                    }
                }
            }
        }
    }
};

class TextStylesPanelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TEXTSTYLES"; }
    std::string_view description() const override { return "Open the text style manager"; }
    Task run(CommandContext& ctx) override {
        ctx.host().openPanel("textstyles");
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeStyleManagerCommand() {
    return std::make_unique<StyleManagerCommand>();
}
std::unique_ptr<cmd::Command> makeTextStylesPanelCommand() {
    return std::make_unique<TextStylesPanelCommand>();
}

}  // namespace tbcad::textcmd
