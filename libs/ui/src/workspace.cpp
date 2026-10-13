#include "tbcad/ui/workspace.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "tbcad/platform/json.h"
#include "tbcad/platform/paths.h"

namespace tbcad::ui {

namespace fs = std::filesystem;

namespace {

std::string lowerOf(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

}  // namespace

std::vector<Workspace> WorkspaceStore::builtins() {
    auto make = [](const char* name, bool commandLine, bool properties,
                   std::map<std::string, bool> panels) {
        Workspace w;
        w.name = name;
        w.builtin = true;
        w.commandLine = commandLine;
        w.properties = properties;
        w.panels = std::move(panels);
        return w;
    };
    return {
        make("Drafting", true, true, {{"layers", true}, {"properties", true}}),
        make("Annotation", true, true,
             {{"layers", true},
              {"properties", true},
              {"text_editor", true},
              {"textstyles", true},
              {"dimstyles", true}}),
        make("Minimal", true, false, {}),
    };
}

std::string WorkspaceStore::fileStem(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        const bool bad = c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                         c == '"' || c == '<' || c == '>' || c == '|';
        out.push_back(bad ? '_' : static_cast<char>(c));
    }
    return out;
}

fs::path WorkspaceStore::fileOf(const std::string& name) const {
    return folder_ / platform::pathFromUtf8(fileStem(name) + ".json");
}

bool WorkspaceStore::isBuiltin(const std::string& name) const {
    for (const Workspace& w : builtins())
        if (lowerOf(w.name) == lowerOf(name))
            return true;
    return false;
}

std::vector<std::string> WorkspaceStore::names() const {
    std::vector<std::string> out;
    for (const Workspace& w : builtins())
        out.push_back(w.name);
    std::vector<std::string> user;
    std::error_code ec;
    if (!folder_.empty() && fs::is_directory(folder_, ec)) {
        for (fs::directory_iterator it(folder_, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || lowerOf(it->path().extension().string()) != ".json")
                continue;
            // The display name is stored inside; the file stem is the fallback.
            std::string name = platform::pathToUtf8(it->path().stem());
            std::ifstream in(it->path(), std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            if (auto j = platform::parseJson(ss.str()); j && j.value().isObject())
                if (const auto* n = j.value().find("name");
                    n && n->isString() && !n->asString().empty())
                    name = n->asString();
            if (!isBuiltin(name))
                user.push_back(std::move(name));
        }
    }
    std::sort(user.begin(), user.end(),
              [](const std::string& a, const std::string& b) { return lowerOf(a) < lowerOf(b); });
    out.insert(out.end(), user.begin(), user.end());
    return out;
}

std::optional<Workspace> WorkspaceStore::load(const std::string& name) const {
    for (Workspace& w : builtins())
        if (lowerOf(w.name) == lowerOf(name))
            return w;
    std::ifstream in(fileOf(name), std::ios::binary);
    if (!in)
        return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    auto j = platform::parseJson(ss.str());
    if (!j || !j.value().isObject())
        return std::nullopt;
    Workspace w;
    w.name = name;
    if (const auto* n = j.value().find("name"); n && n->isString() && !n->asString().empty())
        w.name = n->asString();
    if (const auto* v = j.value().find("commandLine"))
        w.commandLine = v->asBool(true);
    if (const auto* v = j.value().find("properties"))
        w.properties = v->asBool(true);
    if (const auto* v = j.value().find("dockIni"))
        w.dockIni = v->asString();
    if (const auto* v = j.value().find("panels"))
        for (const auto& [id, on] : v->asObject())
            w.panels[id] = on.asBool(false);
    return w;
}

std::string WorkspaceStore::save(const Workspace& ws) {
    const std::string name = trimmed(ws.name);
    if (name.empty())
        return "Give the workspace a name.";
    if (isBuiltin(name))
        return "\"" + name + "\" is built in; choose another name.";
    platform::Json::Object o;
    o["name"] = name;
    o["commandLine"] = ws.commandLine;
    o["properties"] = ws.properties;
    o["dockIni"] = ws.dockIni;
    platform::Json::Object panels;
    for (const auto& [id, on] : ws.panels)
        panels[id] = on;
    o["panels"] = std::move(panels);
    std::error_code ec;
    fs::create_directories(folder_, ec);
    const fs::path file = fileOf(name);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << platform::toJson(platform::Json(std::move(o)), true);
        if (!out)
            return "Cannot write " + tmp.string();
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return "Cannot replace " + file.string();
    }
    return {};
}

std::string WorkspaceStore::remove(const std::string& name) {
    if (isBuiltin(name))
        return "\"" + name + "\" is built in and cannot be deleted.";
    std::error_code ec;
    if (!fs::remove(fileOf(name), ec))
        return "No workspace named \"" + name + "\".";
    return {};
}

std::string WorkspaceStore::rename(const std::string& from, const std::string& to) {
    if (isBuiltin(from))
        return "\"" + from + "\" is built in and cannot be renamed.";
    const std::string target = trimmed(to);
    if (target.empty())
        return "Give the workspace a name.";
    if (isBuiltin(target) || (lowerOf(target) != lowerOf(from) && load(target)))
        return "A workspace named \"" + target + "\" already exists.";
    auto ws = load(from);
    if (!ws)
        return "No workspace named \"" + from + "\".";
    const std::string old = fileOf(from).string();
    ws->name = target;
    if (std::string err = save(*ws); !err.empty())
        return err;
    if (fileOf(from) != fileOf(target)) {
        std::error_code ec;
        fs::remove(fileOf(from), ec);
    }
    return {};
}

}  // namespace tbcad::ui
