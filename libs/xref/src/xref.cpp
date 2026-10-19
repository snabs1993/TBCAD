#include "tbcad/xref/xref.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "tbcad/blocks/blocks.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/underlay/image.h"
#include "tbcad/underlay/pdf.h"

namespace tbcad::xref {

namespace {

constexpr int kMaxNesting = 64;

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// An insert of an overlay xref: left out when its drawing is used as an xref by another one.
bool isOverlayInsert(const Document& doc, const Entity& e) {
    if (const auto* ins = entityAs<InsertData>(e))
        if (const BlockDef* b = doc.block(ins->block))
            return !b->xrefPath.empty() && b->xrefOverlay;
    return false;
}

// Copies the model space and the blocks it uses of `src` into the host document.
class Filler {
public:
    // `merge`: the source's layers are matched by name with the host's own (a file used as a
    // block) instead of being hung below a layer named like the xref.
    Filler(Transaction& tx, Document& host, const Document& src, std::string name, Summary& summary,
           bool merge = false)
        : tx_(tx), host_(host), src_(src), name_(std::move(name)), sum_(summary), merge_(merge) {}

    Result<void> run(BlockId into) {
        if (!merge_) {
            auto root = rootLayer();
            if (!root)
                return root.error();
            root_ = root.value();
        }
        for (EntityId id : src_.modelSpace()) {
            const Entity* e = src_.find(id);
            if (!e || isOverlayInsert(src_, *e))
                continue;
            auto added = tx_.add(remap(*e, 0), into);
            if (!added)
                return added.error();
            ++sum_.entities;
        }
        return failure_.empty() ? Result<void>{} : makeError(ErrorCode::InvalidArgument, failure_);
    }

    // Used by importContent: copy one row (and what it needs) from the source.
    LayerId importLayer(LayerId id) { return mapLayer(id); }
    LinetypeId importLinetype(LinetypeId id) { return mapLinetype(id); }
    TextStyleId importTextStyle(TextStyleId id) { return mapTextStyle(id); }
    DimStyleId importDimStyle(DimStyleId id) { return mapDimStyle(id); }
    // Copies the entities of the source block `sb` into the new host block `into`.
    Result<void> runBlock(const BlockDef& sb, BlockId into) {
        std::vector<EntityId> members = sb.entities;
        for (EntityId id : members) {
            const Entity* e = src_.find(id);
            if (!e)
                continue;
            auto added = tx_.add(remap(*e, 0), into);
            if (!added)
                return added.error();
            ++sum_.entities;
        }
        return failure_.empty() ? Result<void>{} : makeError(ErrorCode::InvalidArgument, failure_);
    }

private:
    Result<LayerId> rootLayer() {
        if (const Layer* l = host_.layerByName(name_))
            return l->id;
        Layer row;
        row.name = name_;
        auto r = tx_.addLayer(std::move(row));
        if (r)
            ++sum_.layers;
        return r;
    }

    LinetypeId mapLinetype(LinetypeId id) {
        if (id == kLinetypeByLayer || id == kLinetypeByBlock || !id)
            return id;
        auto hit = linetypes_.find(id);
        if (hit != linetypes_.end())
            return hit->second;
        LinetypeId out = kLinetypeContinuous;
        if (const Linetype* lt = src_.linetype(id)) {
            if (const Linetype* same = host_.linetypeByName(lt->name)) {
                out = same->id;
            } else {
                Linetype row = *lt;
                row.id = {};
                if (auto r = tx_.addLinetype(std::move(row)))
                    out = r.value();
            }
        }
        linetypes_[id] = out;
        return out;
    }

    TextStyleId mapTextStyle(TextStyleId id) {
        if (!id)
            return id;
        auto hit = textStyles_.find(id);
        if (hit != textStyles_.end())
            return hit->second;
        TextStyleId out{};
        if (const TextStyle* ts = src_.textStyle(id)) {
            if (const TextStyle* same = host_.textStyleByName(ts->name)) {
                out = same->id;
            } else {
                TextStyle row = *ts;
                row.id = {};
                if (auto r = tx_.addTextStyle(std::move(row)))
                    out = r.value();
            }
        }
        textStyles_[id] = out;
        return out;
    }

    DimStyleId mapDimStyle(DimStyleId id) {
        if (!id)
            return id;
        auto hit = dimStyles_.find(id);
        if (hit != dimStyles_.end())
            return hit->second;
        DimStyleId out{};
        if (const DimStyle* ds = src_.dimStyle(id)) {
            if (const DimStyle* same = host_.dimStyleByName(ds->name)) {
                out = same->id;
            } else {
                DimStyle row = *ds;
                row.id = {};
                row.textStyle = mapTextStyle(ds->textStyle);
                if (auto r = tx_.addDimStyle(std::move(row)))
                    out = r.value();
            }
        }
        dimStyles_[id] = out;
        return out;
    }

