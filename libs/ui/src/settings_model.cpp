#include "tbcad/ui/settings_model.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "tbcad/platform/json.h"

namespace tbcad::ui {

namespace {

Result<void> writeAtomic(const std::filesystem::path& file, const std::string& text) {
    if (file.empty())
        return {};  // headless / no settings folder: edits live for the session only
    std::error_code ec;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
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

std::string trimmed(std::string_view v) {
    return cmd::trim(v);
}

}  // namespace

std::string writePgp(const std::map<std::string, std::string>& aliases) {
    std::string out =
        "; TBCAD command aliases in acad.pgp style: ALIAS, *COMMAND\n"
        "; Lines starting with ; are comments.\n\n";
    for (const auto& [a, c] : aliases) {
        std::string left = a + ",";
        left.resize(std::max<std::size_t>(left.size() + 1, 12), ' ');
        out += left + "*" + c + "\n";
    }
    return out;
}

AliasImportReport AliasEditor::importPgp(std::string_view text) {
    AliasImportReport rep;
    PgpParse parsed = parsePgp(text);
    rep.skipped = std::move(parsed.skipped);
    auto next = registry_.userAliases();
    const auto& effective = registry_.aliases();
    for (const auto& [alias, command] : parsed.aliases) {
        const cmd::Command* c = registry_.find(command);
        if (!c || c->name() != command) {
            rep.skipped.push_back(alias + " -> " + command + ": no such command here");
            continue;
        }
        auto cur = effective.find(alias);
        if (cur != effective.end() && cur->second == command) {
            rep.skipped.push_back(alias + " -> " + command + ": already an alias");
            continue;
        }
        for (auto it = next.begin(); it != next.end();) {  // aliases are case-insensitive
            if (cmd::toUpperAscii(trimmed(it->first)) == alias)
                it = next.erase(it);
            else
                ++it;
        }
        next[alias] = command;
        ++rep.added;
    }
    if (rep.added == 0)
        return rep;
    if (std::string err = commit(std::move(next)); !err.empty()) {
        rep.added = 0;
        rep.error = err;
    }
    return rep;
}

std::string AliasEditor::exportPgp() const {
    return writePgp(registry_.aliases());
}

std::string KeybindingEditor::exportTo(const std::filesystem::path& file) const {
    if (auto r = keys_.save(file); !r)
        return r.error().message;
    return {};
}

KeybindingEditor::Outcome KeybindingEditor::importFrom(const std::filesystem::path& file,
                                                       std::vector<std::string>* warnings) {
    Outcome o;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        o.error = "There is no file " + file.string();
        return o;
    }
    // Check the file on a scratch copy first, so a bad file changes nothing.
    platform::Keybindings probe = keys_;
    auto loaded = probe.load(file);
    if (!loaded) {
        o.error = loaded.error().message;
        return o;
    }
    if (warnings)
        *warnings = loaded.value();
    keys_ = std::move(probe);
    return save();
}

std::string AliasEditor::commit(std::map<std::string, std::string> next) {
    const auto before = registry_.userAliases();
    cmd::AliasReport rep = registry_.setUserAliases(next);
    if (!rep.errors.empty()) {
        registry_.setUserAliases(before);  // keep the table as it was
        return rep.errors.front();
    }
    platform::Json::Object o;
    for (const auto& [a, c] : next)
        o[a] = c;
    if (auto w = writeAtomic(file_, platform::toJson(platform::Json(std::move(o)), true)); !w) {
        registry_.setUserAliases(before);
        return w.error().message;
    }
    return {};
}

std::string AliasEditor::set(const std::string& alias, const std::string& command) {
    std::string a = cmd::toUpperAscii(cmd::trim(alias));
    std::string c = cmd::toUpperAscii(cmd::trim(command));
    if (a.empty())
        return "Type the alias first.";
    if (c.empty())
        return "Choose the command the alias runs.";
    if (!registry_.find(c) || registry_.find(c)->name() != c)
        return "Unknown command " + c + ".";
    auto next = registry_.userAliases();
    for (auto it = next.begin(); it != next.end();) {  // aliases are case-insensitive
        if (cmd::toUpperAscii(cmd::trim(it->first)) == a)
            it = next.erase(it);
        else
            ++it;
    }
    next[a] = c;
    return commit(std::move(next));
}

std::string AliasEditor::remove(const std::string& alias) {
    std::string a = cmd::toUpperAscii(cmd::trim(alias));
    auto next = registry_.userAliases();
    bool found = false;
    for (auto it = next.begin(); it != next.end();) {
        if (cmd::toUpperAscii(cmd::trim(it->first)) == a) {
            it = next.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    if (!found)
        return "That is not a user alias.";
    return commit(std::move(next));
}

std::vector<KeyRow> KeybindingEditor::rows() const {
    std::vector<KeyRow> out;
    for (const std::string& action : keys_.actions()) {
        KeyRow r;
        r.action = action;
        r.chord = keys_.label(action);
        r.defaultChord = keys_.defaultChordOf(action);
        r.overridden = keys_.overridden(action);
        if (!r.chord.empty()) {
            for (const std::string& other : keys_.actions()) {
                if (other != action && keys_.label(other) == r.chord) {
                    r.conflictWith = other;
                    break;
                }
            }
        }
        out.push_back(std::move(r));
    }
    return out;
}

KeybindingEditor::Outcome KeybindingEditor::save() {
    Outcome o;
    if (auto r = keys_.save(file_.empty() ? std::filesystem::path{} : file_);
        !r && !file_.empty()) {
        o.error = r.error().message;
        return o;
    }
    o.ok = true;
    return o;
}

KeybindingEditor::Outcome KeybindingEditor::set(const std::string& action,
                                                const std::string& chordText, bool takeOver) {
    Outcome o;
    bool known = false;
    for (const std::string& a : keys_.actions())
        known = known || a == action;
    if (!known) {
        o.error = "Unknown action " + action + ".";
        return o;
    }
    std::string text;
    if (!chordText.empty()) {
        auto chord = platform::parseChord(chordText);
        if (!chord) {
            o.error = "\"" + chordText + "\" is not a key combination.";
            return o;
        }
        text = chord->toString();
        for (const std::string& other : keys_.actions()) {
            if (other != action && keys_.label(other) == text) {
                if (!takeOver) {
                    o.conflictWith = other;
                    return o;
                }
                keys_.set(other, "");
            }
        }
    }
    keys_.set(action, text);
    return save();
}

KeybindingEditor::Outcome KeybindingEditor::addCommand(const std::string& commandLine,
                                                       const std::string& chordText,
                                                       bool takeOver) {
    Outcome o;
    std::string line = commandLine;
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
        line.erase(line.begin());
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
        line.pop_back();
    if (line.empty()) {
        o.error = "Type the command to run.";
        return o;
    }
    const std::string action = std::string(platform::Keybindings::kCommandPrefix) + line;
    const bool existed = [&] {
        for (const std::string& a : keys_.actions())
            if (a == action)
                return true;
        return false;
    }();
    if (!existed)
        keys_.set(action, "");
    Outcome r = set(action, chordText, takeOver);
    if (!r.ok && !existed)
        keys_.forget(action);
    return r;
}

KeybindingEditor::Outcome KeybindingEditor::removeCommand(const std::string& action) {
    Outcome o;
    if (!platform::Keybindings::isCommandAction(action)) {
        o.error = "Only your own command shortcuts can be removed.";
        return o;
    }
    keys_.forget(action);
    return save();
}

KeybindingEditor::Outcome KeybindingEditor::reset(const std::string& action) {
    keys_.reset(action);
    return save();
}

}  // namespace tbcad::ui
