#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include "text_internal.h"

namespace tbcad::text {

namespace {
font::FontLibrary& sharedFonts() {
    static font::FontLibrary lib;
    return lib;
}
std::atomic<std::uint64_t>& fontGeneration() {
    static std::atomic<std::uint64_t> g{1};
    return g;
}
}  // namespace

struct FontScope::Private {
    font::FontLibrary lib;
    std::map<std::tuple<std::string, std::string, bool, bool>, font::FontId> cache;
    std::uint64_t generation = 0;
};

namespace {
thread_local FontScope::Private* g_threadFonts = nullptr;
std::mutex& poolMutex() {
    static std::mutex m;
    return m;
}
std::vector<std::unique_ptr<FontScope::Private>>& pool() {
    static std::vector<std::unique_ptr<FontScope::Private>> p;
    return p;
}
}  // namespace

FontScope::FontScope() : previous_(g_threadFonts) {
    {
        std::lock_guard lock(poolMutex());
        if (!pool().empty()) {
            mine_ = pool().back().release();
            pool().pop_back();
        }
    }
    if (!mine_) {
        mine_ = new Private();
        mine_->lib.setBundledDirectory(sharedFonts().bundledDirectory());
    }
    g_threadFonts = mine_;
}

FontScope::~FontScope() {
    g_threadFonts = previous_;
    std::lock_guard lock(poolMutex());
    pool().emplace_back(mine_);
}

font::FontLibrary& fonts() {
    return g_threadFonts ? g_threadFonts->lib : sharedFonts();
}

const TextStyle* styleFor(const Document& doc, TextStyleId id) {
    if (id.value != 0) {
        if (const TextStyle* s = doc.textStyle(id))
            return s;
    }
    if (const TextStyle* s = doc.textStyle(doc.header().currentTextStyle))
        return s;
    return doc.textStyle(kTextStyleStandard);
}

bool isStrokeFamily(std::string_view family) {
    constexpr std::string_view kName = "TBCAD Stroke";
    if (family.size() != kName.size())
        return false;
    for (std::size_t i = 0; i < family.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(family[i])) !=
            std::tolower(static_cast<unsigned char>(kName[i])))
            return false;
    return true;
}

bool isStrokeFile(std::string_view path) {
    constexpr std::string_view kExt = ".tbsf";
    if (path.size() < kExt.size())
        return false;
    std::string_view tail = path.substr(path.size() - kExt.size());
    for (std::size_t i = 0; i < kExt.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(tail[i])) != kExt[i])
            return false;
    return true;
}

namespace {
using Key = std::tuple<std::string, std::string, bool, bool>;
std::mutex& cacheMutex() {
    static std::mutex m;
    return m;
}
std::map<Key, font::FontId>& fontCache() {
    static std::map<Key, font::FontId> c;
    return c;
}
std::mutex& pathMutex() {
    static std::mutex m;
    return m;
}
// Looking an SHX font up scans the font directories, and every text layout asks (the width factor
// depends on it), so the answer is kept: a drawing with 100,000 texts took 18 s to open with
// the Windows font folder on the search path. A font that was not found is looked for again after a
// few seconds, so installing it takes effect.
struct ShxAnswer {
    FontResolution resolution;
    std::chrono::steady_clock::time_point at;
};
std::mutex& shxMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, ShxAnswer>& shxAnswers() {
    static std::map<std::string, ShxAnswer> c;
    return c;
}
std::vector<std::filesystem::path>& extraPaths() {
    static std::vector<std::filesystem::path> p;
    return p;
}
bool endsWithNoCase(std::string_view s, std::string_view ext) {
    if (s.size() < ext.size())
        return false;
    for (std::size_t i = 0; i < ext.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - ext.size() + i])) !=
            std::tolower(static_cast<unsigned char>(ext[i])))
            return false;
    return true;
}
bool isOutlineFontFile(std::string_view name) {
    return endsWithNoCase(name, ".ttf") || endsWithNoCase(name, ".ttc") ||
           endsWithNoCase(name, ".otf") || endsWithNoCase(name, ".otc");
}
// The .shx font a style asks for, if any.
std::string shxNameOf(const TextStyle& st) {
    if (endsWithNoCase(st.fontFile, ".shx"))
        return st.fontFile;
    return st.shxFile;
}
}  // namespace

bool isShxName(std::string_view name) {
    return endsWithNoCase(name, ".shx");
}

void setFontSearchPaths(std::vector<std::filesystem::path> dirs) {
    {
        std::lock_guard lock(pathMutex());
        extraPaths() = std::move(dirs);
    }
    {
        std::lock_guard lock(shxMutex());
        shxAnswers().clear();
    }
    fontGeneration().fetch_add(1);
    std::lock_guard lock(cacheMutex());
    fontCache().clear();
}

