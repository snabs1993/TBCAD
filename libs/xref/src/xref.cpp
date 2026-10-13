#include "tbcad/xref/xref.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>

#include "tbcad/blocks/blocks.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/io/tbcad.h"

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
        else if (auto* ins = entityAs<InsertData>(e))
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
    std::vector<EntityId> members = def->entities;
    for (EntityId id : members)
        (void)tx.erase(id);
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

std::filesystem::path resolvePath(std::string_view stored, const std::filesystem::path& hostFile) {
    std::filesystem::path p{std::u8string(stored.begin(), stored.end())};
    if (p.is_absolute() || hostFile.empty())
        return p;
    return hostFile.parent_path() / p;
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

std::map<std::string, Loader>& loaders() {
    static std::map<std::string, Loader> table;
    return table;
}

Result<Source> loadOne(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec))
        return makeError(ErrorCode::NotFound, "Cannot find the file " + file.string());
    const std::string ext = lower(file.extension().string());
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
        std::ifstream in(file, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(raw.size());
        std::transform(raw.begin(), raw.end(), bytes.begin(),
                       [](char c) { return static_cast<std::byte>(c); });
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
    return makeError(ErrorCode::Unsupported, "Only .tbcad, .tbt and .dxf files can be attached");
}

// Reads `file` and refreshes the xrefs inside it from their own files. `chain` holds the files
// being read up the call chain, so a loop of xrefs is cut where it closes.
Result<Source> loadChain(const std::filesystem::path& file, std::set<std::filesystem::path>& chain,
                         int depth) {
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
            auto sub = loadChain(p, chain, depth + 1);
            if (sub)
                (void)reload(tx, doc, id, sub.value());
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
    auto block = tx.addBlock(std::move(row));
    if (!block)
        return block.error();
    Summary local;
    Filler fill(tx, doc, *source.doc, std::string(name), summary ? *summary : local);
    if (auto r = fill.run(block.value()); !r)
        return r.error();
    return block.value();
}

Result<BlockId> importAsBlock(Transaction& tx, Document& doc, std::string_view name,
                              const Source& source, Summary* summary) {
    if (!source.doc)
        return makeError(ErrorCode::InvalidArgument, "There is nothing to import");
    if (auto ok = blocks::validateBlockName(doc, name); !ok)
        return ok.error();
    BlockDef row;
    row.name = std::string(name);
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
    (void)tx.modifyBlock(block, [&](BlockDef& d) { d.xrefStamp = source.stamp; });
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

Result<void> bind(Transaction& tx, Document& doc, BlockId block) {
    const BlockDef* def = doc.block(block);
    if (!def || def->xrefPath.empty())
        return makeError(ErrorCode::InvalidArgument, "That block is not an external reference");
    (void)doc;
    return tx.modifyBlock(block, [](BlockDef& d) {
        d.xrefPath.clear();
        d.xrefStamp = 0;
        d.xrefOverlay = false;
    });
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
    if (!std::filesystem::is_regular_file(p, ec))
        return State::Missing;
    return fileStamp(p) == def->xrefStamp ? State::UpToDate : State::Changed;
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
