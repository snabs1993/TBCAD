#include <string>

#include "tbcad/cmd/registry.h"

namespace tbcad::cmd {

namespace {

// Table cells cannot contain a pipe or a line break.
std::string cell(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '|')
            out += "\\|";
        else if (c == '\n' || c == '\r')
            out += ' ';
        else
            out += c;
    }
    return out;
}

}  // namespace

std::string commandListMarkdown(const Registry& registry) {
    std::string out = "| Command | Aliases | What it does |\n| --- | --- | --- |\n";
    for (Command* c : registry.list()) {
        if (c->description().empty())
            continue;
        std::string aliases;
        for (const std::string& a : registry.aliasesOf(c->name())) {
            if (!aliases.empty())
                aliases += ", ";
            aliases += a;
        }
        out += "| " + cell(c->name()) + " | " + cell(aliases) + " | " + cell(c->description()) +
               " |\n";
    }
    return out;
}

}  // namespace tbcad::cmd
