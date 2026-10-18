#include "tbcad/layers/layer_states.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <sstream>

#include "tbcad/layers/layer_ops.h"

namespace tbcad::layers {

namespace {

constexpr std::string_view kStatePrefix = "tbcad.layerstate.";
constexpr std::string_view kFilterPrefix = "tbcad.layerfilter.";
constexpr std::string_view kGroupPrefix = "tbcad.layergroup.";

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

Error err(ErrorCode code, std::string msg) {
    return makeError(code, std::move(msg));
}

Result<std::string> cleanName(std::string_view name) {
    std::size_t b = 0, e = name.size();
    while (b < e && std::isspace(static_cast<unsigned char>(name[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(name[e - 1])))
        --e;
    name = name.substr(b, e - b);
    if (name.empty())
        return err(ErrorCode::InvalidArgument, "The name is empty.");
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20)
            return err(ErrorCode::InvalidArgument, "The name contains a control character.");
    return lower(name);
}

// Field escaping for the stored text: \\ \t \n.
std::string escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\t')
            out += "\\t";
        else if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}

std::string unescape(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == 't' ? '\t' : s[i] == 'n' ? '\n' : s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (true) {
        std::size_t t = line.find('\t', pos);
        out.push_back(line.substr(pos, t == std::string::npos ? std::string::npos : t - pos));
        if (t == std::string::npos)
            break;
        pos = t + 1;
    }
    return out;
}

std::vector<std::string> namesWithPrefix(const Document& doc, std::string_view prefix) {
    std::vector<std::string> out;
    for (const auto& [key, value] : doc.header().custom)
        if (key.compare(0, prefix.size(), prefix) == 0)
            out.push_back(key.substr(prefix.size()));
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

// ---- layer states ----------------------------------------------------------------------

std::vector<std::string> layerStateNames(const Document& doc) {
    return namesWithPrefix(doc, kStatePrefix);
}

Result<void> saveLayerState(const Document& doc, Transaction& tx, std::string_view name) {
    auto key = cleanName(name);
    if (!key)
        return key.error();
    std::string text;
    if (const Layer* cur = doc.layer(doc.header().currentLayer))
        text += "#current\t" + escape(layerPath(doc, cur->id)) + '\n';
    for (LayerId id : layerTreeOrder(doc)) {
        const Layer* l = doc.layer(id);
        text += escape(layerPath(doc, id)) + '\t';
        text += l->on ? '1' : '0';
        text += l->frozen ? '1' : '0';
        text += l->locked ? '1' : '0';
        text += l->plot ? '1' : '0';
        text += '\t' + escape(colorName(l->color));
        text += '\t' + escape(linetypeName(doc, l->linetype));
        text += '\t' + escape(lineweightName(l->lineweight));
        text += '\n';
    }
    return tx.modifyHeader(
        [&](Header& h) { h.custom[std::string(kStatePrefix) + key.value()] = text; });
}

Result<void> restoreLayerState(const Document& doc, Transaction& tx, std::string_view name,
                               StateParts parts, std::size_t* skipped) {
    auto key = cleanName(name);
    if (!key)
        return key.error();
    auto it = doc.header().custom.find(std::string(kStatePrefix) + key.value());
    if (it == doc.header().custom.end())
        return err(ErrorCode::NotFound, "No layer state \"" + std::string(name) + "\".");
    std::size_t bad = 0;
    std::istringstream in(it->second);
    std::string line;
    std::vector<LayerId> saved;
    std::optional<LayerId> savedCurrent;
    while (std::getline(in, line)) {
        auto f = splitTabs(line);
        if (f.size() == 2 && f[0] == "#current") {
            if (auto id = resolveLayer(doc, unescape(f[1])); id)
                savedCurrent = id.value();
            continue;
        }
        if (f.size() != 5 || f[1].size() != 4) {
            ++bad;
            continue;
        }
        auto id = resolveLayer(doc, unescape(f[0]));
        if (!id) {
            ++bad;
            continue;
        }
        saved.push_back(id.value());
        auto cur = [&]() { return doc.layer(id.value()); };
        bool ok = true;
        auto note = [&](const Result<void>& r) {
            if (!r)
                ok = false;
        };
        if (parts.visibility) {
            if (cur()->on != (f[1][0] == '1'))
                note(setOn(doc, tx, id.value(), f[1][0] == '1'));
            if (cur()->frozen != (f[1][1] == '1'))
                note(setFrozen(doc, tx, id.value(), f[1][1] == '1'));
        }
        if (parts.lock && cur()->locked != (f[1][2] == '1'))
            note(setLocked(doc, tx, id.value(), f[1][2] == '1'));
        if (parts.plot && cur()->plot != (f[1][3] == '1'))
            note(setPlot(doc, tx, id.value(), f[1][3] == '1'));
        if (parts.look) {
            if (auto c = parseColor(unescape(f[2])); c)
                note(setLayerColor(doc, tx, id.value(), c.value()));
            else
                ok = false;
            if (auto lt = parseLinetype(doc, unescape(f[3])); lt)
                note(setLayerLinetype(doc, tx, id.value(), lt.value()));
            else
                ok = false;
            if (auto lw = parseLineweight(unescape(f[4])); lw)
                note(setLayerLineweight(doc, tx, id.value(), lw.value()));
            else
                ok = false;
        }
        if (!ok)
            ++bad;
    }
    if (parts.hideOthers) {
        for (LayerId id : layerTreeOrder(doc)) {
            if (std::find(saved.begin(), saved.end(), id) == saved.end() && doc.layer(id)->on)
                if (!setOn(doc, tx, id, false))
                    ++bad;
        }
    }
    if (parts.current && savedCurrent && *savedCurrent != doc.header().currentLayer)
        if (!setCurrentLayer(doc, tx, *savedCurrent))
            ++bad;
    if (skipped)
        *skipped = bad;
    return {};
}

Result<void> deleteLayerState(const Document& doc, Transaction& tx, std::string_view name) {
    auto key = cleanName(name);
    if (!key)
        return key.error();
    const std::string full = std::string(kStatePrefix) + key.value();
    if (!doc.header().custom.count(full))
        return err(ErrorCode::NotFound, "No layer state \"" + std::string(name) + "\".");
    return tx.modifyHeader([&](Header& h) { h.custom.erase(full); });
}

Result<void> renameLayerState(const Document& doc, Transaction& tx, std::string_view from,
                              std::string_view to) {
    auto a = cleanName(from);
    auto b = cleanName(to);
    if (!a)
        return a.error();
    if (!b)
        return b.error();
    const std::string fa = std::string(kStatePrefix) + a.value();
    const std::string fb = std::string(kStatePrefix) + b.value();
    auto it = doc.header().custom.find(fa);
    if (it == doc.header().custom.end())
        return err(ErrorCode::NotFound, "No layer state \"" + std::string(from) + "\".");
    if (fa != fb && doc.header().custom.count(fb))
        return err(ErrorCode::Conflict, "A layer state \"" + std::string(to) + "\" exists.");
    const std::string text = it->second;
    return tx.modifyHeader([&](Header& h) {
        h.custom.erase(fa);
        h.custom[fb] = text;
    });
}

// ---- layer filters ---------------------------------------------------------------------

bool wildcardMatch(std::string_view pattern, std::string_view text) {
    const std::string p = lower(pattern), t = lower(text);
    std::size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*')
        ++pi;
    return pi == p.size();
}

namespace {

std::vector<std::string> words(std::string_view expr) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : expr) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty())
                out.push_back(std::move(cur)), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

int filterDepth = 0;  // nesting of filter:NAME, to stop circular filters

// -1 = cannot understand the word, 0 = no match, 1 = match.
int matchWord(const Document& doc, LayerId id, const std::string& rawWord, std::string& error) {
    std::string word = rawWord;
    bool negate = false;
    if (lower(word).compare(0, 4, "not:") == 0) {
        negate = true;
        word = word.substr(4);
    }
    const Layer* l = doc.layer(id);
    const std::string lw = lower(word);
    int result = -1;
    auto set = [&](bool v) { result = v ? 1 : 0; };
    if (lw == "on")
        set(l->on);
    else if (lw == "off")
        set(!l->on);
    else if (lw == "frozen")
        set(l->frozen);
    else if (lw == "thawed")
        set(!l->frozen);
    else if (lw == "locked")
        set(l->locked);
    else if (lw == "unlocked")
        set(!l->locked);
    else if (lw == "plot")
        set(l->plot);
    else if (lw == "noplot")
        set(!l->plot);
    else if (lw == "used")
        set(entityCount(doc, id) > 0);
    else if (lw == "empty")
        set(entityCount(doc, id) == 0);
    else if (lw == "current")
        set(doc.header().currentLayer == id);
    else if (std::size_t colon = word.find(':'); colon != std::string::npos) {
        const std::string key = lower(word.substr(0, colon));
        const std::string value = word.substr(colon + 1);
        EffectiveLayer e = doc.effective(id);
        if (key == "group") {
            auto members = layerGroupMembers(doc, value);
            if (!members) {
                error = members.error().message;
                return -1;
            }
            set(std::find(members.value().begin(), members.value().end(), id) !=
                members.value().end());
        } else if (key == "filter") {
            auto expr = savedFilterExpression(doc, value);
            if (!expr) {
                error = expr.error().message;
                return -1;
            }
            if (filterDepth > 8) {
                error = "Layer filters refer to each other in a circle.";
                return -1;
            }
            ++filterDepth;
            std::string inner;
            const bool m = layerMatchesFilter(doc, id, expr.value(), &inner);
            --filterDepth;
            if (!inner.empty()) {
                error = inner;
                return -1;
            }
            set(m);
        } else if (key == "name")
            set(wildcardMatch(value, l->name));
        else if (key == "path")
            set(wildcardMatch(value, layerPath(doc, id)));
        else if (key == "branch") {
            auto root = resolveLayer(doc, value);
            if (!root) {
                error = root.error().message;
                return -1;
            }
            auto b = branchOf(doc, root.value());
            set(std::find(b.begin(), b.end(), id) != b.end());
        } else if (key == "color" || key == "colour") {
            auto c = parseColor(value);
            if (!c) {
                error = "Cannot read the colour in \"" + rawWord + "\".";
                return -1;
            }
            set(colorName(e.color) == colorName(c.value()));
        } else if (key == "linetype") {
            set(lower(linetypeName(doc, e.linetype)) == lower(value));
        } else if (key == "lineweight") {
            auto w = parseLineweight(value);
            if (!w) {
                error = "Cannot read the lineweight in \"" + rawWord + "\".";
                return -1;
            }
            set(lineweightName(e.lineweight) == lineweightName(w.value()));
        } else {
            error = "Unknown filter word \"" + rawWord + "\".";
            return -1;
        }
    } else if (word.find_first_of("*?") != std::string::npos || !word.empty()) {
        set(wildcardMatch(word, l->name));
    }
    if (result < 0) {
        error = "Unknown filter word \"" + rawWord + "\".";
        return -1;
    }
    return negate ? 1 - result : result;
}

}  // namespace

bool layerMatchesFilter(const Document& doc, LayerId id, std::string_view expression,
                        std::string* error) {
    std::string unused;
    std::string& why = error ? *error : unused;
    why.clear();
    if (!doc.layer(id))
        return false;
    bool all = true;
    for (const std::string& w : words(expression)) {
        if (w == "*")
            continue;
        int m = matchWord(doc, id, w, why);
        if (m < 0)
            return false;
        all = all && m == 1;
    }
    return all;
}

Result<std::vector<LayerId>> filterLayers(const Document& doc, std::string_view expression) {
    std::vector<LayerId> out;
    std::string why;
    for (LayerId id : layerTreeOrder(doc)) {
        const bool m = layerMatchesFilter(doc, id, expression, &why);
        if (!why.empty())
            return err(ErrorCode::Parse, why);
        if (m)
            out.push_back(id);
    }
    return out;
}

std::vector<std::string> layerFilterNames(const Document& doc) {
    return namesWithPrefix(doc, kFilterPrefix);
}

Result<std::string> savedFilterExpression(const Document& doc, std::string_view name) {
    auto key = cleanName(name);
    if (!key)
        return key.error();
    auto it = doc.header().custom.find(std::string(kFilterPrefix) + key.value());
    if (it == doc.header().custom.end())
        return err(ErrorCode::NotFound, "No layer filter \"" + std::string(name) + "\".");
    return it->second;
}

Result<void> saveLayerFilter(const Document& doc, Transaction& tx, std::string_view name,
                             std::string_view expression) {
    auto key = cleanName(name);
    if (!key)
        return key.error();
    auto check = filterLayers(doc, expression);  // reports words it cannot understand
    if (!check)
        return check.error();
    const std::string text(expression);
    return tx.modifyHeader(
        [&](Header& h) { h.custom[std::string(kFilterPrefix) + key.value()] = text; });
}

Result<void> deleteLayerFilter(const Document& doc, Transaction& tx, std::string_view name) {
    auto key = cleanName(name);
    if (!key)
        return key.error();
    const std::string full = std::string(kFilterPrefix) + key.value();
    if (!doc.header().custom.count(full))
        return err(ErrorCode::NotFound, "No layer filter \"" + std::string(name) + "\".");
    return tx.modifyHeader([&](Header& h) { h.custom.erase(full); });
}

// ---- layer groups -----------------------------------------------------------------------

std::vector<std::string> layerGroupNames(const Document& doc) {
    return namesWithPrefix(doc, kGroupPrefix);
}

namespace {

std::vector<std::string> groupPaths(const Document& doc, const std::string& key) {
    std::vector<std::string> out;
    auto it = doc.header().custom.find(std::string(kGroupPrefix) + key);
    if (it == doc.header().custom.end())
        return out;
    std::istringstream in(it->second);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty())
            out.push_back(unescape(line));
    return out;
}

std::string joinPaths(const std::vector<std::string>& paths) {
    std::string text;
    for (const std::string& p : paths)
        text += escape(p) + '\n';
    return text;
}

}  // namespace

