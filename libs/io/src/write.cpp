#include <algorithm>
#include <cmath>
#include <future>
#include <thread>

#include "chunks.h"
#include "tbcad/io/tbcad.h"

namespace tbcad::io {

using namespace detail;

namespace {

enum HeadType : std::uint8_t { kHeadString = 0, kHeadInt = 1, kHeadReal = 2 };

void headString(ByteWriter& w, StringTable& st, std::string_view key, const std::string& v) {
    w.varint(st.intern(key));
    w.u8(kHeadString);
    w.varint(st.intern(v));
}
void headInt(ByteWriter& w, StringTable& st, std::string_view key, std::int64_t v) {
    w.varint(st.intern(key));
    w.u8(kHeadInt);
    w.i64(v);
}
void headReal(ByteWriter& w, StringTable& st, std::string_view key, double v) {
    w.varint(st.intern(key));
    w.u8(kHeadReal);
    w.f64(v);
}

void writeColor(ByteWriter& w, const Color& c) {
    w.u8(static_cast<std::uint8_t>(c.kind));
    w.u8(c.r);
    w.u8(c.g);
    w.u8(c.b);
}

std::vector<std::uint8_t> buildHead(const Document& doc, StringTable& st, std::uint32_t& count) {
    const Header& h = doc.header();
    ByteWriter body;
    count = 0;
    auto bump = [&] { ++count; };
    headInt(body, st, "units", static_cast<std::int64_t>(h.units)), bump();
    headReal(body, st, "workArea", h.workArea), bump();
    headReal(body, st, "ltScale", h.ltScale), bump();
    headInt(body, st, "currentLayer", h.currentLayer.value), bump();
    headInt(body, st, "currentLevel", h.currentLevel.value), bump();
    headInt(body, st, "levelView", static_cast<std::int64_t>(h.levelView)), bump();
    headInt(body, st, "currentColor",
            (static_cast<std::int64_t>(h.currentColor.kind) << 24) | (h.currentColor.r << 16) |
                (h.currentColor.g << 8) | h.currentColor.b),
        bump();
    headInt(body, st, "currentLinetype", h.currentLinetype.value), bump();
    headInt(body, st, "currentLineweight", h.currentLineweight.v), bump();
    if (h.currentTransparency != kTransparencyByLayer)
        headInt(body, st, "currentTransparency", h.currentTransparency), bump();
    headInt(body, st, "currentTextStyle", h.currentTextStyle.value), bump();
    headInt(body, st, "currentDimStyle", h.currentDimStyle.value), bump();
    headString(body, st, "createdBy", h.createdBy), bump();
    const NextIds next = doc.nextIds();
    headInt(body, st, "nextEntity", static_cast<std::int64_t>(next.entity)), bump();
    headInt(body, st, "nextLayer", next.layer), bump();
    headInt(body, st, "nextLevel", next.level), bump();
    headInt(body, st, "nextLinetype", next.linetype), bump();
    headInt(body, st, "nextTextStyle", next.textStyle), bump();
    headInt(body, st, "nextDimStyle", next.dimStyle), bump();
    headInt(body, st, "nextBlock", next.block), bump();
    for (const auto& [k, v] : h.custom) {
        if (k.starts_with(kOpaqueChunkPrefix))
            continue;
        headString(body, st, std::string(kCustomPrefix) + k, v), bump();
    }
    return std::move(body.bytes());
}

std::uint32_t fourcc4(const std::string& s) {
    char t[5] = {s[0], s[1], s[2], s[3], 0};
    return fourcc(t);
}

}  // namespace

namespace {

// A chunk of the file in the order it will be written: still to be deflated (Raw), being
// deflated on another thread (Prepared) or already in its stored form (Stored).
struct FileItem {
    enum class Kind { Raw, Prepared, Stored } kind = Kind::Raw;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;  // Stored only
    std::vector<std::uint8_t> bytes{};
    std::future<PreparedChunk> prepared{};
};

// The whole document encoded, holding no reference to it any more: the file is finished from this
// by assembleFile, possibly on another thread while the document goes on changing.
struct EncodedFile {
    std::vector<FileItem> items{};
    bool compress = true;
};

Result<EncodedFile> encodeDocument(const Document& doc, const WriteOptions& options) {
    try {
        EncodedFile file;
        file.compress = options.compress;
        std::vector<FileItem>& items = file.items;
        auto addRaw = [&](std::uint32_t type, std::vector<std::uint8_t> bytes) {
            FileItem it;
            it.type = type;
            it.bytes = std::move(bytes);
            items.push_back(std::move(it));
        };
        StringTable st;
        // Every chunk body is built first because STRS, which comes first in the file, can only
        // be completed once all other chunks have interned their strings.
        std::uint32_t headCount = 0;
        std::vector<std::uint8_t> headBody = buildHead(doc, st, headCount);

        ByteWriter ltyp;
        ltyp.varint(doc.linetypes().size());
        for (const auto& [id, r] : doc.linetypes()) {
            ltyp.varint(id.value);
            ltyp.varint(st.intern(r.name));
            ltyp.varint(st.intern(r.description));
            ltyp.varint(r.pattern.size());
            for (double d : r.pattern)
                ltyp.f64(d);
        }

        ByteWriter ltmk;
        std::size_t ltmkCount = 0;
        for (const auto& [id, r] : doc.linetypes()) {
            if (r.marks.empty())
                continue;
            ltmk.varint(id.value);
            ltmk.varint(r.marks.size());
            for (const LinetypeMark& m : r.marks) {
                ltmk.u8(static_cast<std::uint8_t>(m.kind));
                ltmk.u8(static_cast<std::uint8_t>(m.rotationKind));
                ltmk.f64(m.offset);
                ltmk.varint(st.intern(m.text));
                ltmk.varint(st.intern(m.style));
                ltmk.f64(m.scale);
                ltmk.f64(m.rotation);
                ltmk.f64(m.shift.x);
                ltmk.f64(m.shift.y);
            }
            ++ltmkCount;
        }

        ByteWriter tstx;
        std::size_t tstxCount = 0;
        for (const auto& [id, r] : doc.textStyles()) {
            if (r.shxFile.empty() && r.bigFontFile.empty() && !r.annotative)
                continue;
            tstx.varint(id.value);
            tstx.u8(r.annotative ? 1 : 0);
            tstx.varint(st.intern(r.shxFile));
            tstx.varint(st.intern(r.bigFontFile));
            ++tstxCount;
        }

        ByteWriter tsty;
        tsty.varint(doc.textStyles().size());
        for (const auto& [id, r] : doc.textStyles()) {
            tsty.varint(id.value);
            tsty.varint(st.intern(r.name));
            tsty.varint(st.intern(r.fontFamily));
            tsty.varint(st.intern(r.fontFile));
            tsty.f64(r.widthFactor);
            tsty.f64(r.oblique);
        }

        ByteWriter tsth;
        std::size_t tsthCount = 0;
        for (const auto& [id, r] : doc.textStyles())
            if (r.height > 0) {
                tsth.varint(id.value);
                tsth.f64(r.height);
                ++tsthCount;
            }

        ByteWriter dsty;
        dsty.varint(doc.dimStyles().size());
        for (const auto& [id, r] : doc.dimStyles()) {
            dsty.varint(id.value);
            dsty.varint(st.intern(r.name));
            dsty.f64(r.textHeight);
            dsty.varint(r.textStyle.value);
            dsty.f64(r.textGap);
            dsty.u8(static_cast<std::uint8_t>(r.arrowType));
            dsty.f64(r.arrowSize);
            dsty.f64(r.extLineOffset);
            dsty.f64(r.extLineExtension);
            dsty.f64(r.dimLineExtension);
            dsty.u8(static_cast<std::uint8_t>(r.textPlacement));
            dsty.u8(static_cast<std::uint8_t>(r.textAlign));
            dsty.u8(static_cast<std::uint8_t>(r.units));
            dsty.i32(r.precision);
            dsty.varint(st.intern(r.prefix));
            dsty.varint(st.intern(r.suffix));
            dsty.f64(r.roundOff);
            dsty.f64(r.overallScale);
            dsty.u8(static_cast<std::uint8_t>((r.suppressLeadingZeros ? 1 : 0) |
                                              (r.suppressTrailingZeros ? 2 : 0)));
            writeColor(dsty, r.dimLineColor);
            writeColor(dsty, r.extLineColor);
            writeColor(dsty, r.textColor);
        }

        // Alternate units and tolerances: a separate chunk, only for the styles that use them, so
        // that older programs still read the file.
        ByteWriter dstx;
        std::size_t dstxCount = 0;
        for (const auto& [id, r] : doc.dimStyles()) {
            if (!r.altUnits && r.altScale == 1 && r.altUnit == LengthUnit::Inch &&
                r.altPrecision == 2 && r.altPrefix.empty() && r.altSuffix.empty() &&
                r.tolerance == DimTolerance::None && r.tolUpper == 0 && r.tolLower == 0 &&
                r.tolPrecision == 2)
                continue;
            dstx.varint(id.value);
            dstx.u8(r.altUnits ? 1 : 0);
            dstx.f64(r.altScale);
            dstx.u8(static_cast<std::uint8_t>(r.altUnit));
            dstx.i32(r.altPrecision);
            dstx.varint(st.intern(r.altPrefix));
            dstx.varint(st.intern(r.altSuffix));
            dstx.u8(static_cast<std::uint8_t>(r.tolerance));
            dstx.f64(r.tolUpper);
            dstx.f64(r.tolLower);
            dstx.i32(r.tolPrecision);
            ++dstxCount;
        }

        // Layers carry their parent id; ByParent colour / linetype / lineweight are stored as the
        // special values of those fields (colour kind 4, linetype 0xFFFFFFFD, lineweight -4).
        ByteWriter layr;
        layr.varint(doc.layers().size());
        for (const auto& [id, r] : doc.layers()) {
            layr.varint(id.value);
            layr.varint(st.intern(r.name));
            layr.varint(r.parent.value);
            writeColor(layr, r.color);
            layr.u32(r.linetype.value);
            layr.i16(r.lineweight.v);
            layr.u8(static_cast<std::uint8_t>(
                (r.on ? 1 : 0) | (r.frozen ? 2 : 0) | (r.locked ? 4 : 0) | (r.plot ? 8 : 0) |
                (r.transparency ? 16 : 0) | (r.description.empty() ? 0 : 32)));
            if (r.transparency)
                layr.u8(r.transparency);
            if (!r.description.empty())
                layr.varint(st.intern(r.description));
        }

        // Levels (LVLS) are written only when the document has more than the pristine default
        // level; ELVL maps entities to their level, listing only those not on the default level.
        ByteWriter lvls;
        bool writeLvls = doc.levels().size() > 1;
        if (const Level* l1 = doc.level(kLevelDefault);
            l1 && (l1->name != "Level 1" || l1->elevation != 0 || !l1->visible || l1->locked)) {
            writeLvls = true;
        }
        lvls.varint(doc.levels().size());
        for (const auto& [id, r] : doc.levels()) {
            lvls.varint(id.value);
            lvls.varint(st.intern(r.name));
            lvls.f64(r.elevation);
            lvls.u8(static_cast<std::uint8_t>((r.visible ? 1 : 0) | (r.locked ? 2 : 0)));
        }

        ByteWriter blks;
        blks.varint(doc.blocks().size());
        for (const auto& [id, r] : doc.blocks()) {
            blks.varint(id.value);
            blks.varint(st.intern(r.name));
            blks.vec2(r.base);
            blks.u8(r.anonymous ? 1 : 0);
        }

        ByteWriter xrfs;
        std::size_t xrefCount = 0;
        for (const auto& [id, r] : doc.blocks())
            xrefCount += r.xrefPath.empty() ? 0 : 1;
        xrfs.varint(xrefCount);
        for (const auto& [id, r] : doc.blocks()) {
            if (r.xrefPath.empty())
                continue;
            xrfs.varint(id.value);
            xrfs.varint(st.intern(r.xrefPath));
            xrfs.i64(r.xrefStamp);
        }

        // Overlay flags of xrefs (since 1.5); only when at least one xref is an overlay.
        ByteWriter xrfo;
        std::size_t overlayCount = 0;
        for (const auto& [id, r] : doc.blocks())
            overlayCount += (!r.xrefPath.empty() && r.xrefOverlay) ? 1 : 0;
        xrfo.varint(overlayCount);
        for (const auto& [id, r] : doc.blocks())
            if (!r.xrefPath.empty() && r.xrefOverlay)
                xrfo.varint(id.value);

        // Dynamic block definitions (format 1.4).
        ByteWriter dynb;
        std::size_t dynCount = 0;
        for (const auto& [id, r] : doc.blocks())
            dynCount += r.dynamic ? 1 : 0;
        dynb.varint(dynCount);
        for (const auto& [id, r] : doc.blocks()) {
            if (!r.dynamic)
                continue;
            dynb.varint(id.value);
            dynb.varint(r.dynamic->params.size());
            for (const DynParam& p : r.dynamic->params) {
                dynb.u8(static_cast<std::uint8_t>(p.kind));
                dynb.varint(st.intern(p.name));
                dynb.vec2(p.base);
                dynb.vec2(p.end);
                dynb.f64(p.min);
                dynb.f64(p.max);
                dynb.varint(p.states.size());
                for (std::size_t i = 0; i < p.states.size(); ++i) {
                    dynb.varint(st.intern(p.states[i]));
                    const auto none = std::vector<EntityId>{};
                    const auto& objs = i < p.stateObjects.size() ? p.stateObjects[i] : none;
                    dynb.varint(objs.size());
                    for (EntityId e : objs)
                        dynb.varint(e.value);
                }
            }
            dynb.varint(r.dynamic->actions.size());
            for (const DynAction& a : r.dynamic->actions) {
                dynb.u8(static_cast<std::uint8_t>(a.kind));
                dynb.varint(static_cast<std::uint64_t>(std::max(a.param, 0)));
                dynb.vec2(a.frameMin);
                dynb.vec2(a.frameMax);
                dynb.varint(a.objects.size());
                for (EntityId e : a.objects)
                    dynb.varint(e.value);
            }
        }

        // Parametric assembly definitions (format 1.5).
        ByteWriter asmb;
        std::size_t asmCount = 0;
        for (const auto& [id, r] : doc.blocks())
            asmCount += r.assembly ? 1 : 0;
        asmb.varint(asmCount);
        auto putValue = [&](const AsmValue& v) {
            asmb.varint(st.intern(v.name));
            asmb.u8(v.isText ? 1 : 0);
            asmb.f64(v.number);
            asmb.varint(st.intern(v.text));
        };
        for (const auto& [id, r] : doc.blocks()) {
            if (!r.assembly)
                continue;
            asmb.varint(id.value);
            asmb.varint(r.assembly->params.size());
            for (const AsmParam& p : r.assembly->params) {
                asmb.u8(static_cast<std::uint8_t>(p.kind));
                asmb.varint(st.intern(p.name));
                asmb.f64(p.defaultNumber);
                asmb.varint(st.intern(p.defaultText));
                asmb.f64(p.min);
                asmb.f64(p.max);
                asmb.varint(p.choices.size());
                for (const std::string& c : p.choices)
                    asmb.varint(st.intern(c));
            }
            asmb.varint(r.assembly->bindings.size());
            for (const AsmBinding& b : r.assembly->bindings) {
                asmb.u8(static_cast<std::uint8_t>(b.kind));
                asmb.varint(b.entity.value);
                asmb.varint(st.intern(b.target));
                asmb.varint(st.intern(b.formula));
            }
            asmb.varint(r.assembly->presets.size());
            for (const AsmPreset& pr : r.assembly->presets) {
                asmb.varint(st.intern(pr.name));
                asmb.varint(pr.values.size());
                for (const AsmValue& v : pr.values)
                    putValue(v);
            }
            asmb.varint(st.intern(r.assembly->joinClass));
        }

        // Entities in id order, split over ENTS chunks of about 4 MiB.
        std::vector<const Entity*> ents;
        ents.reserve(doc.entityCount());
        doc.forEachEntity([&](const Entity& e) { ents.push_back(&e); });
        const auto byId = [](const Entity* a, const Entity* b) {
            return a->common.id < b->common.id;
        };
        if (!std::is_sorted(ents.begin(), ents.end(), byId))  // usually already in id order
            std::sort(ents.begin(), ents.end(), byId);
        // Each full chunk goes to a worker thread for deflating while the next one is being
        // encoded; at most as many as there are cores run at once.
        std::vector<FileItem> entItems;
        {
            const std::size_t maxRunning = std::max(1u, std::thread::hardware_concurrency());
            const bool compress = options.compress;
            std::size_t waited = 0;  // entItems before this index have been waited for
            std::size_t inChunk = 0;
            ByteWriter body;
            auto flush = [&] {
                if (inChunk == 0)
                    return;
                ByteWriter c;
                c.varint(inChunk);
                c.raw(body.bytes());
                body = ByteWriter{};
                inChunk = 0;
                // Too many running: wait for the oldest, so memory and threads stay bounded.
                if (entItems.size() - waited >= maxRunning)
                    entItems[waited++].prepared.wait();
                FileItem it;
                it.kind = FileItem::Kind::Prepared;
                it.type = kEnts;
                it.prepared = std::async(
                    std::launch::async,
                    [raw = std::move(c.bytes()), compress] { return prepareChunk(raw, compress); });
                entItems.push_back(std::move(it));
            };
            for (const Entity* e : ents) {
                encodeEntity(body, st, *e, doc.ownerBlock(e->common.id));
                ++inChunk;
                if (body.size() >= kEntsChunkTarget)
                    flush();
            }
            flush();
        }

        ByteWriter elvl;
        {
            std::size_t n = 0;
            for (const Entity* e : ents)
                n += e->common.level == kLevelDefault ? 0 : 1;
            if (n > 0) {
                elvl.varint(n);
                std::uint64_t prev = 0;
                for (const Entity* e : ents) {
                    if (e->common.level == kLevelDefault)
                        continue;
                    elvl.varint(e->common.id.value - prev);
                    prev = e->common.id.value;
                    elvl.varint(e->common.level.value);
                }
            }
        }

        // Draw order, only when it differs from creation order.
        ByteWriter ordr;
        {
            std::vector<EntityId> order = doc.modelSpace();
            if (!std::is_sorted(order.begin(), order.end())) {
                ordr.varint(order.size());
                for (EntityId id : order)
                    ordr.varint(id.value);
            }
        }

        // Opaque chunks of unknown type carried through from a file we read.
        std::vector<std::pair<std::string, const std::string*>> opaque;
        for (const auto& [k, v] : doc.header().custom) {
            if (k.starts_with(kOpaqueChunkPrefix))
                opaque.emplace_back(k, &v);
        }

        ByteWriter strs;
        strs.varint(st.all().size());
        for (const std::string& s : st.all()) {
            strs.varint(s.size());
            strs.raw(s.data(), s.size());
        }

        {
            ByteWriter head;
            head.varint(headCount);
            head.raw(headBody);
            addRaw(kHead, std::move(head.bytes()));
        }
        addRaw(kStrs, std::move(strs.bytes()));
        addRaw(kLtyp, std::move(ltyp.bytes()));
        addRaw(kTsty, std::move(tsty.bytes()));
        if (tstxCount > 0) {
            ByteWriter body;
            body.varint(tstxCount);
            body.raw(tstx.bytes());
            addRaw(kTstx, std::move(body.bytes()));
        }
        if (ltmkCount > 0) {
            ByteWriter body;
            body.varint(ltmkCount);
            body.raw(ltmk.bytes());
            addRaw(kLtmk, std::move(body.bytes()));
        }
        if (tsthCount > 0) {
            ByteWriter body;
            body.varint(tsthCount);
            body.raw(tsth.bytes());
            addRaw(kTsth, std::move(body.bytes()));
        }
        addRaw(kDsty, std::move(dsty.bytes()));
        if (dstxCount > 0) {
            ByteWriter body;
            body.varint(dstxCount);
            body.raw(dstx.bytes());
            addRaw(kDstx, std::move(body.bytes()));
        }
        addRaw(kLayr, std::move(layr.bytes()));
        if (writeLvls)
            addRaw(kLvls, std::move(lvls.bytes()));
        addRaw(kBlks, std::move(blks.bytes()));
        if (xrefCount > 0)
            addRaw(kXrfs, std::move(xrfs.bytes()));
        if (overlayCount > 0)
            addRaw(kXrfo, std::move(xrfo.bytes()));
        if (dynCount > 0)
            addRaw(kDynb, std::move(dynb.bytes()));
        if (asmCount > 0)
            addRaw(kAsmb, std::move(asmb.bytes()));
        for (FileItem& it : entItems)
            items.push_back(std::move(it));
        if (elvl.size() != 0)
            addRaw(kElvl, std::move(elvl.bytes()));
        if (ordr.size() != 0)
            addRaw(kOrdr, std::move(ordr.bytes()));
        for (const auto& [key, value] : opaque) {
            ByteReader r(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(value->data()), value->size()));
            std::uint32_t type = r.u32();
            std::uint32_t flags = r.u32();
            bool replaced = false;
            for (const ExtraChunk& x : options.extraChunks) {
                replaced = replaced || (x.type.size() == 4 && fourcc4(x.type) == type);
            }
            if (!replaced) {
                FileItem it;
                it.kind = FileItem::Kind::Stored;
                it.type = type;
                it.flags = flags;
                const auto rest = r.rest();
                it.bytes.assign(rest.begin(), rest.end());
                items.push_back(std::move(it));
            }
        }
        for (const ExtraChunk& x : options.extraChunks) {
            if (x.type.size() != 4)
                return makeError(ErrorCode::InvalidArgument, "chunk type must be four characters");
            addRaw(fourcc4(x.type), x.payload);
        }
        return file;
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::Generic, std::string("cannot write .tbcad: ") + ex.what());
    }
}

