#include <cctype>
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

bool fontAvailable(const std::string& family, const std::string& file) {
    font::FontLibrary& lib = fonts();
    if (!file.empty())
        return isStrokeFile(file) ? lib.loadStroke(std::filesystem::path(file)).ok()
                                  : lib.load(std::filesystem::path(file)).ok();
    if (isStrokeFamily(family))
        return lib.loadBundledStroke().ok();
    return lib.findSystem(family, false, false).has_value();
}

font::FontId fontForStyle(const TextStyle* style, bool bold, bool italic) {
    using Key = std::tuple<std::string, std::string, bool, bool>;
    static std::mutex mutex;
    static std::map<Key, font::FontId> cache;

    std::string family = style ? style->fontFamily : std::string{};
    std::string file = style ? style->fontFile : std::string{};
    Key key{family, file, bold, italic};
    std::lock_guard lock(mutex);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    font::FontLibrary& lib = fonts();
    std::optional<font::FontId> found;
    if (!file.empty()) {
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
