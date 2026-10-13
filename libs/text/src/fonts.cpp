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
        auto r = lib.load(std::filesystem::path(file));
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
