#include "tbcad/block_cmds/palette.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <unordered_map>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"
#include "tbcad/xref/xref.h"

namespace tbcad::block_cmds {

namespace {

constexpr std::size_t kMaxPoints = 20000;
constexpr std::size_t kMaxLibraryItems = 2000;
constexpr std::uintmax_t kMaxLibraryFile = std::uintmax_t{40} << 20;

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Collector {
    std::vector<std::vector<Vec2>> strokes{};
    std::size_t points = 0;
    BBox2 box{};

    void add(std::vector<Vec2> pts, bool closed) {
        if (pts.size() < 2 || points >= kMaxPoints)
            return;
        if (closed && distance(pts.front(), pts.back()) > 0)
            pts.push_back(pts.front());
        for (Vec2 p : pts)
            box.expand(p);
        points += pts.size();
        strokes.push_back(std::move(pts));
    }

    void add(const DisplayList& list) {
        for (const DisplayItem& item : list.items) {
            if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
                add(pl->pts, pl->closed);
            } else if (const auto* fill = std::get_if<DisplayFill>(&item)) {
                for (const auto& ring : fill->rings)
                    add(ring, true);
            }
        }
    }

    Thumbnail finish() {
        Thumbnail t;
        if (strokes.empty() || box.empty())
            return t;
        const double w = box.max.x - box.min.x, h = box.max.y - box.min.y;
        const double side = std::max(w, h);
        if (!(side > 0))
            return t;
        for (auto& s : strokes)
            for (Vec2& p : s)
                p = {(p.x - box.min.x) / side, (p.y - box.min.y) / side};
        t.strokes = std::move(strokes);
        t.width = w / side;
        t.height = h / side;
        return t;
    }
};

}  // namespace

Thumbnail thumbnailOfBlock(const Document& doc, BlockId block) {
    const BlockDef* def = doc.block(block);
    if (!def)
        return {};
    Entity ins;
    ins.common.layer = kLayerZero;
    ins.data = Indirect<InsertData>(InsertData{block, def->base, {1, 1}, 0});
    BBox2 extent = blocks::insertBounds(doc, ins);
    if (extent.empty())
        return {};
    geom::DisplayContext dc;
    dc.pixelSizeMm = std::max(extent.max.x - extent.min.x, extent.max.y - extent.min.y) / 128;
    if (!(dc.pixelSizeMm > 0))
        return {};
    DisplayList list;
    blocks::buildInsertDisplay(doc, ins, dc, list);
    Collector c;
    c.add(list);
    return c.finish();
}

Thumbnail thumbnailOfModel(const Document& doc) {
    BBox2 extent;
    std::vector<EntityId> ids = doc.modelSpace();
    for (EntityId id : ids)
        if (const Entity* e = doc.find(id))
            if (e->common.visible)
                extent.merge(geom::bounds(*e, doc));
    if (extent.empty())
        return {};
    geom::DisplayContext dc;
    dc.pixelSizeMm = std::max(extent.max.x - extent.min.x, extent.max.y - extent.min.y) / 128;
    if (!(dc.pixelSizeMm > 0))
        return {};
    Collector c;
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e || !e->common.visible)
            continue;
        DisplayList list;
        geom::buildDisplay(doc, *e, dc, list);
        c.add(list);
    }
    return c.finish();
}

std::vector<LibraryItem> scanLibrary(const std::filesystem::path& folder,
                                     const std::string& filter) {
    namespace fs = std::filesystem;
    std::vector<LibraryItem> out;
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec))
        return out;
    const std::string needle = lower(filter);
    fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec),
        end;
    for (; !ec && it != end && out.size() < kMaxLibraryItems; it.increment(ec)) {
        if (it.depth() >= 3) {
            it.disable_recursion_pending();
        }
        std::error_code e2;
        if (!it->is_regular_file(e2))
            continue;
        const std::string ext = lower(it->path().extension().string());
        if (ext != ".tbcad" && ext != ".dxf")
            continue;
        LibraryItem item{it->path(), it->path().stem().string()};
        if (!needle.empty() && lower(item.name).find(needle) == std::string::npos)
            continue;
        out.push_back(std::move(item));
    }
    std::sort(out.begin(), out.end(), [](const LibraryItem& a, const LibraryItem& b) {
        std::string la = lower(a.name), lb = lower(b.name);
        return la != lb ? la < lb : a.path < b.path;
    });
    return out;
}