#ifdef _WIN32
namespace {
// The Fonts folders of CAD programs under Program Files, newest version first (found once).
const std::vector<std::filesystem::path>& installedCadFontDirs() {
    static const std::vector<std::filesystem::path> dirs = [] {
        namespace fs = std::filesystem;
        std::vector<fs::path> out;
        std::error_code ec;
        for (const char* var : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"}) {
            const char* root = std::getenv(var);
            if (!root)
                continue;
            const fs::path vendor = fs::path(root) / "Autodesk";
            if (!fs::is_directory(vendor, ec))
                continue;
            std::vector<fs::path> found;
            for (const auto& entry : fs::directory_iterator(vendor, ec))
                if (entry.is_directory(ec) && fs::is_directory(entry.path() / "Fonts", ec))
                    found.push_back(entry.path() / "Fonts");
            std::sort(found.begin(), found.end(), std::greater<>());
            for (fs::path& f : found)
                if (std::find(out.begin(), out.end(), f) == out.end())
                    out.push_back(std::move(f));
        }
        return out;
    }();
    return dirs;
}
}  // namespace
#endif

std::filesystem::path locateFontFile(const std::string& name) {
    namespace fs = std::filesystem;
    if (name.empty())
        return {};
    std::error_code ec;
    const fs::path given(name);
    if (given.is_absolute() && fs::is_regular_file(given, ec))
        return given;
    std::vector<fs::path> dirs;
    {
        std::lock_guard lock(pathMutex());
        dirs = extraPaths();
    }
    if (const char* env = std::getenv("TBCAD_FONT_PATH")) {
        std::string e = env;
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::size_t from = 0;
        while (from <= e.size()) {
            std::size_t to = e.find(sep, from);
            if (to == std::string::npos)
                to = e.size();
            if (to > from)
                dirs.emplace_back(e.substr(from, to - from));
            from = to + 1;
        }
    }
    dirs.emplace_back("assets/fonts/shx");
#ifdef TBCAD_BUNDLED_FONT_DIR
    dirs.emplace_back(fs::path(TBCAD_BUNDLED_FONT_DIR) / "shx");
#endif
#ifdef _WIN32
    if (const char* windir = std::getenv("WINDIR"))
        dirs.emplace_back(fs::path(windir) / "Fonts");
    dirs.emplace_back("C:/Windows/Fonts");
    // Fonts installed for the current user only.
    if (const char* local = std::getenv("LOCALAPPDATA"))
        dirs.emplace_back(fs::path(local) / "Microsoft" / "Windows" / "Fonts");
    // SHX fonts that come with another CAD program on this computer (romans.shx and the like):
    // the real font draws text exactly as in that program's plots.
    for (const fs::path& d : installedCadFontDirs())
        dirs.push_back(d);
#else
    dirs.emplace_back("/usr/share/fonts");
    dirs.emplace_back("/usr/local/share/fonts");
#endif
    const std::string file = given.filename().string();
    auto lower = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    for (const fs::path& d : dirs) {
        if (!fs::is_directory(d, ec))
            continue;
        if (fs::is_regular_file(d / file, ec))
            return d / file;
        // File names differ in case between systems: look the name up without it.
        const std::string want = lower(file);
        for (const auto& entry : fs::directory_iterator(d, ec))
            if (entry.is_regular_file(ec) && lower(entry.path().filename().string()) == want)
                return entry.path();
    }
    return {};
}

namespace {
FontResolution resolveShx(const std::string& shx) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(shxMutex());
        auto it = shxAnswers().find(shx);
        if (it != shxAnswers().end() && (it->second.resolution.kind == FontResolution::Kind::Shx ||
                                         now - it->second.at < std::chrono::seconds(3)))
            return it->second.resolution;
    }
    FontResolution r;
    r.wanted = shx;
    const auto path = locateFontFile(shx);
    if (!path.empty() && fonts().loadStroke(path).ok()) {
        r.kind = FontResolution::Kind::Shx;
        r.used = path.string();
    } else {
        r.kind = FontResolution::Kind::Substitute;
        r.used = "TBCAD Stroke";
    }
    std::lock_guard lock(shxMutex());
    shxAnswers()[shx] = ShxAnswer{r, now};
    return r;
}
}  // namespace

FontResolution resolveStyleFont(const TextStyle& st) {
    FontResolution r;
    const std::string shx = shxNameOf(st);
    if (!shx.empty())
        return resolveShx(shx);
    r.wanted = !st.fontFile.empty() ? st.fontFile : st.fontFamily;
    if (!r.wanted.empty() && !fontAvailable(st.fontFamily, st.fontFile)) {
        r.kind = FontResolution::Kind::Substitute;
        r.used = "Sarabun";
    } else {
        r.used = r.wanted.empty() ? "Sarabun" : r.wanted;
    }
    return r;
}

std::vector<std::pair<std::string, FontResolution>> missingFonts(const Document& doc) {
    std::vector<std::pair<std::string, FontResolution>> out;
    for (const auto& [id, st] : doc.textStyles()) {
        FontResolution r = resolveStyleFont(st);
        if (r.kind == FontResolution::Kind::Substitute)
            out.emplace_back(st.name, std::move(r));
    }
    return out;
}

