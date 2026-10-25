// CHECKSTANDARDS: lists what differs from a standards drawing and opens the panel with a Fix
// button per row. _FIXSTANDARDS is what the buttons run, so every fix is one undo step.
#include <algorithm>
#include <filesystem>
#include <sstream>

#include "commands.h"
#include "tbcad/layers/standards.h"
#include "tbcad/xref/xref.h"

namespace tbcad::layers {

namespace {

constexpr const char* kFileSetting = "standards.file";
constexpr const char* kKeysSetting = "tmp.standards.keys";

std::filesystem::path pathOf(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

class CheckStandardsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CHECKSTANDARDS"; }
    std::vector<std::string> defaultAliases() const override { return {"STANDARDS"}; }
    std::string_view description() const override {
        return "Compare layers, linetypes, text and dimension styles with a standards drawing";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const std::string last = ctx.host().setting(kFileSetting).value_or("");
        cmd::StringOptions so;
        so.defaultValue = last;
        auto f = co_await ctx.getString(
            last.empty() ? "Enter the standards file (.tbcad or .dxf)"
                         : "Enter the standards file (.tbcad or .dxf) <" + last + ">",
            so);
        if (!f.ok())
            co_return;
        const std::string file = cmd::trim(f.text);
        if (file.empty()) {
            ctx.message("No standards file given.");
            co_return;
        }
        auto src = xref::loadSource(pathOf(file));
        if (!src) {
            ctx.message("Cannot read " + file + ": " + src.error().message);
            co_return;
        }
        ctx.host().setSetting(kFileSetting, file);
        const auto found = checkStandards(ctx.doc(), *src.value().doc);
        if (found.empty()) {
            ctx.message("No standards violations: the drawing follows " + file + ".");
            co_return;
        }
        for (const auto& v : found)
            ctx.message(v.name + ": " + v.problem + " (" + fixLabel(v) + ")");
        ctx.message(std::to_string(found.size()) +
                    (found.size() == 1 ? " violation" : " violations") +
                    ". Use the Fix buttons in the Standards panel.");
        ctx.host().openPanel("standards");
    }
};

// _FIXSTANDARDS: fixes the violations named in "tmp.standards.keys" (keys one per line, or "*"
// for all) against the file in "standards.file". Not for typing.
class FixStandardsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "_FIXSTANDARDS"; }
    std::string_view description() const override { return {}; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const std::string keys = ctx.host().setting(kKeysSetting).value_or("");
        ctx.host().setSetting(kKeysSetting, "");
        const std::string file = ctx.host().setting(kFileSetting).value_or("");
        auto src = xref::loadSource(pathOf(file));
        if (!src) {
            ctx.message("Cannot read the standards file " + file);
            co_return;
        }
        ctx.setUndoLabel("CHECKSTANDARDS");
        std::vector<std::string> wanted;
        std::istringstream in(keys);
        for (std::string line; std::getline(in, line);)
            if (!line.empty())
                wanted.push_back(line);
        const bool all = wanted.size() == 1 && wanted[0] == "*";
        std::vector<StandardsViolation> chosen;
        for (auto& v : checkStandards(ctx.doc(), *src.value().doc))
            if (all || std::find(wanted.begin(), wanted.end(), v.key()) != wanted.end())
                chosen.push_back(std::move(v));
        const StandardsResult r = fixStandards(ctx.doc(), *src.value().doc, ctx.tx(), chosen);
        for (const auto& why : r.failed)
            ctx.message("Not fixed, " + why);
        ctx.message("Fixed " + std::to_string(r.fixed) +
                    (r.fixed == 1 ? " violation." : " violations.") +
                    (r.fixed ? " Undo brings them back." : ""));
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeCheckStandardsCommand() {
    return std::make_unique<CheckStandardsCommand>();
}
std::unique_ptr<cmd::Command> makeFixStandardsCommand() {
    return std::make_unique<FixStandardsCommand>();
}

}  // namespace tbcad::layers
