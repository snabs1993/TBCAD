#include <filesystem>
#include <fstream>
#include <sstream>

#include "commands.h"

namespace tbcad::sysvars {

namespace {

using cmd::CommandContext;
using cmd::Task;

namespace fs = std::filesystem;

// Reads a script file: one answer per line, ';' starts a comment line, a blank line is Enter.
// A line that starts a command may carry its answers separated by spaces.
bool readScript(CommandContext& ctx, const std::string& given, std::vector<std::string>& lines) {
    fs::path path{std::u8string(given.begin(), given.end())};
    std::error_code ec;
    if (!fs::exists(path, ec) && !path.has_extension())
        path += ".scr";
    if (!fs::exists(path, ec)) {
        const fs::path folder = ctx.host().settingsFolder();
        if (!folder.empty() && fs::exists(folder / path, ec))
            path = folder / path;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ctx.message("Cannot open the script file " + given + ".");
        return false;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    std::string text = buf.str();
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        text.erase(0, 3);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line.front() == ';')
            continue;
        lines.push_back(line);
    }
    return true;
}

class ScriptCommand final : public cmd::Command {
public:
    explicit ScriptCommand(bool repeat) : repeat_(repeat) {}
    std::string_view name() const override { return repeat_ ? "RSCRIPT" : "SCRIPT"; }
    std::string_view description() const override {
        return repeat_ ? "Run a script file over and over (Esc stops)"
                       : "Run a script file: each line is typed as if you entered it";
    }

    Task run(CommandContext& ctx) override {
        cmd::StringOptions so;
        auto file = co_await ctx.getString("Script file name", so);
        if (!file.ok() || file.text.empty())
            co_return;
        int repeat = 1;
        if (repeat_) {
            cmd::IntOptions io;
            io.defaultValue = 100;
            io.min = 1;
            io.max = 100000;
            auto n = co_await ctx.getInteger("Number of times to run the script <100>", io);
            if (!n.ok())
                co_return;
            repeat = static_cast<int>(n.value);
        }
        std::vector<std::string> lines;
        if (!readScript(ctx, file.text, lines))
            co_return;
        if (lines.empty()) {
            ctx.message("The script is empty.");
            co_return;
        }
        if (!ctx.host().queueScript(std::move(lines), repeat))
            ctx.message("Scripts cannot run here.");
    }

private:
    bool repeat_;
};

}  // namespace

std::unique_ptr<cmd::Command> makeScriptCommand(bool repeat) {
    return std::make_unique<ScriptCommand>(repeat);
}

}  // namespace tbcad::sysvars
