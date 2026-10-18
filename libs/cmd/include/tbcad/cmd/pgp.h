#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tbcad::cmd {

// An AutoCAD acad.pgp style alias file: `L, *LINE` lines, `;` comments, an [External Commands]
// section that is ignored. `skipped` explains every line that was not an alias.
struct PgpParse {
    std::map<std::string, std::string> aliases{};  // upper-case alias -> upper-case command
    std::vector<std::string> skipped{};
};
PgpParse parsePgp(std::string_view text);

}  // namespace tbcad::cmd
