// DXF content TBCAD has no model for, carried in Header::custom so that a DXF opened and saved
// again keeps it: header variables the writer does not write itself, and the CLASSES section.
//
// A kept item is a list of groups, stored as lines "code TAB value" (a DXF value never holds a
// line break). Text values are held as UTF-8.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tbcad::dxf::detail {

using KeptGroups = std::vector<std::pair<int, std::string>>;

inline constexpr std::string_view kKeptVarPrefix = "dxf.hv.";  // + "$NAME"
inline constexpr const char* kKeptClassesKey = "dxf.classes";

inline void packGroup(std::string& out, int code, std::string_view value) {
    out += std::to_string(code);
    out += '\t';
    out += value;
    out += '\n';
}

inline KeptGroups unpackGroups(std::string_view text) {
    KeptGroups g;
    while (!text.empty()) {
        std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos || tab == 0)
            continue;
        int code = 0;
        bool ok = true;
        for (char c : line.substr(0, tab)) {
            if (c < '0' || c > '9' || code > 100000) {
                ok = false;
                break;
            }
            code = code * 10 + (c - '0');
        }
        if (ok)
            g.emplace_back(code, std::string(line.substr(tab + 1)));
    }
    return g;
}

// Header variables the writer always writes itself. Of these the overridable ones take the value
// of the source file when it had one; the others are derived from the drawing.
inline bool ownedHeaderVar(std::string_view n) {
    static constexpr std::string_view owned[] = {
        "$ACADVER", "$HANDSEED",  "$DWGCODEPAGE", "$INSBASE",   "$UCSNAME",  "$UCSORG",
        "$UCSXDIR", "$UCSYDIR",   "$EXTMIN",      "$EXTMAX",    "$LIMMIN",   "$LIMMAX",
        "$LTSCALE", "$CELTSCALE", "$PSLTSCALE",   "$MSLTSCALE", "$PLINEGEN", "$CECOLOR",
        "$CELTYPE", "$CLAYER",    "$TEXTSTYLE",   "$DIMSTYLE",  "$LUNITS",   "$LUPREC",
        "$AUNITS",  "$AUPREC",    "$MEASUREMENT", "$INSUNITS"};
    for (std::string_view o : owned)
        if (o == n)
            return true;
    return false;
}

inline bool overridableHeaderVar(std::string_view n) {
    static constexpr std::string_view v[] = {"$INSBASE", "$LIMMIN",  "$LIMMAX",
                                             "$CECOLOR", "$CELTYPE", "$LUNITS",
                                             "$LUPREC",  "$AUNITS",  "$AUPREC"};
    for (std::string_view o : v)
        if (o == n)
            return true;
    return false;
}

// Group codes that hold object handles: a variable using them would point at objects the saved
// file numbers differently.
inline bool handleCode(int c) {
    return c == 105 || (c >= 330 && c <= 369) || (c >= 390 && c <= 399) || c == 1005;
}

}  // namespace tbcad::dxf::detail
