#include "tbcad/block_cmds/block_list.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_map>

namespace tbcad::block_cmds {

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::vector<BlockRow> blockRows(const Document& doc, const std::string& filter) {
    // The insert counts walk every entity, which the panel would do on each frame: keep them
    // until the drawing changes.
    static const Document* cachedDoc = nullptr;
    static std::uint64_t cachedRevision = 0;
    static std::unordered_map<BlockId, std::size_t> refs;
    if (cachedDoc != &doc || cachedRevision != doc.revision()) {
        refs.clear();
        doc.forEachEntity([&](const Entity& e) {
            if (const auto* ins = entityAs<InsertData>(e))
                ++refs[ins->block];
        });
        cachedDoc = &doc;
        cachedRevision = doc.revision();
    }
    const std::string needle = lower(filter);
    std::vector<BlockRow> rows;
    for (const auto& [id, def] : doc.blocks()) {
        if (def.anonymous)
            continue;
        if (!needle.empty() && lower(def.name).find(needle) == std::string::npos)
            continue;
        rows.push_back({id, def.name, def.entities.size(), refs[id], def.base});
    }
    std::sort(rows.begin(), rows.end(),
              [](const BlockRow& a, const BlockRow& b) { return lower(a.name) < lower(b.name); });
    return rows;
}

}  // namespace tbcad::block_cmds
