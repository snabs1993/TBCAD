#include "tbcad/hatch/pattern.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace tbcad::hatch {

double PatternFamily::cycleLength() const {
    double sum = 0;
    for (double d : dashes)
        sum += std::abs(d);
    return sum;
}

namespace {

std::string_view trimView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        std::size_t pos = s.find(sep, start);
        out.push_back(trimView(s.substr(start, pos == std::string_view::npos ? pos : pos - start)));
        if (pos == std::string_view::npos)
            break;
        start = pos + 1;
    }
    return out;
}

bool parseNumber(std::string_view s, double& out) {
    s = trimView(s);
    if (s.empty())
        return false;
    // from_chars for double needs a recent standard library on every platform we build on, but
    // not a leading '+'.
    if (s.front() == '+')
        s.remove_prefix(1);
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size() && std::isfinite(out);
}

bool parsePair(std::string_view s, Vec2& out) {
    auto parts = split(s, ',');
    return parts.size() == 2 && parseNumber(parts[0], out.x) && parseNumber(parts[1], out.y);
}

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

Error lineError(int line, const std::string& what) {
    return makeError(ErrorCode::Parse, "patterns line " + std::to_string(line) + ": " + what);
}

}  // namespace

Result<std::vector<Pattern>> parsePatterns(std::string_view text) {
    std::vector<Pattern> out;
    int lineNo = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t eol = text.find('\n', pos);
        std::string_view raw =
            text.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? text.size() + 1 : eol + 1;
        ++lineNo;
        std::string_view line = trimView(raw);
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[') {
            if (line.back() != ']' || line.size() < 3)
                return lineError(lineNo, "malformed section header");
            std::string name = upper(trimView(line.substr(1, line.size() - 2)));
            for (const Pattern& p : out)
                if (p.name == name)
                    return lineError(lineNo, "duplicate pattern " + name);
            out.push_back(Pattern{});
            out.back().name = std::move(name);
            continue;
        }
        if (out.empty())
            return lineError(lineNo, "entry before the first [NAME] section");
        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            return lineError(lineNo, "expected key = value");
        std::string key = upper(trimView(line.substr(0, eq)));
        std::string_view value = trimView(line.substr(eq + 1));
        Pattern& pat = out.back();
        if (key == "DESCRIPTION") {
            pat.description = std::string(value);
        } else if (key == "SOLID") {
            std::string v = upper(value);
            pat.solid = v == "YES" || v == "TRUE" || v == "1";
        } else if (key == "LINE") {
            auto parts = split(value, '|');
            if (parts.size() != 4)
                return lineError(lineNo, "line needs: angle | origin | delta | dashes");
            PatternFamily f;
            double deg = 0;
            if (!parseNumber(parts[0], deg))
                return lineError(lineNo, "bad angle");
            f.angle = deg * kPi / 180.0;
            if (!parsePair(parts[1], f.origin))
                return lineError(lineNo, "bad origin");
            if (!parsePair(parts[2], f.delta))
                return lineError(lineNo, "bad delta");
            if (!(std::abs(f.delta.y) > 0))
                return lineError(lineNo, "delta y (line spacing) must not be zero");
            if (!parts[3].empty()) {
                for (std::string_view d : split(parts[3], ',')) {
                    double v = 0;
                    if (!parseNumber(d, v))
                        return lineError(lineNo, "bad dash length");
                    f.dashes.push_back(v);
                }
                if (f.cycleLength() <= 0)
                    return lineError(lineNo, "dash list has zero total length");
            }
            pat.families.push_back(std::move(f));
        } else {
            return lineError(lineNo, "unknown key " + key);
        }
    }
    for (const Pattern& p : out)
        if (!p.solid && p.families.empty())
            return makeError(ErrorCode::Parse, "pattern " + p.name + " has no lines and is not solid");
    return out;
}

const std::vector<Pattern>& builtinPatterns() {
    static const std::vector<Pattern> patterns = [] {
        auto r = parsePatterns(builtinPatternText());
        if (!r.ok())
            throw std::logic_error("built-in patterns are invalid: " + r.error().message);
        return std::move(r).value();
    }();
    return patterns;
}

namespace {
std::vector<Pattern>& userPatterns() {
    static std::vector<Pattern> list;
    return list;
}
}  // namespace

void PatternRegistry::add(Pattern pattern) {
    pattern.name = upper(pattern.name);
    auto& list = userPatterns();
    for (Pattern& p : list)
        if (p.name == pattern.name) {
            p = std::move(pattern);
            return;
        }
    list.push_back(std::move(pattern));
}

void PatternRegistry::clearUser() {
    userPatterns().clear();
}

const Pattern* PatternRegistry::find(std::string_view name) {
    std::string up = upper(name);
    for (const Pattern& p : userPatterns())
        if (p.name == up)
            return &p;
    return findBuiltinPattern(name);
}

std::vector<std::string> PatternRegistry::names() {
    std::vector<std::string> out;
    for (const Pattern& p : builtinPatterns())
        out.push_back(p.name);
    for (const Pattern& p : userPatterns())
        if (std::find(out.begin(), out.end(), p.name) == out.end())
            out.push_back(p.name);
    return out;
}

const Pattern* findBuiltinPattern(std::string_view name) {
    std::string up = upper(name);
    for (const Pattern& p : builtinPatterns())
        if (p.name == up)
            return &p;
    return nullptr;
}

}  // namespace tbcad::hatch
