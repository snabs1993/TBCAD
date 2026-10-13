#include "tbcad/platform/settings.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace tbcad::platform {

Settings Settings::withDefaults() {
    Settings s;
    auto& o = s.root_.object();
    o["ui.theme"] = "dark";
    o["ui.scale"] = 1.0;
    o["autosave.enabled"] = true;
    o["autosave.minutes"] = 10;
    o["view.lineweights"] = true;
    o["view.grid"] = false;
    o["drafting.snap"] = false;
    o["drafting.ortho"] = false;
    o["drafting.polar"] = false;
    o["drafting.osnap"] = true;
    o["pick.aperturePx"] = 6;
    return s;
}

Result<void> Settings::load(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec))
        return {};
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
    for (const auto& [k, v] : parsed.value().asObject())
        root_.object()[k] = v;
    return {};
}

Result<void> Settings::save(const std::filesystem::path& file) const {
    std::error_code ec;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return makeError(ErrorCode::Io, "Cannot write " + tmp.string());
        out << toJson(root_, true);
        out.flush();
        if (!out)
            return makeError(ErrorCode::Io, "Cannot write " + tmp.string());
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return makeError(ErrorCode::Io, "Cannot replace " + file.string());
    }
    dirty_ = false;
    return {};
}

bool Settings::has(std::string_view key) const {
    return root_.find(std::string(key)) != nullptr;
}

Settings::Kind Settings::kindOf(std::string_view key) const {
    const Json* v = root_.find(std::string(key));
    if (!v)
        return Kind::Missing;
    if (v->isBool())
        return Kind::Bool;
    if (v->isNumber())
        return Kind::Number;
    if (v->isString())
        return Kind::String;
    return Kind::Other;
}

bool Settings::getBool(std::string_view key, bool fallback) const {
    const Json* v = root_.find(std::string(key));
    return v && v->isBool() ? v->asBool() : fallback;
}
double Settings::getNumber(std::string_view key, double fallback) const {
    const Json* v = root_.find(std::string(key));
    return v && v->isNumber() ? v->asNumber() : fallback;
}
std::string Settings::getString(std::string_view key, std::string_view fallback) const {
    const Json* v = root_.find(std::string(key));
    return v && v->isString() ? v->asString() : std::string(fallback);
}
std::vector<std::string> Settings::getStrings(std::string_view key) const {
    std::vector<std::string> out;
    const Json* v = root_.find(std::string(key));
    if (v && v->isArray())
        for (const Json& e : v->asArray())
            if (e.isString())
                out.push_back(e.asString());
    return out;
}

void Settings::setBool(std::string_view key, bool v) {
    root_.object()[std::string(key)] = v;
    dirty_ = true;
}
void Settings::setNumber(std::string_view key, double v) {
    root_.object()[std::string(key)] = v;
    dirty_ = true;
}
void Settings::setString(std::string_view key, std::string v) {
    root_.object()[std::string(key)] = std::move(v);
    dirty_ = true;
}
void Settings::setStrings(std::string_view key, const std::vector<std::string>& v) {
    Json::Array a;
    for (const auto& s : v)
        a.emplace_back(s);
    root_.object()[std::string(key)] = Json(std::move(a));
    dirty_ = true;
}

void Settings::addRecentFile(const std::string& path) {
    auto list = recentFiles();
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    list.insert(list.begin(), path);
    if (list.size() > kMaxRecent)
        list.resize(kMaxRecent);
    setStrings("files.recent", list);
}
void Settings::removeRecentFile(const std::string& path) {
    auto list = recentFiles();
    list.erase(std::remove(list.begin(), list.end(), path), list.end());
    setStrings("files.recent", list);
}

}  // namespace tbcad::platform
