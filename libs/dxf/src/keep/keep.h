// DXF content TBCAD has no model for, carried in Header::custom so that a DXF opened and saved
// again keeps it: header variables the writer does not write itself, and the CLASSES section.
//
// A kept item is a list of groups, stored as lines "code TAB value" (a DXF value never holds a
// line break). Text values are held as UTF-8.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tbcad/dxf/lexer.h"

namespace tbcad::dxf::detail {

using KeptGroups = std::vector<std::pair<int, std::string>>;

inline constexpr std::string_view kKeptVarPrefix = "dxf.hv.";  // + "$NAME"
inline constexpr const char* kKeptClassesKey = "dxf.classes";
// OBJECTS records kept as they are (handles as @N tokens) and the root dictionary entries that
// lead to them ("name TAB @N" lines).
inline constexpr const char* kKeptObjectsKey = "dxf.objects";
inline constexpr const char* kKeptObjectRootKey = "dxf.objroot";
// The handle each entity had in the source file: "idDelta,handleDelta;" pairs in entity id order
// (decimal, the handle delta signed), so a saved DXF gives the entities their old handles back.
inline constexpr const char* kKeptHandlesKey = "dxf.handles";
// The page setup of a layout (its AcDbPlotSettings groups), by layout name.
inline constexpr std::string_view kKeptPlotSetPrefix = "dxf.plotset.";
// Descriptions of groups: lines "name TAB description".
inline constexpr const char* kKeptGroupDescKey = "dxf.groupdesc";

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
        "$ACADVER",   "$HANDSEED",  "$DWGCODEPAGE", "$INSBASE",   "$UCSNAME",  "$UCSORG",
        "$UCSXDIR",   "$UCSYDIR",   "$EXTMIN",      "$EXTMAX",    "$LIMMIN",   "$LIMMAX",
        "$LTSCALE",   "$CELTSCALE", "$PSLTSCALE",   "$MSLTSCALE", "$PLINEGEN", "$CECOLOR",
        "$CELTYPE",   "$CLAYER",    "$TEXTSTYLE",   "$DIMSTYLE",  "$LUNITS",   "$LUPREC",
        "$AUNITS",    "$AUPREC",    "$MEASUREMENT", "$INSUNITS",  "$FILLMODE", "$ATTMODE",
        "$QTEXTMODE", "$MIRRTEXT",  "$PSTYLEMODE"};
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

// The value of a group as text, for content that is carried to the writer unread. `dec` decodes a
// string value; false when the value has no text form (binary chunks).
template <class Dec>
bool keptValue(const Group& g, Dec&& dec, std::string& out) {
    switch (g.type()) {
        case ValueType::String:
            out = dec(g.str());
            return true;
        case ValueType::Double: {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.17g", g.number());
            out = buf;
            return true;
        }
        case ValueType::Int16:
        case ValueType::Int32:
        case ValueType::Int64:
        case ValueType::Bool:
            out = std::to_string(g.integer());
            return true;
        case ValueType::Handle: {
            const Handle& h = std::get<Handle>(g.value);
            out = h.valid ? std::string(h.text) : std::string();
            return h.valid;
        }
        case ValueType::Binary: {
            static const char* digits = "0123456789ABCDEF";
            out.clear();
            for (std::byte b : std::get<std::vector<std::byte>>(g.value)) {
                out.push_back(digits[static_cast<unsigned>(b) >> 4]);
                out.push_back(digits[static_cast<unsigned>(b) & 15]);
            }
            return true;
        }
    }
    return false;
}

using HandlePairs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;  // (entity id, handle)

// `pairs` must be sorted by entity id.
inline std::string packHandles(const HandlePairs& pairs) {
    std::string out;
    out.reserve(pairs.size() * 6);
    std::uint64_t prevId = 0, prevHandle = 0;
    for (const auto& [id, handle] : pairs) {
        out += std::to_string(id - prevId);
        out += ',';
        out += std::to_string(static_cast<std::int64_t>(handle - prevHandle));
        out += ';';
        prevId = id;
        prevHandle = handle;
    }
    return out;
}

inline HandlePairs unpackHandles(std::string_view text) {
    HandlePairs out;
    std::uint64_t id = 0, handle = 0;
    while (!text.empty()) {
        const std::size_t semi = text.find(';');
        const std::string_view item = text.substr(0, semi);
        text = semi == std::string_view::npos ? std::string_view{} : text.substr(semi + 1);
        const std::size_t comma = item.find(',');
        if (comma == std::string_view::npos)
            continue;
        const std::string a(item.substr(0, comma)), b(item.substr(comma + 1));
        id += std::strtoull(a.c_str(), nullptr, 10);
        handle += static_cast<std::uint64_t>(std::strtoll(b.c_str(), nullptr, 10));
        out.emplace_back(id, handle);
    }
    return out;
}

// A text with field codes ("%<date>%") is written with the values shown; its TBCAD XDATA keeps
// the source ("field=" pieces of hex UTF-8) and "fieldof=<hash>;<check>": the hash of the text as
// written, and of the source worked out with every date as kFieldCheckDate. The importer gives the
// fields back only while the text is the one we wrote and the fields still work out the same in
// the drawing it read (an object they name may have another id there).
inline constexpr std::string_view kFieldCheckDate = "\x01";
inline std::string fieldHash(std::string_view written) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : written) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

inline std::vector<std::string> fieldPieces(std::string_view source) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    constexpr std::size_t kPiece = 120;  // bytes per 1000 group (240 hex digits, under 255)
    std::vector<std::string> out;
    for (std::size_t at = 0; at < source.size(); at += kPiece) {
        std::string hex = "field=";
        for (std::size_t k = at; k < std::min(source.size(), at + kPiece); ++k) {
            const auto b = static_cast<unsigned char>(source[k]);
            hex.push_back(kDigits[b >> 4]);
            hex.push_back(kDigits[b & 15]);
        }
        out.push_back(std::move(hex));
    }
    return out;
}

// Appends the bytes of one "field=" piece (after the prefix) to `source`; false when not hex.
inline bool appendFieldPiece(std::string_view hex, std::string& source) {
    if (hex.size() % 2 != 0)
        return false;
    const auto digit = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                      : -1;
    };
    for (std::size_t k = 0; k < hex.size(); k += 2) {
        const int hi = digit(hex[k]), lo = digit(hex[k + 1]);
        if (hi < 0 || lo < 0)
            return false;
        source.push_back(static_cast<char>(hi * 16 + lo));
    }
    return true;
}

}  // namespace tbcad::dxf::detail