    // The host layer for a source layer: the source tree hangs below the xref's root layer.
    LayerId mapLayer(LayerId id) {
        if (!id)
            id = kLayerZero;
        if (merge_ && id == kLayerZero)
            return kLayerZero;  // layer 0 keeps meaning "the layer of the insert"
        auto hit = layers_.find(id);
        if (hit != layers_.end())
            return hit->second;
        const Layer* l = src_.layer(id);
        if (!l)
            return layers_[id] = root_;
        LayerId parent = l->parent ? mapLayer(l->parent) : root_;
        LayerId out = parent;
        if (const Layer* same = host_.layerByName(parent, l->name)) {
            out = same->id;
        } else {
            Layer row = *l;
            row.id = {};
            row.parent = parent;
            row.linetype = mapLinetype(l->linetype);
            if (!row.linetype || row.linetype == kLinetypeByLayer ||
                row.linetype == kLinetypeByBlock)
                row.linetype = kLinetypeContinuous;
            // A tree deeper than the sub-layer limit collapses into the deepest allowed layer.
            if (auto r = tx_.addLayer(std::move(row))) {
                out = r.value();
                ++sum_.layers;
            }
        }
        layers_[id] = out;
        return out;
    }

    BlockId mapBlock(BlockId id, int depth) {
        auto hit = blocks_.find(id);
        if (hit != blocks_.end())
            return hit->second;
        const BlockDef* sb = src_.block(id);
        if (!sb || depth > kMaxNesting)
            return {};
        std::string base = name_ + "$" + sb->name;
        std::string use = base;
        for (int n = 2; host_.blockByName(use); ++n)
            use = base + "#" + std::to_string(n);
        BlockDef row;
        row.name = use;
        row.base = sb->base;
        row.anonymous = sb->anonymous;
        auto added = tx_.addBlock(std::move(row));
        if (!added) {
            failure_ = added.error().message;
            return {};
        }
        BlockId out = added.value();
        blocks_[id] = out;
        ++sum_.blocks;
        std::vector<EntityId> members = sb->entities;
        for (EntityId eid : members) {
            const Entity* e = src_.find(eid);
            if (!e || isOverlayInsert(src_, *e))
                continue;
            Entity copy = remap(*e, depth + 1);
            if (auto r = tx_.add(std::move(copy), out); !r)
                failure_ = r.error().message;
        }
        return out;
    }

    Entity remap(const Entity& in, int depth) {
        Entity e = in;
        e.common.id = {};
        e.common.layer = mapLayer(in.common.layer);
        e.common.level = {};  // the host decides (the current level); block content ignores it
        e.common.linetype = mapLinetype(in.common.linetype);
        if (auto* t = entityAs<TextData>(e))
            t->style = mapTextStyle(t->style);
        else if (auto* m = entityAs<MTextData>(e))
            m->style = mapTextStyle(m->style);
        else if (auto* tb = entityAs<TableData>(e))
            tb->style = mapTextStyle(tb->style);
        else if (auto* d = entityAs<DimensionData>(e))
            d->style = mapDimStyle(d->style);
        else if (auto* l = entityAs<LeaderData>(e))
            l->style = mapDimStyle(l->style);
        else if (auto* ml = entityAs<MLeaderData>(e)) {
            ml->style = mapDimStyle(ml->style);
            if (ml->block)
                ml->block = mapBlock(ml->block, depth);
        } else if (auto* ins = entityAs<InsertData>(e))
            ins->block = mapBlock(ins->block, depth);
        return e;
    }

