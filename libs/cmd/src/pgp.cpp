#include "tbcad/cmd/pgp.h"

#include <cctype>

#include "tbcad/cmd/command.h"

namespace tbcad::cmd {

namespace {

bool validAliasText(const std::string& a) {
    if (a.empty() || a.size() > 20)
        return false;
    for (unsigned char c : a)
        if (!std::isalnum(c) && c != '_' && c != '-')
            return false;
    return true;
}

}  // namespace

PgpParse parsePgp(std::string_view text) {
    PgpParse out;
    bool external = false;
    std::size_t lineNo = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
            end = text.size();
        std::string line(text.substr(pos, end - pos));
        pos = end + 1;
        ++lineNo;
        if (auto semi = line.find(';'); semi != std::string::npos)
            line.erase(semi);
        line = trim(line);
        if (line.empty())
            continue;
        const std::string where = "line " + std::to_string(lineNo) + ": ";
        if (line.front() == '[') {
            external = toUpperAscii(line).find("EXTERNAL") != std::string::npos;
            continue;
        }
        if (external)
            continue;  // `NOTEPAD, 1, File to edit: ,` runs a program; not supported
        auto comma = line.find(',');
        if (comma == std::string::npos) {
            out.skipped.push_back(where + "no comma, skipped");
            continue;
        }
        std::string alias = toUpperAscii(trim(line.substr(0, comma)));
        std::string rest = trim(line.substr(comma + 1));
        if (rest.empty() || rest.front() != '*') {
            out.skipped.push_back(where + alias + " does not start with *, skipped");
            continue;
        }
        rest.erase(0, 1);
        // Keep the command word only; anything after (a flag column) is dropped.
        std::size_t stop = rest.find_first_of(" \t,");
        std::string command = toUpperAscii(trim(rest.substr(0, stop)));
        if (!validAliasText(alias)) {
            out.skipped.push_back(where + "\"" + alias + "\" is not a usable alias");
            continue;
        }
        if (command.empty()) {
            out.skipped.push_back(where + alias + " has no command");
            continue;
        }
        out.aliases[alias] = command;
    }
    return out;
}

}  // namespace tbcad::cmd