// Waits for the chunks being deflated and lays the file out in order.
Result<std::vector<std::uint8_t>> assembleFile(EncodedFile&& file) {
    try {
        ByteWriter out;
        appendFileHeader(out);
        for (FileItem& it : file.items) {
            switch (it.kind) {
                case FileItem::Kind::Raw:
                    appendChunk(out, it.type, it.bytes, file.compress);
                    break;
                case FileItem::Kind::Prepared:
                    appendPrepared(out, it.type, it.prepared.get());
                    break;
                case FileItem::Kind::Stored:
                    appendStoredChunk(out, it.type, it.flags, it.bytes);
                    break;
            }
        }
        appendStoredChunk(out, kEnd, 0, {});
        return std::move(out.bytes());
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::Generic, std::string("cannot write .tbcad: ") + ex.what());
    }
}

}  // namespace

Result<std::vector<std::uint8_t>> writeTbcadToMemory(const Document& doc,
                                                     const WriteOptions& options) {
    Result<EncodedFile> file = encodeDocument(doc, options);
    if (!file)
        return file.error();
    return assembleFile(std::move(file.value()));
}

std::future<Result<void>> writeTbcadAsync(const Document& doc, const std::filesystem::path& path,
                                          const WriteOptions& options) {
    Result<EncodedFile> file = encodeDocument(doc, options);
    if (!file) {
        std::promise<Result<void>> p;
        p.set_value(file.error());
        return p.get_future();
    }
    // From here the document is not touched again.
    return std::async(std::launch::async,
                      [f = std::make_shared<EncodedFile>(std::move(file.value())), path,
                       options]() mutable -> Result<void> {
                          Result<std::vector<std::uint8_t>> bytes = assembleFile(std::move(*f));
                          if (!bytes)
                              return bytes.error();
                          return saveTbcadBytes(bytes.value(), path, options);
                      });
}

}  // namespace tbcad::io