    Transaction& tx_;
    Document& host_;
    const Document& src_;
    std::string name_;
    Summary& sum_;
    bool merge_ = false;
    LayerId root_{};
    std::string failure_{};
    std::unordered_map<LayerId, LayerId> layers_{};
    std::unordered_map<LinetypeId, LinetypeId> linetypes_{};
    std::unordered_map<TextStyleId, TextStyleId> textStyles_{};
    std::unordered_map<DimStyleId, DimStyleId> dimStyles_{};
    std::unordered_map<BlockId, BlockId> blocks_{};
};

// Blocks reachable from `block` through inserts, excluding `block` itself.
std::set<BlockId> nestedBlocks(const Document& doc, BlockId block) {
    std::set<BlockId> seen;
    std::vector<BlockId> todo{block};
    while (!todo.empty()) {
        BlockId b = todo.back();
        todo.pop_back();
        const BlockDef* def = doc.block(b);
        if (!def)
            continue;
        for (EntityId id : def->entities)
            if (const Entity* e = doc.find(id))
                if (const auto* ins = entityAs<InsertData>(*e))
                    if (ins->block != block && seen.insert(ins->block).second)
                        todo.push_back(ins->block);
    }
    return seen;
}

void eraseContent(Transaction& tx, const Document& doc, BlockId block) {
    const BlockDef* def = doc.block(block);
    if (!def)
        return;
    (void)tx.eraseAllIn(block);
}

// Erases blocks that nothing references any more; a block still used elsewhere stays.
std::size_t eraseBlocks(Transaction& tx, std::set<BlockId> blocks) {
    std::size_t gone = 0;
    for (bool progress = true; progress && !blocks.empty();) {
        progress = false;
        for (auto it = blocks.begin(); it != blocks.end();) {
            if (tx.eraseBlock(*it).ok()) {
                it = blocks.erase(it);
                ++gone;
                progress = true;
            } else {
                ++it;
            }
        }
    }
    return gone;
}

void eraseLayerTree(Transaction& tx, const Document& doc, LayerId root) {
    std::vector<LayerId> order;
    std::function<void(LayerId)> walk = [&](LayerId id) {
        for (const auto& [lid, l] : doc.layers())
            if (l.parent == id)
                walk(lid);
        order.push_back(id);
    };
    walk(root);
    for (LayerId id : order)
        (void)tx.eraseLayer(id);
}

}  // namespace

namespace {
// Whether the drive or share a path starts with is there, asked once per run: a drive letter that
// is not mapped, or a network share that is off line, can take seconds to time out on every look.
bool rootReachable(const std::filesystem::path& p) {
    const std::filesystem::path root = p.root_path();
    if (root.empty() || root == p.root_directory())
        return true;  // a relative path, or one on the system's own root
    static std::mutex m;
    static std::map<std::filesystem::path, bool> known;
    std::lock_guard lock(m);
    if (auto it = known.find(root); it != known.end())
        return it->second;
    std::error_code ec;
    const bool there = std::filesystem::exists(root, ec);
    known[root] = there;
    return there;
}
}  // namespace

std::filesystem::path resolvePath(std::string_view stored, const std::filesystem::path& hostFile) {
    const std::string plain = underlay::parseSpec(stored).file;
    std::filesystem::path p{std::u8string(plain.begin(), plain.end())};
    if (hostFile.empty())
        return p;
    std::filesystem::path full = p.is_absolute() ? p : hostFile.parent_path() / p;
    // As AutoCAD does, a file that is not where the path says is looked for by its name in the
    // folder of the drawing (a project folder copied to another disk keeps working). The name
    // may follow a Windows separator that is not one on this system.
    std::error_code ec;
    if (rootReachable(full) && std::filesystem::is_regular_file(full, ec))
        return full;
    const std::size_t cut = plain.find_last_of("/\\");
    const std::string name = cut == std::string::npos ? plain : plain.substr(cut + 1);
    if (!name.empty()) {
        std::filesystem::path near =
            hostFile.parent_path() / std::filesystem::path(std::u8string(name.begin(), name.end()));
        if (near != full && std::filesystem::is_regular_file(near, ec))
            return near;
    }
    return full;
}

std::string rebaseStored(std::string_view stored, const std::filesystem::path& fromHost,
                         const std::filesystem::path& toHost) {
    if (fromHost.empty() || toHost.empty() || stored.empty())
        return std::string(stored);
    if (fromHost.parent_path() == toHost.parent_path())
        return std::string(stored);
    const std::string plain = underlay::parseSpec(stored).file;
    const std::filesystem::path p{std::u8string(plain.begin(), plain.end())};
    if (p.is_absolute() || p.empty())
        return std::string(stored);
    std::error_code ec;
    const std::filesystem::path abs =
        std::filesystem::weakly_canonical(fromHost.parent_path() / p, ec);
    if (ec)
        return std::string(stored);
    std::string out = storedPath(abs, toHost, PathType::Relative);
    const auto hash = stored.find('#');
    if (hash != std::string_view::npos)
        out += std::string(stored.substr(hash));
    return out;
}

std::size_t rebaseAll(Transaction& tx, const Document& doc, const std::filesystem::path& fromHost,
                      const std::filesystem::path& toHost) {
    std::vector<std::pair<BlockId, std::string>> todo;
    for (const auto& [id, b] : doc.blocks()) {
        if (b.xrefPath.empty())
            continue;
        std::string now = rebaseStored(b.xrefPath, fromHost, toHost);
        if (now != b.xrefPath)
            todo.emplace_back(id, std::move(now));
    }
    for (auto& [id, text] : todo)
        (void)tx.modifyBlock(id, [&](BlockDef& d) { d.xrefPath = std::move(text); });
    return todo.size();
}

std::int64_t fileStamp(const std::filesystem::path& file) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(file, ec);
    if (ec)
        return 0;
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count();
}

