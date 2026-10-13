#include <algorithm>
#include <cmath>

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
    headInt(body, st, "currentColor",
            (static_cast<std::int64_t>(h.currentColor.kind) << 24) | (h.currentColor.r << 16) |
                (h.currentColor.g << 8) | h.currentColor.b),
        bump();
    headInt(body, st, "currentLinetype", h.currentLinetype.value), bump();
    headInt(body, st, "currentLineweight", h.currentLineweight.v), bump();
    headInt(body, st, "currentTextStyle", h.currentTextStyle.value), bump();
    headInt(body, st, "currentDimStyle", h.currentDimStyle.value), bump();
    headString(body, st, "createdBy", h.createdBy), bump();
    for (const auto& [k, v] : h.custom) {
        if (k.starts_with(kOpaqueChunkPrefix))
            continue;
        headString(body, st, std::string(kCustomPrefix) + k, v), bump();
    }
    return std::move(body.bytes());
}

}  // namespace

Result<std::vector<std::uint8_t>> writeTbcadToMemory(const Document& doc,
                                                     const WriteOptions& options) {
    try {
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
            layr.u8(static_cast<std::uint8_t>((r.on ? 1 : 0) | (r.frozen ? 2 : 0) |
                                              (r.locked ? 4 : 0) | (r.plot ? 8 : 0)));
        }

        ByteWriter blks;
        blks.varint(doc.blocks().size());
        for (const auto& [id, r] : doc.blocks()) {
            blks.varint(id.value);
            blks.varint(st.intern(r.name));
            blks.vec2(r.base);
            blks.u8(r.anonymous ? 1 : 0);
        }

        // Entities in id order, split over ENTS chunks of about 8 MiB.
        std::vector<const Entity*> ents;
        ents.reserve(doc.entityCount());
        doc.forEachEntity([&](const Entity& e) { ents.push_back(&e); });
        std::sort(ents.begin(), ents.end(),
                  [](const Entity* a, const Entity* b) { return a->common.id < b->common.id; });
        std::vector<std::vector<std::uint8_t>> entChunks;
        {
            std::size_t inChunk = 0;
            ByteWriter body;
            auto flush = [&] {
                if (inChunk == 0)
                    return;
                ByteWriter c;
                c.varint(inChunk);
                c.raw(body.bytes());
                entChunks.push_back(std::move(c.bytes()));
                body = ByteWriter{};
                inChunk = 0;
            };
            for (const Entity* e : ents) {
                encodeEntity(body, st, *e, doc.ownerBlock(e->common.id));
                ++inChunk;
                if (body.size() >= kEntsChunkTarget)
                    flush();
            }
            flush();
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

        ByteWriter out;
        appendFileHeader(out);
        {
            ByteWriter head;
            head.varint(headCount);
            head.raw(headBody);
            appendChunk(out, kHead, head.bytes(), options.compress);
        }
        appendChunk(out, kStrs, strs.bytes(), options.compress);
        appendChunk(out, kLtyp, ltyp.bytes(), options.compress);
        appendChunk(out, kTsty, tsty.bytes(), options.compress);
        appendChunk(out, kDsty, dsty.bytes(), options.compress);
        appendChunk(out, kLayr, layr.bytes(), options.compress);
        appendChunk(out, kBlks, blks.bytes(), options.compress);
        for (const auto& c : entChunks)
            appendChunk(out, kEnts, c, options.compress);
        if (ordr.size() != 0)
            appendChunk(out, kOrdr, ordr.bytes(), options.compress);
        for (const auto& [key, value] : opaque) {
            ByteReader r(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(value->data()), value->size()));
            std::uint32_t type = r.u32();
            std::uint32_t flags = r.u32();
            appendStoredChunk(out, type, flags, r.rest());
        }
        appendStoredChunk(out, kEnd, 0, {});
        return std::move(out.bytes());
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::Generic, std::string("cannot write .tbcad: ") + ex.what());
    }
}

}  // namespace tbcad::io
