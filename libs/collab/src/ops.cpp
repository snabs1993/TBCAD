#include "tbcad/collab/ops.h"

#include <algorithm>

#include "bytes.h"

namespace tbcad::collab {

namespace {

constexpr std::uint8_t kOpMagic[4] = {'T', 'B', 'O', 'P'};
constexpr std::uint8_t kAcceptedMagic[4] = {'T', 'B', 'A', 'C'};
constexpr std::uint8_t kFormat = 1;

bool existedBefore(const ChangeSet& cs, const Key& k) {
    auto idOf = [](const auto& c) { return (c.after ? c.after->id : c.before->id).value; };
    switch (k.space) {
        case Space::Entity:
            for (const EntityChange& c : cs.entities) {
                if (cs.idOf(c).value == k.id)
                    return c.before != kNoSnapshot;
            }
            return false;
        case Space::Layer:
            for (const auto& c : cs.layers)
                if (idOf(c) == k.id)
                    return c.before.has_value();
            return false;
        case Space::Linetype:
            for (const auto& c : cs.linetypes)
                if (idOf(c) == k.id)
                    return c.before.has_value();
            return false;
        case Space::TextStyle:
            for (const auto& c : cs.textStyles)
                if (idOf(c) == k.id)
                    return c.before.has_value();
            return false;
        case Space::DimStyle:
            for (const auto& c : cs.dimStyles)
                if (idOf(c) == k.id)
                    return c.before.has_value();
            return false;
        case Space::Block:
            for (const auto& c : cs.blocks)
                if (idOf(c) == k.id)
                    return c.before.has_value();
            return false;
        case Space::Header:
        case Space::Order:
            return true;
    }
    return false;
}

void putHeader(detail::Writer& w, const std::uint8_t (&magic)[4]) {
    for (std::uint8_t c : magic)
        w.u8(c);
    w.u8(kFormat);
}

void checkHeader(detail::Reader& r, const std::uint8_t (&magic)[4]) {
    for (std::uint8_t c : magic) {
        if (r.u8() != c)
            throw detail::FormatError("wrong record type");
    }
    if (r.u8() != kFormat)
        throw detail::FormatError("unsupported record version");
}

PeerId peerId(detail::Reader& r) {
    std::uint64_t v = r.varint();
    if (v > 0xFFFFFFFFu)
        throw detail::FormatError("peer id out of range");
    return static_cast<PeerId>(v);
}

}  // namespace

Op makeOp(PeerId author, std::uint64_t clientSeq, const ChangeSet& committed,
          const VersionMap& versions) {
    Op op;
    op.author = author;
    op.clientSeq = clientSeq;
    op.delta = io::deltaOf(committed);
    for (const Key& k : keysOf(op.delta))
        op.bases.push_back({k, versions.get(k), existedBefore(committed, k)});
    return op;
}

std::vector<std::uint8_t> encodeOp(const Op& op) {
    detail::Writer w;
    putHeader(w, kOpMagic);
    w.varint(op.author);
    w.varint(op.clientSeq);
    w.varint(op.bases.size());
    for (const Base& b : op.bases) {
        w.u8(static_cast<std::uint8_t>(b.key.space));
        w.varint(b.key.id);
        w.varint(b.version);
        w.u8(b.existed ? 1 : 0);
    }
    w.raw(io::encodeDelta(op.delta));
    return std::move(w.bytes());
}

Result<Op> decodeOp(std::span<const std::uint8_t> bytes, const io::DeltaLimits& limits) {
    if (bytes.size() > limits.maxBytes)
        return makeError(ErrorCode::OutOfRange, "operation is too large");
    try {
        detail::Reader r(bytes);
        checkHeader(r, kOpMagic);
        Op op;
        op.author = peerId(r);
        op.clientSeq = r.varint();
        std::size_t n = r.count(4);
        if (n > limits.maxEntities + 6 * limits.maxRows + 2)
            throw detail::FormatError("too many bases");
        op.bases.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            Base b;
            std::uint8_t s = r.u8();
            if (s > kLastSpace)
                throw detail::FormatError("invalid space");
            b.key.space = static_cast<Space>(s);
            b.key.id = r.varint();
            b.version = r.varint();
            std::uint8_t e = r.u8();
            if (e > 1)
                throw detail::FormatError("invalid flag");
            b.existed = e == 1;
            if (!op.bases.empty() && !(op.bases.back().key < b.key))
                throw detail::FormatError("bases out of order");
            op.bases.push_back(b);
        }
        Result<io::Delta> d = io::decodeDelta(r.rest(), limits);
        if (!d)
            return d.error();
        op.delta = std::move(d.value());
        return op;
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::Parse, std::string("bad operation: ") + ex.what());
    }
}

std::vector<std::uint8_t> encodeAccepted(const Accepted& a) {
    detail::Writer w;
    putHeader(w, kAcceptedMagic);
    w.varint(a.seq);
    w.varint(a.author);
    w.varint(a.clientSeq);
    w.raw(io::encodeDelta(a.delta));
    return std::move(w.bytes());
}

Result<Accepted> decodeAccepted(std::span<const std::uint8_t> bytes,
                                const io::DeltaLimits& limits) {
    if (bytes.size() > limits.maxBytes)
        return makeError(ErrorCode::OutOfRange, "broadcast is too large");
    try {
        detail::Reader r(bytes);
        checkHeader(r, kAcceptedMagic);
        Accepted a;
        a.seq = r.varint();
        a.author = peerId(r);
        a.clientSeq = r.varint();
        if (a.seq == 0)
            throw detail::FormatError("sequence 0");
        Result<io::Delta> d = io::decodeDelta(r.rest(), limits);
        if (!d)
            return d.error();
        a.delta = std::move(d.value());
        return a;
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::Parse, std::string("bad broadcast: ") + ex.what());
    }
}

void rebasePending(std::vector<Op>& pending, const Accepted& own) {
    std::vector<Key> keys = keysOf(own.delta);
    for (Op& op : pending) {
        for (Base& b : op.bases) {
            auto it = std::lower_bound(keys.begin(), keys.end(), b.key);
            if (it == keys.end() || *it != b.key)
                continue;
            b.version = own.seq;
            // Whether the thing exists now is what the earlier op left behind.
            auto row = [&](const auto& rows) {
                for (const auto& r : rows)
                    if (r.id == b.key.id)
                        b.existed = r.after.has_value();
            };
            switch (b.key.space) {
                case Space::Entity:
                    for (const auto& e : own.delta.entities)
                        if (e.id.value == b.key.id)
                            b.existed = e.after.has_value();
                    break;
                case Space::Layer:
                    row(own.delta.layers);
                    break;
                case Space::Linetype:
                    row(own.delta.linetypes);
                    break;
                case Space::TextStyle:
                    row(own.delta.textStyles);
                    break;
                case Space::DimStyle:
                    row(own.delta.dimStyles);
                    break;
                case Space::Block:
                    row(own.delta.blocks);
                    break;
                case Space::Header:
                case Space::Order:
                    break;
            }
        }
    }
}

}  // namespace tbcad::collab