std::string defaultName(const std::filesystem::path& file) {
    std::string s = file.stem().string();
    for (char& c : s) {
        auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f ||
            std::string_view("<>/\\\":;?*|,=`$").find(c) != std::string_view::npos)
            c = '_';
    }
    while (!s.empty() && (s.front() == '*' || s.front() == ' '))
        s.erase(s.begin());
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s.empty() ? "xref" : s;
}

namespace {

Result<Source> loadPdf(const std::filesystem::path& file, const underlay::PdfOptions& options) {
    underlay::PdfReport report;
    auto doc = underlay::importPdfFile(file, options, &report);
    if (!doc)
        return doc.error();
    Source out;
    out.doc = std::move(doc.value());
    out.stamp = fileStamp(file);
    out.warnings = report.notes.size();
    return out;
}

Result<Source> loadImageFrame(const std::filesystem::path& file) {
    auto info = underlay::imageInfo(file);
    if (!info)
        return info.error();
    auto doc = underlay::imageFrameDocument(info.value().width, info.value().height);
    if (!doc)
        return doc.error();
    Source out;
    out.doc = std::move(doc.value());
    out.stamp = fileStamp(file);
    return out;
}

std::map<std::string, Loader>& loaders() {
    static std::map<std::string, Loader> table;
    return table;
}

Result<Source> loadOne(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec))
        return makeError(ErrorCode::NotFound, "Cannot find the file " + file.string());
    const std::string ext = lower(file.extension().string());
    if (ext == ".pdf")
        return loadPdf(file, {});
    if (underlay::isImagePath(file.string()))
        return loadImageFrame(file);
    Source out;
    out.stamp = fileStamp(file);
    out.file = file;
    if (ext == ".tbcad" || ext == ".tbt") {  // a template is a .tbcad file
        io::IoReport report;
        auto doc = io::readTbcad(file, report);
        if (!doc)
            return doc.error();
        out.doc = std::move(doc.value());
        out.warnings = report.warnings.size();
        return out;
    }
    if (ext == ".dxf") {
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        const std::streamoff size = in ? static_cast<std::streamoff>(in.tellg()) : 0;
        std::vector<std::byte> bytes(size > 0 ? static_cast<std::size_t>(size) : 0);
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        dxf::Report report;
        auto res = dxf::importDxf(bytes, dxf::ImportOptions{}, report);
        if (!res)
            return makeError(ErrorCode::Parse, res.error().message);
        out.doc = std::move(res.value().document);
        return out;
    }
    if (auto it = loaders().find(ext); it != loaders().end()) {
        auto res = it->second(file);
        if (!res)
            return res.error();
        res.value().file = file;
        if (res.value().stamp == 0)
            res.value().stamp = out.stamp;
        return res;
    }
    return makeError(ErrorCode::Unsupported,
                     "Only .tbcad, .tbt, .dxf, .dwg, .pdf and picture files can be attached");
}

// Files already read in one go (loadOnOpen): a drawing that several xrefs, or several levels,
// point at is parsed once.
using SourceCache = std::map<std::filesystem::path, std::shared_ptr<Result<Source>>>;

Result<Source> loadChain(const std::filesystem::path& file, std::set<std::filesystem::path>& chain,
                         int depth, SourceCache* cache = nullptr);

// The file through the cache (read on first use), or read afresh without one.
std::shared_ptr<Result<Source>> loadCached(const std::filesystem::path& file,
                                           std::set<std::filesystem::path>& chain, int depth,
                                           SourceCache* cache) {
    std::error_code ec;
    const std::filesystem::path canon = std::filesystem::weakly_canonical(file, ec);
    const std::filesystem::path& key = ec ? file : canon;
    if (cache)
        if (auto it = cache->find(key); it != cache->end())
            return it->second;
    auto loaded = std::make_shared<Result<Source>>(loadChain(file, chain, depth, cache));
    if (cache)
        (*cache)[key] = loaded;
    return loaded;
}

// Reads `file` and refreshes the xrefs inside it from their own files. `chain` holds the files
// being read up the call chain, so a loop of xrefs is cut where it closes.
Result<Source> loadChain(const std::filesystem::path& file, std::set<std::filesystem::path>& chain,
                         int depth, SourceCache* cache) {
    auto loaded = loadOne(file);
    if (!loaded || depth >= kMaxNesting)
        return loaded;
    Document& doc = *loaded.value().doc;
    std::vector<BlockId> nested;
    for (BlockId id : xrefs(doc))
        if (!doc.block(id)->xrefOverlay)
            nested.push_back(id);
    if (nested.empty())
        return loaded;
    std::error_code ec;
    std::filesystem::path self = std::filesystem::weakly_canonical(file, ec);
    chain.insert(ec ? file : self);
    {
        Transaction tx = doc.begin("refresh nested xrefs");
        for (BlockId id : nested) {
            std::filesystem::path p = resolvePath(doc.block(id)->xrefPath, file);
            std::filesystem::path key = std::filesystem::weakly_canonical(p, ec);
            if (chain.count(ec ? p : key))
                continue;  // a loop: keep the snapshot
            auto sub = loadCached(p, chain, depth + 1, cache);
            if (*sub)
                (void)reload(tx, doc, id, sub->value());
        }
        tx.commit();
    }
    chain.erase(ec ? file : self);
    return loaded;
}

}  // namespace