const Thumbnail* LibraryThumbnails::find(const std::filesystem::path& file) const {
    auto it = entries_.find(file);
    if (it == entries_.end())
        return nullptr;
    return it->second.stamp == xref::fileStamp(file) ? &it->second.thumbnail : nullptr;
}

bool LibraryThumbnails::loadNext(const std::vector<LibraryItem>& items) {
    for (const LibraryItem& item : items) {
        if (find(item.path))
            continue;
        Entry entry;
        entry.stamp = xref::fileStamp(item.path);
        std::error_code ec;
        if (std::filesystem::file_size(item.path, ec) <= kMaxLibraryFile && !ec) {
            auto src = xref::loadSource(item.path);
            if (src.ok() && src.value().doc)
                entry.thumbnail = thumbnailOfModel(*src.value().doc);
        }
        entries_[item.path] = std::move(entry);
        return true;
    }
    return false;
}

std::filesystem::path libraryFileFor(const std::string& typed,
                                     const std::filesystem::path& drawingFile,
                                     const std::filesystem::path& libraryFolder) {
    namespace fs = std::filesystem;
    std::string text = typed;
    while (!text.empty() && text.front() == ' ')
        text.erase(text.begin());
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);
    if (text.empty())
        return {};
    fs::path p{std::u8string(text.begin(), text.end())};
    std::vector<fs::path> candidates;
    if (p.is_absolute()) {
        candidates.push_back(p);
    } else {
        if (!drawingFile.empty())
            candidates.push_back(drawingFile.parent_path() / p);
        if (!libraryFolder.empty())
            candidates.push_back(libraryFolder / p);
        candidates.push_back(p);
    }
    std::error_code ec;
    for (const fs::path& c : candidates) {
        std::vector<fs::path> tries{c};
        if (c.extension().empty()) {
            fs::path withExt = c;
            withExt += ".tbcad";
            tries.push_back(withExt);
            fs::path asDxf = c;
            asDxf += ".dxf";
            tries.push_back(asDxf);
        }
        for (const fs::path& t : tries) {
            const std::string ext = lower(t.extension().string());
            if ((ext == ".tbcad" || ext == ".dxf") && fs::is_regular_file(t, ec))
                return t;
        }
    }
    return {};
}

namespace {

void countInto(const Document& doc, BlockId block, std::size_t mult, int depth,
               std::unordered_map<BlockId, std::size_t>& total) {
    if (depth > 64)
        return;
    total[block] += mult;
    const BlockDef* def = doc.block(block);
    if (!def)
        return;
    for (EntityId id : def->entities)
        if (const Entity* e = doc.find(id))
            if (const auto* ins = entityAs<InsertData>(*e))
                countInto(doc, ins->block, mult, depth + 1, total);
}

}  // namespace

std::vector<BlockCountRow> blockCount(const Document& doc) {
    std::unordered_map<BlockId, std::size_t> direct, total;
    for (EntityId id : doc.modelSpace())
        if (const Entity* e = doc.find(id))
            if (const auto* ins = entityAs<InsertData>(*e)) {
                ++direct[ins->block];
                countInto(doc, ins->block, 1, 0, total);
            }
    std::vector<BlockCountRow> rows;
    for (const auto& [id, n] : total) {
        const BlockDef* def = doc.block(id);
        if (!def || def->anonymous)
            continue;
        rows.push_back({def->name, direct[id], n});
    }
    std::sort(rows.begin(), rows.end(), [](const BlockCountRow& a, const BlockCountRow& b) {
        return lower(a.name) < lower(b.name);
    });
    return rows;
}

std::string blockCountCsv(const std::vector<BlockCountRow>& rows) {
    auto quote = [](const std::string& s) {
        if (s.find_first_of(",\"\r\n") == std::string::npos)
            return s;
        std::string q = "\"";
        for (char c : s) {
            if (c == '"')
                q += '"';
            q += c;
        }
        return q + "\"";
    };
    std::string out = "Block,Count,Total\r\n";
    for (const BlockCountRow& r : rows)
        out +=
            quote(r.name) + "," + std::to_string(r.direct) + "," + std::to_string(r.total) + "\r\n";
    return out;
}

}  // namespace tbcad::block_cmds