double effectiveWidthFactor(const TextStyle* style) {
    if (!style)
        return 1.0;
    // A style that was read from an SHX font carries the stand-in's 1/0.92; the real font needs
    // none.
    if (!style->shxFile.empty() && !endsWithNoCase(style->fontFile, ".shx") &&
        resolveStyleFont(*style).kind == FontResolution::Kind::Shx)
        return style->widthFactor * 0.92;
    return style->widthFactor;
}

bool fontAvailable(const std::string& family, const std::string& file) {
    font::FontLibrary& lib = fonts();
    if (endsWithNoCase(file, ".shx"))
        return !locateFontFile(file).empty();
    if (!file.empty())
        return isStrokeFile(file) ? lib.loadStroke(std::filesystem::path(file)).ok()
                                  : lib.load(std::filesystem::path(file)).ok();
    if (isStrokeFamily(family))
        return lib.loadBundledStroke().ok();
    return lib.findSystem(family, false, false).has_value();
}

font::FontId fontForStyle(const TextStyle* style, bool bold, bool italic) {
    std::string family = style ? style->fontFamily : std::string{};
    std::string file = style ? style->fontFile : std::string{};
    Key key{family, file + '\n' + (style ? style->shxFile : std::string{}), bold, italic};
    // A thread with a library of its own keeps its answers in it, without taking the lock.
    std::unique_lock<std::mutex> lock;
    std::map<Key, font::FontId>* cachePtr;
    if (g_threadFonts) {
        if (g_threadFonts->generation != fontGeneration().load()) {
            g_threadFonts->cache.clear();
            g_threadFonts->generation = fontGeneration().load();
        }
        cachePtr = &g_threadFonts->cache;
    } else {
        lock = std::unique_lock<std::mutex>(cacheMutex());
        cachePtr = &fontCache();
    }
    auto& cache = *cachePtr;
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    font::FontLibrary& lib = fonts();
    std::optional<font::FontId> found;
    const auto lookupStart = std::chrono::steady_clock::now();
    if (style) {
        const std::string shx = shxNameOf(*style);
        if (!shx.empty()) {
            const auto path = locateFontFile(shx);
            if (!path.empty()) {
                auto r = lib.loadStroke(path);
                if (r.ok())
                    found = r.value();
            }
            if (!found) {  // missing: the stroke font stands in
                auto r = lib.loadBundledStroke();
                if (r.ok())
                    found = r.value();
            }
        }
    }
    if (!found && !file.empty() && !endsWithNoCase(file, ".shx")) {
        // Drawings name the file bare ("arial.ttf") or with the author's folders: look it up in
        // the font folders when it is not where the name says.
        std::filesystem::path path(file);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
            if (auto located = locateFontFile(file); !located.empty())
                path = located;
        auto r = isStrokeFile(file) ? lib.loadStroke(path) : lib.load(path);
        if (r.ok())
            found = r.value();
        // Not on this machine: the installed font of the same name ("arial" for arial.ttf).
        // Only for a TrueType / OpenType file name: a bare "txt" (TBCAD writes it for a style
        // with no font) must not become AutoCAD's installed "Txt" font on the way back in.
        if (!found && family.empty() && isOutlineFontFile(file))
            found = lib.findSystem(std::filesystem::path(file).stem().string(), bold, italic);
    }
    if (!found && isStrokeFamily(family)) {
        auto r = lib.loadBundledStroke();
        if (r.ok())
            found = r.value();
    }
    if (!found && !family.empty()) {
        found = lib.findSystem(family, bold, italic);
        if (!found && (bold || italic))
            found = lib.findSystem(family, false, false);
    }
    if (!found) {
        auto r = lib.loadBundled("Sarabun");
        if (r.ok())
            found = r.value();
    }
    font::FontId id = found.value_or(kNoFont);
    // TBCAD_FONT_PROFILE / TBCAD_INDEX_PROFILE: a font lookup that took long, on stderr.
    static const bool profile = [] {
        for (const char* var : {"TBCAD_FONT_PROFILE", "TBCAD_INDEX_PROFILE"})
            if (const char* v = std::getenv(var); v && *v && *v != '0')
                return true;
        return false;
    }();
    if (profile) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - lookupStart)
                              .count();
        if (ms > 20)
            std::fprintf(stderr,
                         "font profile: style '%s' (family '%s', file '%s', shx '%s'%s%s) took "
                         "%.1f ms\n",
                         style ? style->name.c_str() : "", family.c_str(), file.c_str(),
                         style ? style->shxFile.c_str() : "", bold ? ", bold" : "",
                         italic ? ", italic" : "", ms);
    }
    cache.emplace(std::move(key), id);
    return id;
}

}  // namespace tbcad::text