void registerLoader(std::string extension, Loader loader) {
    loaders()[lower(std::move(extension))] = std::move(loader);
}

std::string storedPath(const std::filesystem::path& file, const std::filesystem::path& hostFile,
                       PathType type) {
    auto text = [](const std::filesystem::path& p) {
        std::u8string u = p.generic_u8string();
        return std::string(u.begin(), u.end());
    };
    if (type == PathType::None)
        return text(file.filename());
    if (type == PathType::Relative && !hostFile.empty() && file.is_absolute()) {
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(file, hostFile.parent_path(), ec);
        if (!ec && !rel.empty())
            return text(rel);
    }
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(file, ec);
    return text(ec ? file : abs);
}

Result<Source> loadSource(const std::filesystem::path& file) {
    std::set<std::filesystem::path> chain;
    return loadChain(file, chain, 0);
}

Result<Source> loadStored(std::string_view stored, const std::filesystem::path& hostFile) {
    const std::filesystem::path file = resolvePath(stored, hostFile);
    if (underlay::isPdfPath(stored)) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec))
            return makeError(ErrorCode::NotFound, "Cannot find the file " + file.string());
        return loadPdf(file, underlay::parseSpec(stored).pdf);
    }
    if (underlay::isImagePath(stored)) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec))
            return makeError(ErrorCode::NotFound, "Cannot find the file " + file.string());
        return loadImageFrame(file);
    }
    return loadSource(file);
}

bool isUnderlayPath(std::string_view stored) {
    return underlay::isPdfPath(stored) || underlay::isImagePath(stored);
}

bool isUnderlay(const Document& doc, BlockId block) {
    const BlockDef* def = doc.block(block);
    return def && !def->xrefPath.empty() && isUnderlayPath(def->xrefPath);
}

Result<BlockId> attach(Transaction& tx, Document& doc, std::string_view name,
                       std::string_view stored, const Source& source, Summary* summary,
                       bool overlay) {
    if (!source.doc)
        return makeError(ErrorCode::InvalidArgument, "There is nothing to attach");
    if (auto ok = blocks::validateBlockName(doc, name); !ok)
        return ok.error();
    BlockDef row;
    row.name = std::string(name);
    row.xrefPath = std::string(stored);
    row.xrefStamp = source.stamp;
    row.xrefOverlay = overlay;
    row.base = insertionBase(*source.doc);
    auto block = tx.addBlock(std::move(row));
    if (!block)
        return block.error();
    Summary local;
    Filler fill(tx, doc, *source.doc, std::string(name), summary ? *summary : local);
    if (auto r = fill.run(block.value()); !r)
        return r.error();
    return block.value();
}

Vec2 insertionBase(const Document& doc) {
    const auto it = doc.header().custom.find("tbcad.insbase");
    if (it == doc.header().custom.end())
        return {};
    const std::size_t comma = it->second.find(',');
    if (comma == std::string::npos)
        return {};
    try {
        return {std::stod(it->second.substr(0, comma)), std::stod(it->second.substr(comma + 1))};
    } catch (...) {
        return {};
    }
}

std::string insertionBaseText(Vec2 base) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%.9g,%.9g", base.x, base.y);
    return buf;
}

Result<BlockId> importAsBlock(Transaction& tx, Document& doc, std::string_view name,
                              const Source& source, Summary* summary) {
    if (!source.doc)
        return makeError(ErrorCode::InvalidArgument, "There is nothing to import");
    if (auto ok = blocks::validateBlockName(doc, name); !ok)
        return ok.error();
    BlockDef row;
    row.name = std::string(name);
    row.base = insertionBase(*source.doc);
    auto block = tx.addBlock(std::move(row));
    if (!block)
        return block.error();
    Summary local;
    Filler fill(tx, doc, *source.doc, std::string(name), summary ? *summary : local, true);
    if (auto r = fill.run(block.value()); !r)
        return r.error();
    return block.value();
}

