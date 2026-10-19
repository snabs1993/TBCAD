#include "tbcad/cmd/input.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "tbcad/cmd/host.h"

namespace tbcad::cmd {

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return std::string(s.substr(b, e - b));
}

std::string toUpperAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

std::optional<std::string> matchKeyword(const std::vector<std::string>& keywords,
                                        std::string_view input) {
    std::string in = toUpperAscii(trim(input));
    if (in.empty())
        return std::nullopt;
    for (const std::string& k : keywords) {
        if (toUpperAscii(k) == in)
            return k;
    }
    // AutoCAD's capital letters: "X" chooses "eXit" (capitals that do not start the keyword).
    for (const std::string& k : keywords) {
        if (k.empty() || !std::islower(static_cast<unsigned char>(k[0])))
            continue;
        std::string caps;
        for (char c : k)
            if (std::isupper(static_cast<unsigned char>(c)))
                caps += c;
        if (!caps.empty() && caps == in)
            return k;
    }
    // AutoCAD's capital-letter shortcuts: "BEgin" needs at least "BE", so "B" can mean "Back".
    auto shortest = [](const std::string& k) {
        std::size_t n = 0;
        while (n < k.size() && std::isupper(static_cast<unsigned char>(k[n])))
            ++n;
        const bool mixed = n < k.size() && std::islower(static_cast<unsigned char>(k[n]));
        return mixed && n >= 2 ? n : std::size_t{1};
    };
    const std::string* found = nullptr;
    for (const std::string& k : keywords) {
        if (in.size() >= shortest(k) && toUpperAscii(k).rfind(in, 0) == 0) {
            if (found)
                return std::nullopt;  // ambiguous prefix
            found = &k;
        }
    }
    if (found)
        return *found;
    return std::nullopt;
}

void PointFilterChain::add(PointFilter* f, int order) {
    entries_.push_back({order, f});
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const Entry& a, const Entry& b) { return a.order < b.order; });
}

void PointFilterChain::remove(PointFilter* f) {
    std::erase_if(entries_, [&](const Entry& e) { return e.f == f; });
}

Vec2 PointFilterChain::apply(const PointQuery& query, Vec2 raw, FilterFeedback& feedback) const {
    feedback = FilterFeedback{};
    for (const Entry& e : entries_) {
        FilterFeedback fb;
        if (auto p = e.f->filter(query, raw, fb)) {
            feedback = std::move(fb);
            feedback.active = true;
            return *p;
        }
    }
    return raw;
}

namespace {

std::optional<double> lengthOf(std::string_view s, LengthUnit unit) {
    std::string t = trim(s);
    if (t.empty())
        return std::nullopt;
    auto r = parseLength(t, unit);
    if (!r)
        return std::nullopt;
    return r.value();
}

}  // namespace

std::optional<Vec2> parsePointText(std::string_view text, const PointParseContext& ctx) {
    std::string t = trim(text);
    bool relative = false;
    if (!t.empty() && t[0] == '@') {
        relative = true;
        t.erase(0, 1);
    } else if (!t.empty() && t[0] == '#') {
        t.erase(0, 1);
    }
    if (t.empty())
        return std::nullopt;
    const Vec2 origin = relative ? ctx.lastPoint : Vec2{};

    if (auto comma = t.find(','); comma != std::string::npos) {
        if (t.find(',', comma + 1) != std::string::npos)
            return std::nullopt;  // 3D input is not supported
        auto x = lengthOf(std::string_view(t).substr(0, comma), ctx.unit);
        auto y = lengthOf(std::string_view(t).substr(comma + 1), ctx.unit);
        if (!x || !y)
            return std::nullopt;
        return origin + Vec2{*x, *y};
    }
    if (auto lt = t.find('<'); lt != std::string::npos) {
        auto d = lengthOf(std::string_view(t).substr(0, lt), ctx.unit);
        std::string angText = trim(std::string_view(t).substr(lt + 1));
        if (!d || angText.empty())
            return std::nullopt;
        auto a = parseAngle(angText, ctx.angleUnit);
        if (!a)
            return std::nullopt;
        return origin + Vec2{std::cos(a.value()), std::sin(a.value())} * *d;
    }
    return std::nullopt;
}

std::string PromptInfo::display() const {
    std::string s = text;
    // Many prompts already spell out "[A/B] <default>" in their text; do not repeat either part.
    if (!keywords.empty() && text.find('[') == std::string::npos) {
        s += " or [";
        for (std::size_t i = 0; i < keywords.size(); ++i) {
            if (i)
                s += '/';
            s += keywords[i];
        }
        s += ']';
    }
    if (defaultText && text.find("<" + *defaultText + ">") == std::string::npos)
        s += " <" + *defaultText + ">";
    s += ": ";
    return s;
}

}  // namespace tbcad::cmd