Result<std::vector<LayerId>> layerGroupMembers(const Document& doc, std::string_view name) {
    auto key = cleanName(name);
    if (!key)
        return key.error();
    if (!doc.header().custom.count(std::string(kGroupPrefix) + key.value()))
        return err(ErrorCode::NotFound, "No layer group \"" + std::string(name) + "\".");
    std::vector<LayerId> found;
    for (const std::string& p : groupPaths(doc, key.value()))
        if (auto id = resolveLayer(doc, p); id)
            found.push_back(id.value());
    std::vector<LayerId> out;
    for (LayerId id : layerTreeOrder(doc))
        if (std::find(found.begin(), found.end(), id) != found.end())
            out.push_back(id);
    return out;
}

Result<void> saveLayerGroup(const Document& doc, Transaction& tx, std::string_view name,
                            const std::vector<LayerId>& members) {
    auto key = cleanName(name);
    if (!key)
        return key.error();
    std::vector<std::string> paths;
    for (LayerId id : members)
        if (doc.layer(id))
            paths.push_back(layerPath(doc, id));
    const std::string text = joinPaths(paths);
    return tx.modifyHeader(
        [&](Header& h) { h.custom[std::string(kGroupPrefix) + key.value()] = text; });
}

Result<void> addToLayerGroup(const Document& doc, Transaction& tx, std::string_view name,
                             const std::vector<LayerId>& more) {
    auto cur = layerGroupMembers(doc, name);
    if (!cur)
        return cur.error();
    std::vector<LayerId> all = cur.value();
    for (LayerId id : more)
        if (std::find(all.begin(), all.end(), id) == all.end())
            all.push_back(id);
    return saveLayerGroup(doc, tx, name, all);
}

Result<void> removeFromLayerGroup(const Document& doc, Transaction& tx, std::string_view name,
                                  const std::vector<LayerId>& gone) {
    auto cur = layerGroupMembers(doc, name);
    if (!cur)
        return cur.error();
    std::vector<LayerId> keep;
    for (LayerId id : cur.value())
        if (std::find(gone.begin(), gone.end(), id) == gone.end())
            keep.push_back(id);
    return saveLayerGroup(doc, tx, name, keep);
}

Result<void> deleteLayerGroup(const Document& doc, Transaction& tx, std::string_view name) {
    auto key = cleanName(name);
    if (!key)
        return key.error();
    const std::string full = std::string(kGroupPrefix) + key.value();
    if (!doc.header().custom.count(full))
        return err(ErrorCode::NotFound, "No layer group \"" + std::string(name) + "\".");
    return tx.modifyHeader([&](Header& h) { h.custom.erase(full); });
}

}  // namespace tbcad::layers