Result<void> reload(Transaction& tx, Document& doc, BlockId block, const Source& source,
                    Summary* summary) {
    const BlockDef* def = doc.block(block);
    if (!def || def->xrefPath.empty())
        return makeError(ErrorCode::InvalidArgument, "That block is not an external reference");
    if (!source.doc)
        return makeError(ErrorCode::InvalidArgument, "There is nothing to reload");
    const std::string name = def->name;
    std::set<BlockId> nested = nestedBlocks(doc, block);
    eraseContent(tx, doc, block);
    eraseBlocks(tx, std::move(nested));
    (void)tx.modifyBlock(block, [&](BlockDef& d) {
        d.xrefStamp = source.stamp;
        d.base = insertionBase(*source.doc);
    });
    Summary local;
    Filler fill(tx, doc, *source.doc, name, summary ? *summary : local);
    return fill.run(block);
}

Result<void> detach(Transaction& tx, Document& doc, BlockId block) {
    const BlockDef* def = doc.block(block);
    if (!def || def->xrefPath.empty())
        return makeError(ErrorCode::InvalidArgument, "That block is not an external reference");
    const std::string name = def->name;
    std::set<BlockId> nested = nestedBlocks(doc, block);
    // The inserts go first: model space and other blocks.
    std::vector<EntityId> inserts;
    doc.forEachEntity([&](const Entity& e) {
        if (const auto* ins = entityAs<InsertData>(e); ins && ins->block == block)
            inserts.push_back(e.common.id);
    });
    for (EntityId id : inserts)
        (void)tx.erase(id);
    eraseContent(tx, doc, block);
    eraseBlocks(tx, std::move(nested));
    if (auto r = tx.eraseBlock(block); !r)
        return r;
    if (const Layer* root = doc.layerByName(name))
        eraseLayerTree(tx, doc, root->id);
    return {};
}

Result<void> unload(Transaction& tx, Document& doc, BlockId block) {
    const BlockDef* def = doc.block(block);
    if (!def || def->xrefPath.empty())
        return makeError(ErrorCode::InvalidArgument, "That block is not an external reference");
    std::set<BlockId> nested = nestedBlocks(doc, block);
    eraseContent(tx, doc, block);
    eraseBlocks(tx, std::move(nested));
    return tx.modifyBlock(block, [](BlockDef& d) { d.xrefStamp = 0; });
}

namespace {

// BIND "Insert": the xref's layer tree (below the top-level layer named like the xref) is merged
// into the host's own tree by name; entities, in the model and in blocks, follow.
Result<void> mergeLayersIntoHost(Transaction& tx, Document& doc, const std::string& name) {
    const Layer* rootRow = doc.layerByName(name);
    if (!rootRow)
        return {};
    const LayerId root = rootRow->id;
    std::vector<LayerId> order{root};  // breadth first: parents before children
    for (std::size_t i = 0; i < order.size(); ++i) {
        std::vector<std::pair<std::string, LayerId>> kids;
        for (const auto& [id, l] : doc.layers())
            if (l.parent == order[i])
                kids.emplace_back(l.name, id);
        std::sort(kids.begin(), kids.end());
        for (auto& k : kids)
            order.push_back(k.second);
    }
    std::map<LayerId, LayerId> to;  // xref layer -> host layer
    for (std::size_t i = 1; i < order.size(); ++i) {
        const LayerId id = order[i];
        const Layer* l = doc.layer(id);
        const LayerId targetParent = l->parent == root ? LayerId{} : to[l->parent];
        if (l->parent == root && l->name == "0") {
            to[id] = kLayerZero;
            continue;
        }
        if (const Layer* same = doc.layerByName(targetParent, l->name); same && same->id != root) {
            to[id] = same->id;
            continue;
        }
        // The copy keeps the look the layer had, also where it inherited it from its parent.
        EffectiveLayer eff = doc.effective(id);
        Layer row = *l;
        row.id = {};
        row.parent = targetParent;
        row.color = eff.color;
        row.linetype = eff.linetype;
        row.lineweight = eff.lineweight;
        auto r = tx.addLayer(std::move(row));
        if (!r)
            return r.error();
        to[id] = r.value();
    }
    std::vector<EntityId> users;
    doc.forEachEntity([&](const Entity& e) {
        if (to.count(e.common.layer))
            users.push_back(e.common.id);
    });
    for (EntityId id : users) {
        LayerId from = doc.find(id)->common.layer;
        LayerId target = to[from];
        if (auto r = tx.modify(id, [&](Entity& e) { e.common.layer = target; }); !r)
            return r.error();
    }
    // Remove the emptied xref layers, leaves first. A layer something still uses stays.
    std::set<LayerId> used;
    doc.forEachEntity([&](const Entity& e) { used.insert(e.common.layer); });
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (!used.count(*it) && *it != doc.header().currentLayer)
            (void)tx.eraseLayer(*it);
    return {};
}

}  // namespace

