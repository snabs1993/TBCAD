#include "tbcad/platform/settings.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
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
    o["view.transparency"] = true;
    o["files.filedia"] = true;  // 0: OPEN / SAVEAS ask for the path on the command line
    o["view.grid"] = false;
    o["drafting.snap"] = false;
    o["drafting.ortho"] = false;
    o["drafting.polar"] = true;   // as AutoCAD: green polar and object-snap tracking guides
    o["drafting.otrack"] = true;
    o["drafting.osnap"] = true;
    o["pick.aperturePx"] = 6;
    o["files.defaultFormat"] = "dxf";
    o["files.dxfVersion"] = "auto";  // auto | r12 | r2000 | r2007
    o["files.backup"] = true;        // ISAVEBAK: keep the previous file as <name>.bak
    o["files.isavePercent"] = 50;    // ISAVEPERCENT: accepted, saves are always whole files
    o["view.crosshairSize"] = 100;   // CURSORSIZE: % of the drawing area
    o["view.lockFade"] = 60;         // LAYLOCKFADECTL: % faded on locked layers
    o["grips.size"] = 4;             // GRIPSIZE: half the side of a grip, pixels
    o["osnap.markerSize"] = 12;      // AutoSnap marker size, pixels
    o["osnap.marker"] = true;        // AutoSnap marker shown
    o["osnap.apbox"] = false;        // APBOX: aperture box at point prompts
    o["update.check"] = true;        // look for a new version when the program starts (once a day)
    o["update.channel"] = "auto";    // auto | stable | test
    o["update.skip"] = "";           // the version the user chose to skip
    o["update.lastCheck"] = 0;       // seconds since 1970 of the last automatic check
    return s;
}

void Settings::resetToDefaults() {
    // The options of the Settings dialog; recent files, workspaces and folders stay.
    static constexpr std::string_view kPrefixes[] = {"ui.theme",
                                                     "ui.scale",
                                                     "ui.wheel.",
                                                     "ui.rightClick",
                                                     "view.",
                                                     "hover.",
                                                     "hud.",
                                                     "pick.",
                                                     "autosave.",
                                                     "drafting.",
                                                     "files.defaultFormat",
                                                     "files.dxfVersion",
                                                     "files.backup",
                                                     "files.isavePercent",
                                                     "grips.",
                                                     "osnap.",
                                                     "ui.viewportBackground"};
    auto& o = root_.object();
    for (auto it = o.begin(); it != o.end();) {
        bool drop = false;
        for (std::string_view p : kPrefixes)
            drop = drop || it->first.rfind(p, 0) == 0;
        it = drop ? o.erase(it) : std::next(it);
    }
    Settings defaults = withDefaults();  // a named object: it must outlive the loop
    for (const auto& [k, v] : defaults.root_.object())
        o[k] = v;
    dirty_ = true;
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

std::vector<std::string> Settings::repair() {
    std::vector<std::string> fixed;
    auto& o = root_.object();
    const Settings defaults = withDefaults();
    for (const auto& [k, v] : defaults.root_.asObject()) {
        auto it = o.find(k);
        if (it == o.end())
            continue;
        const Json& cur = it->second;
        const bool same = (v.isBool() && cur.isBool()) || (v.isNumber() && cur.isNumber()) ||
                          (v.isString() && cur.isString());
        if (!same) {
            it->second = v;
            fixed.push_back("Setting " + k + " had a value of the wrong type; using the default");
        }
    }
    // The ranges of the Settings dialog.
    struct Range {
        const char* key;
        double lo, hi, fallback;
    };
    static constexpr Range kRanges[] = {{"ui.scale", 0.75, 2.0, 1.0},
                                        {"pick.aperturePx", 1, 50, 6},
                                        {"view.crosshairSize", 1, 100, 100},
                                        {"view.lockFade", -90, 90, 60},
                                        {"grips.size", 1, 20, 4},
                                        {"osnap.markerSize", 4, 40, 12},
                                        {"ui.wheel.delayMs", 100, 800, 250},
                                        {"autosave.minutes", 1, 600, 10}};
    for (const Range& r : kRanges) {
        auto it = o.find(r.key);
        if (it == o.end())
            continue;
        if (!it->second.isNumber()) {
            o.erase(it);
            fixed.push_back(std::string("Setting ") + r.key +
                            " was not a number; using the default");
            continue;
        }
        const double v = it->second.asNumber();
        const double c = std::isfinite(v) ? std::clamp(v, r.lo, r.hi) : r.fallback;
        if (c != v) {
            it->second = c;
            fixed.push_back(std::string("Setting ") + r.key + " was out of range; using " +
                            toJson(Json(c), false));
        }
    }
    if (!fixed.empty())
        dirty_ = true;
    return fixed;
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
