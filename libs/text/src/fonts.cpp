#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <tuple>

#include "text_internal.h"

namespace tbcad::text {

font::FontLibrary& fonts() {
    static font::FontLibrary lib;
    return lib;
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
    std::lock_guard lock(cacheMutex());
    fontCache().clear();
}

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
    dirs.emplace_back("C:/Windows/Fonts");
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

FontResolution resolveStyleFont(const TextStyle& st) {
    FontResolution r;
    const std::string shx = shxNameOf(st);
    if (!shx.empty()) {
        r.wanted = shx;
        const auto path = locateFontFile(shx);
        if (!path.empty() && fonts().loadStroke(path).ok()) {
            r.kind = FontResolution::Kind::Shx;
            r.used = path.string();
            return r;
        }
        r.kind = FontResolution::Kind::Substitute;
        r.used = "TBCAD Stroke";
        return r;
    }
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
    std::lock_guard lock(cacheMutex());
    auto& cache = fontCache();
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    font::FontLibrary& lib = fonts();
    std::optional<font::FontId> found;
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
        auto r = isStrokeFile(file) ? lib.loadStroke(std::filesystem::path(file))
                                    : lib.load(std::filesystem::path(file));
        if (r.ok())
            found = r.value();
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
    cache.emplace(std::move(key), id);
    return id;
}

}  // namespace tbcad::text