Result<void> bind(Transaction& tx, Document& doc, BlockId block, BindMode mode) {
    const BlockDef* def = doc.block(block);
    if (!def || def->xrefPath.empty())
        return makeError(ErrorCode::InvalidArgument, "That block is not an external reference");
    const std::string name = def->name;
    if (auto r = tx.modifyBlock(block,
                                [](BlockDef& d) {
                                    d.xrefPath.clear();
                                    d.xrefStamp = 0;
                                    d.xrefOverlay = false;
                                });
        !r || mode == BindMode::Bind)
        return r;
    if (auto r = mergeLayersIntoHost(tx, doc, name); !r)
        return r;
    // Nested blocks lose the "<xref>$" prefix when the plain name is free.
    const std::string prefix = name + "$";
    std::vector<std::pair<BlockId, std::string>> renames;
    for (const auto& [id, b] : doc.blocks()) {
        if (b.name.rfind(prefix, 0) != 0 || b.name.size() == prefix.size())
            continue;
        std::string plain = b.name.substr(prefix.size());
        if (!doc.blockByName(plain))
            renames.emplace_back(id, plain);
    }
    for (auto& [id, plain] : renames)
        if (!doc.blockByName(plain))
            (void)tx.modifyBlock(id, [&](BlockDef& d) { d.name = plain; });
    return {};
}

Result<void> changePath(Transaction& tx, Document& doc, BlockId block, std::string_view stored) {
    const BlockDef* def = doc.block(block);
    if (!def || def->xrefPath.empty())
        return makeError(ErrorCode::InvalidArgument, "That block is not an external reference");
    if (stored.empty())
        return makeError(ErrorCode::InvalidArgument, "The path is empty");
    std::string text(stored);
    if (text.find('#') == std::string::npos) {
        auto hash = def->xrefPath.find('#');
        if (hash != std::string::npos)
            text += def->xrefPath.substr(hash);
    }
    return tx.modifyBlock(block, [&](BlockDef& d) { d.xrefPath = text; });
}

