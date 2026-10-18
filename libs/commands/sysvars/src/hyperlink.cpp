// HYPERLINK, SPACETRANS and DELAY.
//
// HYPERLINK keeps a link (a web address or a file) and a description on an object, in
// Header::custom under "tbcad.link.<object id>". SPACETRANS converts a length between paper units
// and model units with the annotation scale. DELAY waits, for scripts.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "commands.h"

namespace tbcad::sysvars {

namespace {

using cmd::CommandContext;
using cmd::Task;

std::string linkKey(EntityId id) {
    return "tbcad.link." + std::to_string(id.value);
}

// The stored text is "<target>\n<description>".
std::string linkTarget(const std::string& stored) {
    return stored.substr(0, stored.find('\n'));
}
std::string linkDescription(const std::string& stored) {
    const std::size_t at = stored.find('\n');
    return at == std::string::npos ? std::string() : stored.substr(at + 1);
}

class HyperlinkCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "HYPERLINK"; }
    std::string_view description() const override {
        return "Attach a web address or file to an object, open it, or list the links";
    }

    Task run(CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Set", "Remove", "Open", "List"};
        ko.defaultKeyword = "Set";
        auto act = co_await ctx.getKeyword("Hyperlink [Set/Remove/Open/List] <Set>", ko);
        if (!act.ok())
            co_return;
        if (act.keyword == "List") {
            std::size_t n = 0;
            for (const auto& [key, value] : ctx.doc().header().custom) {
                if (key.rfind("tbcad.link.", 0) != 0)
                    continue;
                const std::string desc = linkDescription(value);
                ctx.message("Object " + key.substr(11) + ": " + linkTarget(value) +
                            (desc.empty() ? "" : "  (" + desc + ")"));
                ++n;
            }
            if (n == 0)
                ctx.message("No object has a hyperlink.");
            co_return;
        }
        auto e = co_await ctx.getEntity("Select an object");
        if (!e.ok())
            co_return;
        const std::string key = linkKey(e.id);
        const auto& custom = ctx.doc().header().custom;
        const auto it = custom.find(key);
        if (act.keyword == "Open") {
            if (it == custom.end()) {
                ctx.message("That object has no hyperlink.");
                co_return;
            }
            ctx.host().requestUi("open:" + linkTarget(it->second));
            ctx.message("Opening " + linkTarget(it->second));
            co_return;
        }
        if (act.keyword == "Remove") {
            if (it == custom.end()) {
                ctx.message("That object has no hyperlink.");
                co_return;
            }
            ctx.setUndoLabel("HYPERLINK");
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom.erase(key); });
            ctx.message("Hyperlink removed.");
            co_return;
        }
        cmd::StringOptions so;
        if (it != custom.end())
            so.defaultValue = linkTarget(it->second);
        auto target = co_await ctx.getString("Web address or file", so);
        if (!target.ok())
            co_return;
        cmd::StringOptions optional;
        optional.allowEmpty = true;
        auto desc = co_await ctx.getString("Description (Enter for none)", optional);
        if (desc.status == cmd::Status::Cancel)
            co_return;
        std::string text = cmd::trim(target.text);
        if (text.empty())
            co_return;
        // The text is stored on one line each.
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::string d = desc.ok() ? cmd::trim(desc.text) : std::string();
        std::replace(d.begin(), d.end(), '\n', ' ');
        ctx.setUndoLabel("HYPERLINK");
        (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[key] = text + "\n" + d; });
        ctx.message("Hyperlink set to " + text + ".");
    }
};

class SpaceTransCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SPACETRANS"; }
    std::string_view description() const override {
        return "Convert a length between paper units and model units";
    }

    Task run(CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Paper", "Model"};
        ko.defaultKeyword = "Paper";
        auto from = co_await ctx.getKeyword("Convert a length from [Paper/Model] <Paper>", ko);
        if (!from.ok())
            co_return;
        cmd::RealOptions ro;
        auto len = co_await ctx.getReal("Length", ro);
        if (!len.ok())
            co_return;
        cmd::RealOptions sc;
        sc.defaultValue = currentAnnotationScale(ctx.doc().header());
        auto scale = co_await ctx.getReal("Scale, N of 1:N", sc);
        if (!scale.ok())
            co_return;
        if (scale.value <= 0) {
            ctx.message("The scale must be above zero.");
            co_return;
        }
        const bool toModel = from.keyword == "Paper";
        const double out = toModel ? len.value * scale.value : len.value / scale.value;
        char buf[96];
        std::snprintf(buf, sizeof buf, "%.6g", out);
        ctx.message(std::string(buf) + (toModel ? " in model units." : " in paper units."));
    }
};

class DelayCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DELAY"; }
    std::string_view description() const override {
        return "Wait a number of milliseconds (up to 10000), for scripts";
    }

    Task run(CommandContext& ctx) override {
        cmd::RealOptions ro;
        ro.defaultValue = 1000.0;
        auto ms = co_await ctx.getReal("Milliseconds to wait <1000>", ro);
        if (!ms.ok())
            co_return;
        if (ms.value < 0 || ms.value > 10000) {
            ctx.message("Give a time from 0 to 10000 milliseconds.");
            co_return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(ms.value)));
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeHyperlinkCommand() {
    return std::make_unique<HyperlinkCommand>();
}
std::unique_ptr<cmd::Command> makeSpaceTransCommand() {
    return std::make_unique<SpaceTransCommand>();
}
std::unique_ptr<cmd::Command> makeDelayCommand() {
    return std::make_unique<DelayCommand>();
}

}  // namespace tbcad::sysvars
