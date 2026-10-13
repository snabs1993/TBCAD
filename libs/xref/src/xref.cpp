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

// Copies the model space and the blocks it uses of `src` into the host document.
class Filler {
public:
    // `merge`: the source's layers are matched by name with the host's own (a file used as a
    // block) instead of being hung below a layer named like the xref.
    Filler(Transaction& tx, Document& host, const Document& src, std::string name,
           Summary& summary, bool merge = false)
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
            if (!e)
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

Result<Source> loadSource(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec))
        return makeError(ErrorCode::NotFound, "Cannot find the file " + file.string());
    const std::string ext = lower(file.extension().string());
    Source out;
    out.stamp = fileStamp(file);
    if (ext == ".tbcad") {
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
    return makeError(ErrorCode::Unsupported, "Only .tbcad and .dxf files can be attached");
}

Result<BlockId> attach(Transaction& tx, Document& doc, std::string_view name,
                       std::string_view stored, const Source& source, Summary* summary) {
    if (!source.doc)
        return makeError(ErrorCode::InvalidArgument, "There is nothing to attach");
    if (auto ok = blocks::validateBlockName(doc, name); !ok)
        return ok.error();
    BlockDef row;
    row.name = std::string(name);
    row.xrefPath = std::string(stored);
    row.xrefStamp = source.stamp;
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

Result<void> bind(Transaction& tx, Document& doc, BlockId block) {
    const BlockDef* def = doc.block(block);
    if (!def || def->xrefPath.empty())
        return makeError(ErrorCode::InvalidArgument, "That block is not an external reference");
    (void)doc;
    return tx.modifyBlock(block, [](BlockDef& d) {
        d.xrefPath.clear();
        d.xrefStamp = 0;
    });
}

State state(const Document& doc, BlockId block, const std::filesystem::path& hostFile) {
    const BlockDef* def = doc.block(block);
    if (!def || def->xrefPath.empty())
        return State::NotXref;
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
