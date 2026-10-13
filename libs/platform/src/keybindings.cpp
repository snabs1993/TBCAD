#include "tbcad/platform/keybindings.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include "tbcad/platform/json.h"

namespace tbcad::platform {

namespace {

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

std::string trimmed(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return std::string(s);
}

bool validKey(const std::string& k) {
    if (k.size() == 1)
        return std::isalnum(static_cast<unsigned char>(k[0])) != 0;
    if (k.size() >= 2 && k[0] == 'F') {
        int n = 0;
        for (std::size_t i = 1; i < k.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(k[i])))
                return false;
            n = n * 10 + (k[i] - '0');
        }
        return n >= 1 && n <= 24;
    }
    static const char* names[] = {"ESCAPE", "ENTER",  "DELETE",   "TAB",       "SPACE",
                                  "HOME",   "END",    "LEFT",     "RIGHT",     "UP",
                                  "DOWN",   "PAGEUP", "PAGEDOWN", "BACKSPACE", "INSERT"};
    return std::find(std::begin(names), std::end(names), k) != std::end(names);
}

}  // namespace

std::string KeyChord::toString() const {
    std::string s;
    if (ctrl)
        s += "Ctrl+";
    if (shift)
        s += "Shift+";
    if (alt)
        s += "Alt+";
    if (key.size() <= 1) {
        s += key;
    } else {
        // "ESCAPE" -> "Escape", "F5" stays "F5"
        if (key[0] == 'F' && std::isdigit(static_cast<unsigned char>(key[1]))) {
            s += key;
        } else {
            s += key[0];
            for (std::size_t i = 1; i < key.size(); ++i)
                s += static_cast<char>(std::tolower(static_cast<unsigned char>(key[i])));
        }
    }
    return s;
}

std::optional<KeyChord> parseChord(std::string_view text) {
    KeyChord c;
    std::string t = trimmed(text);
    if (t.empty())
        return std::nullopt;
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= t.size(); ++i) {
        if (i == t.size() || t[i] == '+') {
            parts.push_back(trimmed(std::string_view(t).substr(start, i - start)));
            start = i + 1;
        }
    }
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        std::string m = upper(parts[i]);
        if (m == "CTRL" || m == "CONTROL")
            c.ctrl = true;
        else if (m == "SHIFT")
            c.shift = true;
        else if (m == "ALT")
            c.alt = true;
        else
            return std::nullopt;
    }
    c.key = upper(parts.back());
    if (c.key == "ESC")
        c.key = "ESCAPE";
    if (c.key == "RETURN")
        c.key = "ENTER";
    if (c.key == "DEL")
        c.key = "DELETE";
    if (!validKey(c.key))
        return std::nullopt;
    return c;
}

void Keybindings::declare(std::string actionId, std::string defaultChord) {
    defaults_[std::move(actionId)] = std::move(defaultChord);
}

Result<std::vector<std::string>> Keybindings::load(const std::filesystem::path& file) {
    std::vector<std::string> warnings;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
        return warnings;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return makeError(ErrorCode::Io, "Cannot read " + file.string());
    std::ostringstream buf;
    buf << in.rdbuf();
    auto parsed = parseJson(buf.str());
    if (!parsed)
        return makeError(ErrorCode::Parse,
                         file.filename().string() + ": " + parsed.error().message);
    if (!parsed.value().isObject())
        return makeError(ErrorCode::Parse,
                         file.filename().string() + " must contain a JSON object");
    overrides_.clear();
    for (const auto& [action, value] : parsed.value().asObject()) {
        if (!value.isString()) {
            warnings.push_back("keybindings.json: '" + action + "' must be a text value");
            continue;
        }
        if (!value.asString().empty() && !parseChord(value.asString())) {
            warnings.push_back("keybindings.json: '" + value.asString() + "' is not a valid key");
            continue;
        }
        if (!defaults_.count(action))
            warnings.push_back("keybindings.json: unknown action '" + action + "'");
        overrides_[action] = value.asString();
    }
    // Two actions on one chord: the first (alphabetically) wins; tell the user about the rest.
    std::map<std::string, std::string> seen;
    for (const auto& action : actions()) {
        auto chord = chordOf(action);
        if (!chord)
            continue;
        auto [it, inserted] = seen.emplace(chord->toString(), action);
        if (!inserted)
            warnings.push_back("keybindings.json: " + chord->toString() + " is used by both '" +
                               it->second + "' and '" + action + "'");
    }
    return warnings;
}

Result<void> Keybindings::save(const std::filesystem::path& file) const {
    Json::Object o;
    for (const auto& action : actions()) {
        auto chord = chordOf(action);
        o[action] = chord ? chord->toString() : std::string{};
    }
    std::error_code ec;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return makeError(ErrorCode::Io, "Cannot write " + tmp.string());
        out << toJson(Json(std::move(o)), true);
        if (!out)
            return makeError(ErrorCode::Io, "Cannot write " + tmp.string());
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return makeError(ErrorCode::Io, "Cannot replace " + file.string());
    }
    return {};
}

std::optional<KeyChord> Keybindings::chordOf(std::string_view actionId) const {
    std::string id(actionId);
    auto o = overrides_.find(id);
    if (o != overrides_.end())
        return parseChord(o->second);
    auto d = defaults_.find(id);
    if (d != defaults_.end())
        return parseChord(d->second);
    return std::nullopt;
}

std::string Keybindings::actionFor(const KeyChord& chord) const {
    for (const auto& action : actions()) {
        auto c = chordOf(action);
        if (c && *c == chord)
            return action;
    }
    return {};
}

std::string Keybindings::label(std::string_view actionId) const {
    auto c = chordOf(actionId);
    return c ? c->toString() : std::string{};
}

void Keybindings::set(std::string_view actionId, std::string chord) {
    overrides_[std::string(actionId)] = std::move(chord);
}

std::string Keybindings::defaultChordOf(std::string_view actionId) const {
    auto d = defaults_.find(std::string(actionId));
    if (d == defaults_.end())
        return {};
    auto c = parseChord(d->second);
    return c ? c->toString() : std::string{};
}

bool Keybindings::overridden(std::string_view actionId) const {
    return overrides_.count(std::string(actionId)) > 0;
}

void Keybindings::reset(std::string_view actionId) {
    overrides_.erase(std::string(actionId));
}

std::vector<std::string> Keybindings::actions() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : defaults_)
        out.push_back(k);
    for (const auto& [k, v] : overrides_)
        if (!defaults_.count(k))
            out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace tbcad::platform