namespace tbcad::io {

namespace {
// The header key and payload of the opaque chunk `type`.
std::optional<std::pair<std::string, std::vector<std::uint8_t>>> locateOptionalChunk(
    const Document& doc, std::string_view type) {
    if (type.size() != 4)
        return std::nullopt;
    char t[5] = {type[0], type[1], type[2], type[3], 0};
    const std::uint32_t want = detail::fourcc(t);
    for (const auto& [key, value] : doc.header().custom) {
        if (!key.starts_with(detail::kOpaqueChunkPrefix) || value.size() < 8)
            continue;
        try {
            detail::ByteReader r(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
            detail::RawChunk c;
            c.type = r.u32();
            c.flags = r.u32();
            if (c.type != want)
                continue;
            c.stored = r.rest();
            std::vector<std::uint8_t> scratch;
            auto payload = detail::chunkPayload(c, scratch);
            return std::make_pair(key, std::vector<std::uint8_t>(payload.begin(), payload.end()));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}
}  // namespace

std::optional<std::vector<std::uint8_t>> findOptionalChunk(const Document& doc,
                                                           std::string_view type) {
    auto found = locateOptionalChunk(doc, type);
    if (!found)
        return std::nullopt;
    return std::move(found->second);
}

std::optional<std::vector<std::uint8_t>> takeOptionalChunk(Document& doc, std::string_view type) {
    auto found = locateOptionalChunk(doc, type);
    if (!found)
        return std::nullopt;
    Transaction tx = doc.begin("drop optional chunk");
    if (!tx.modifyHeader([&](Header& h) { h.custom.erase(found->first); }))
        return std::move(found->second);
    tx.commit();
    return std::move(found->second);
}

}  // namespace tbcad::io
