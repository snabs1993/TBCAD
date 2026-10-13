#include "tbcad/collab/versions.h"

#include <algorithm>

#include "bytes.h"

namespace tbcad::collab {

const char* spaceName(Space s) {
    switch (s) {
        case Space::Entity:
            return "entity";
        case Space::Layer:
            return "layer";
        case Space::Linetype:
            return "linetype";
        case Space::TextStyle:
            return "text style";
        case Space::DimStyle:
            return "dimension style";
        case Space::Block:
            return "block";
        case Space::Header:
            return "drawing settings";
        case Space::Order:
            return "draw order";
    }
    return "?";
}

std::vector<Key> keysOf(const io::Delta& d) {
    std::vector<Key> keys;
    keys.reserve(d.entities.size() + d.layers.size() + d.linetypes.size() + d.textStyles.size() +
                 d.dimStyles.size() + d.blocks.size() + d.order.size() + 1);
    for (const auto& e : d.entities)
        keys.push_back({Space::Entity, e.id.value});
    for (const auto& r : d.layers)
        keys.push_back({Space::Layer, r.id});
    for (const auto& r : d.linetypes)
        keys.push_back({Space::Linetype, r.id});
    for (const auto& r : d.textStyles)
        keys.push_back({Space::TextStyle, r.id});
    for (const auto& r : d.dimStyles)
        keys.push_back({Space::DimStyle, r.id});
    for (const auto& r : d.blocks)
        keys.push_back({Space::Block, r.id});
    if (d.header)
        keys.push_back({Space::Header, 0});
    for (const auto& o : d.order)
        keys.push_back({Space::Order, o.id.value});
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

bool exists(const Document& doc, const Key& k) {
    switch (k.space) {
        case Space::Entity:
            return doc.contains(EntityId{k.id});
        case Space::Layer:
            return k.id <= 0xFFFFFFFFu && doc.layer(LayerId{static_cast<std::uint32_t>(k.id)});
        case Space::Linetype:
            return k.id <= 0xFFFFFFFFu &&
                   doc.linetype(LinetypeId{static_cast<std::uint32_t>(k.id)});
        case Space::TextStyle:
            return k.id <= 0xFFFFFFFFu &&
                   doc.textStyle(TextStyleId{static_cast<std::uint32_t>(k.id)});
        case Space::DimStyle:
            return k.id <= 0xFFFFFFFFu &&
                   doc.dimStyle(DimStyleId{static_cast<std::uint32_t>(k.id)});
        case Space::Block:
            return k.id <= 0xFFFFFFFFu && doc.block(BlockId{static_cast<std::uint32_t>(k.id)});
        case Space::Header:
        case Space::Order:
            return true;
    }
    return false;
}

namespace {
constexpr std::uint8_t kMagic[4] = {'T', 'B', 'V', 'R'};
}

std::vector<std::uint8_t> encodeVersions(const VersionMap& versions, std::uint64_t sequence) {
    detail::Writer w;
    for (std::uint8_t c : kMagic)
        w.u8(c);
    w.u8(1);
    w.varint(sequence);
    w.varint(versions.size());
    // Sorted by key, so ids of one space are close together and delta-code well.
    Space space = Space::Entity;
    std::uint64_t lastId = 0;
    for (const auto& [k, v] : versions.entries()) {
        if (k.space != space) {
            space = k.space;
            lastId = 0;
        }
        w.u8(static_cast<std::uint8_t>(k.space));
        w.varint(k.id - lastId);
        w.varint(v);
        lastId = k.id;
    }
    return std::move(w.bytes());
}

Result<DecodedVersions> decodeVersions(std::span<const std::uint8_t> bytes,
                                       std::size_t maxEntries) {
    try {
        detail::Reader r(bytes);
        for (std::uint8_t c : kMagic) {
            if (r.u8() != c)
                throw detail::FormatError("not a versions record");
        }
        if (r.u8() != 1)
            throw detail::FormatError("unsupported versions record");
        DecodedVersions out;
        out.sequence = r.varint();
        std::size_t n = r.count(3);
        if (n > maxEntries)
            throw detail::FormatError("too many version entries");
        Space space = Space::Entity;
        std::uint64_t lastId = 0;
        bool first = true;
        Key prev{};
        for (std::size_t i = 0; i < n; ++i) {
            std::uint8_t s = r.u8();
            if (s > kLastSpace)
                throw detail::FormatError("invalid version space");
            if (static_cast<Space>(s) != space) {
                if (static_cast<Space>(s) < space)
                    throw detail::FormatError("version entries out of order");
                space = static_cast<Space>(s);
                lastId = 0;
            }
            std::uint64_t step = r.varint();
            if (step > UINT64_MAX - lastId)
                throw detail::FormatError("version id overflows");
            Key k{space, lastId + step};
            if (!first && !(prev < k))
                throw detail::FormatError("version entries out of order");
            std::uint64_t v = r.varint();
            if (v == 0)
                throw detail::FormatError("version 0 is not stored");
            out.versions.set(k, v);
            prev = k;
            first = false;
            lastId = k.id;
        }
        if (!r.atEnd())
            throw detail::FormatError("trailing data");
        return out;
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::Parse, std::string("bad versions record: ") + ex.what());
    }
}

}  // namespace tbcad::collab