namespace {

std::string layerPathOf(const Document& doc, LayerId id) {
    std::vector<std::string> parts;
    for (int guard = 0; id && guard < 16; ++guard) {
        const Layer* l = doc.layer(id);
        if (!l)
            break;
        parts.push_back(l->name);
        id = l->parent;
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        out += (out.empty() ? "" : "~") + *it;
    return out;
}

// The layer with this parent~child path, or null.
const Layer* layerByPath(const Document& doc, const std::string& path) {
    for (const auto& [id, l] : doc.layers())
        if (layerPathOf(doc, id) == path)
            return &l;
    return nullptr;
}

}  // namespace

std::vector<ContentItem> listContent(const Document& src) {
    std::vector<ContentItem> out;
    auto sorted = [&](ContentKind kind, std::vector<std::string> names) {
        std::sort(names.begin(), names.end(),
                  [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
        for (std::string& n : names)
            out.push_back({kind, std::move(n)});
    };
    std::vector<std::string> names;
    for (const auto& [id, b] : src.blocks())
        if (!b.anonymous && b.xrefPath.empty() && !b.name.empty() && b.name.front() != '*')
            names.push_back(b.name);
    sorted(ContentKind::Block, std::move(names));
    names.clear();
    for (const auto& [id, l] : src.layers())
        if (id != kLayerZero)
            names.push_back(layerPathOf(src, id));
    sorted(ContentKind::Layer, std::move(names));
    names.clear();
    for (const auto& [id, lt] : src.linetypes())
        if (id != kLinetypeContinuous)
            names.push_back(lt.name);
    sorted(ContentKind::Linetype, std::move(names));
    names.clear();
    for (const auto& [id, ts] : src.textStyles())
        if (id != kTextStyleStandard)
            names.push_back(ts.name);
    sorted(ContentKind::TextStyle, std::move(names));
    names.clear();
    for (const auto& [id, ds] : src.dimStyles())
        if (id != kDimStyleStandard)
            names.push_back(ds.name);
    sorted(ContentKind::DimStyle, std::move(names));
    return out;
}

Result<void> importContent(Transaction& tx, Document& doc, const Source& source,
                           const std::vector<ContentItem>& items, ImportReport& report) {
    if (!source.doc)
        return makeError(ErrorCode::InvalidArgument, "There is nothing to import from");
    const Document& src = *source.doc;
    Summary summary;
    Filler fill(tx, doc, src, "content", summary, true);
    for (const ContentItem& item : items) {
        auto skip = [&](const std::string& why) {
            report.skipped.push_back(item.name + ": " + why);
        };
        switch (item.kind) {
            case ContentKind::Block: {
                const BlockDef* sb = src.blockByName(item.name);
                if (!sb) {
                    skip("not in the file");
                    break;
                }
                if (auto ok = blocks::validateBlockName(doc, item.name); !ok) {
                    skip(ok.error().message);
                    break;
                }
                BlockDef row;
                row.name = sb->name;
                row.base = sb->base;
                auto block = tx.addBlock(std::move(row));
                if (!block)
                    return block.error();
                if (auto r = fill.runBlock(*sb, block.value()); !r)
                    return r;
                ++report.added;
                break;
            }
            case ContentKind::Layer: {
                const Layer* sl = layerByPath(src, item.name);
                if (!sl) {
                    skip("not in the file");
                } else if (layerByPath(doc, item.name)) {
                    skip("already in the drawing");
                } else {
                    const std::size_t before = summary.layers;
                    (void)fill.importLayer(sl->id);
                    report.added += summary.layers - before;
                }
                break;
            }
            case ContentKind::Linetype: {
                const Linetype* sl = src.linetypeByName(item.name);
                if (!sl) {
                    skip("not in the file");
                } else if (doc.linetypeByName(item.name)) {
                    skip("already in the drawing");
                } else {
                    (void)fill.importLinetype(sl->id);
                    ++report.added;
                }
                break;
            }
            case ContentKind::TextStyle: {
                const TextStyle* sts = src.textStyleByName(item.name);
                if (!sts) {
                    skip("not in the file");
                } else if (doc.textStyleByName(item.name)) {
                    skip("already in the drawing");
                } else {
                    (void)fill.importTextStyle(sts->id);
                    ++report.added;
                }
                break;
            }
            case ContentKind::DimStyle: {
                const DimStyle* sds = src.dimStyleByName(item.name);
                if (!sds) {
                    skip("not in the file");
                } else if (doc.dimStyleByName(item.name)) {
                    skip("already in the drawing");
                } else {
                    (void)fill.importDimStyle(sds->id);
                    ++report.added;
                }
                break;
            }
        }
    }
    return {};
}

State state(const Document& doc, BlockId block, const std::filesystem::path& hostFile) {
    const BlockDef* def = doc.block(block);
    if (!def || def->xrefPath.empty())
        return State::NotXref;
    if (def->xrefStamp == 0)
        return State::Unloaded;
    std::filesystem::path p = resolvePath(def->xrefPath, hostFile);
    std::error_code ec;
    if (!rootReachable(p) || !std::filesystem::is_regular_file(p, ec))
        return State::Missing;
    return fileStamp(p) == def->xrefStamp ? State::UpToDate : State::Changed;
}

OpenLoad loadOnOpen(Document& doc, const std::filesystem::path& hostFile) {
    OpenLoad out;
    std::vector<BlockId> todo;
    for (BlockId id : xrefs(doc)) {
        const BlockDef* def = doc.block(id);
        if (def->xrefStamp == 0 && def->entities.empty() && !isUnderlayPath(def->xrefPath))
            todo.push_back(id);
    }
    if (todo.empty())
        return out;
    std::error_code ec;
    std::set<std::filesystem::path> chain;
    SourceCache cache;
    const std::filesystem::path self = std::filesystem::weakly_canonical(hostFile, ec);
    chain.insert(ec ? hostFile : self);
    Transaction tx = doc.begin("load external references");
    for (BlockId id : todo) {
        const BlockDef* def = doc.block(id);
        const std::string name = def->name;
        const std::filesystem::path file = resolvePath(def->xrefPath, hostFile);
        if (!rootReachable(file) || !std::filesystem::is_regular_file(file, ec)) {
            out.missing.push_back(name);
            continue;
        }
        const std::filesystem::path key = std::filesystem::weakly_canonical(file, ec);
        if (chain.count(ec ? file : key)) {
            out.failed.push_back(name + ": refers back to this drawing");
            continue;
        }
        auto source = loadCached(file, chain, 1, &cache);
        if (!*source) {
            out.failed.push_back(name + ": " + source->error().message);
            continue;
        }
        if (auto r = reload(tx, doc, id, source->value()); !r) {
            out.failed.push_back(name + ": " + r.error().message);
            continue;
        }
        out.loaded.push_back(name);
    }
    tx.commit();
    return out;
}

std::vector<BlockId> xrefs(const Document& doc) {
    std::vector<BlockId> out;
    for (const auto& [id, b] : doc.blocks())
        if (!b.xrefPath.empty())
            out.push_back(id);
    return out;
}

std::size_t useCount(const Document& doc, BlockId block) {
    std::size_t n = 0;
    doc.forEachEntity([&](const Entity& e) {
        if (const auto* ins = entityAs<InsertData>(e); ins && ins->block == block)
            ++n;
    });
    return n;
}

}  // namespace tbcad::xref
