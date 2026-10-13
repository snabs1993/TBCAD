#include "tbcad/collab/protocol.h"

#include <algorithm>

#include "bytes.h"

namespace tbcad::collab {

namespace {

using detail::FormatError;
using detail::Reader;
using detail::Writer;

enum Tag : std::uint8_t {
    kHello = 1,
    kWelcome,
    kDenied,
    kOp,
    kAccepted,
    kRefused,
    kRangeRequest,
    kRangeGrant,
    kPeerJoined,
    kPeerLeft,
    kRoleChanged,
    kBye,
};

void putString(Writer& w, const std::string& s, std::size_t max) {
    const std::size_t n = std::min(s.size(), max);
    w.varint(n);
    w.raw({reinterpret_cast<const std::uint8_t*>(s.data()), n});
}

std::string getString(Reader& r, std::size_t max) {
    const std::size_t n = r.count(1);
    if (n > max)
        throw FormatError("text too long");
    auto b = r.take(n);
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

Role getRole(Reader& r) {
    std::uint8_t v = r.u8();
    if (v > static_cast<std::uint8_t>(Role::Viewer))
        throw FormatError("unknown role");
    return static_cast<Role>(v);
}

void putPeer(Writer& w, const PeerInfo& p) {
    w.varint(p.peer);
    putString(w, p.name, kMaxNameBytes);
    w.u8(static_cast<std::uint8_t>(p.role));
}

PeerInfo getPeer(Reader& r) {
    PeerInfo p;
    p.peer = static_cast<PeerId>(r.varint());
    p.name = getString(r, kMaxNameBytes);
    p.role = getRole(r);
    return p;
}

void putRange(Writer& w, const IdRange& range) {
    w.varint(range.first);
    w.varint(range.limit);
}

IdRange getRange(Reader& r) {
    IdRange range;
    range.first = r.varint();
    range.limit = r.varint();
    return range;
}

struct Encoder {
    Writer& w;
    void operator()(const msg::Hello& m) {
        w.u8(kHello);
        w.varint(m.protocol);
        putString(w, m.name, kMaxNameBytes);
    }
    void operator()(const msg::Welcome& m) {
        w.u8(kWelcome);
        w.varint(m.peer);
        w.u8(static_cast<std::uint8_t>(m.role));
        w.varint(m.sequence);
        putRange(w, m.range);
        putString(w, m.hostName, kMaxNameBytes);
        w.varint(m.peers.size());
        for (const PeerInfo& p : m.peers)
            putPeer(w, p);
        w.raw(m.snapshot);
    }
    void operator()(const msg::Denied& m) {
        w.u8(kDenied);
        putString(w, m.reason, kMaxMessageBytes);
    }
    void operator()(const msg::OpMsg& m) {
        w.u8(kOp);
        w.raw(encodeOp(m.op));
    }
    void operator()(const msg::AcceptedMsg& m) {
        w.u8(kAccepted);
        w.raw(encodeAccepted(m.accepted));
    }
    void operator()(const msg::Refused& m) {
        w.u8(kRefused);
        w.varint(m.clientSeq);
        w.u8(static_cast<std::uint8_t>(m.reason));
        putString(w, m.message, kMaxMessageBytes);
        w.u8(m.key ? 1 : 0);
        if (m.key) {
            w.u8(static_cast<std::uint8_t>(m.key->space));
            w.varint(m.key->id);
        }
        w.varint(m.currentVersion);
        w.varint(m.byPeer);
        putString(w, m.byName, kMaxNameBytes);
    }
    void operator()(const msg::RangeRequest&) { w.u8(kRangeRequest); }
    void operator()(const msg::RangeGrant& m) {
        w.u8(kRangeGrant);
        putRange(w, m.range);
    }
    void operator()(const msg::PeerJoined& m) {
        w.u8(kPeerJoined);
        putPeer(w, m.peer);
    }
    void operator()(const msg::PeerLeft& m) {
        w.u8(kPeerLeft);
        w.varint(m.peer);
    }
    void operator()(const msg::RoleChanged& m) {
        w.u8(kRoleChanged);
        w.varint(m.peer);
        w.u8(static_cast<std::uint8_t>(m.role));
    }
    void operator()(const msg::Bye& m) {
        w.u8(kBye);
        putString(w, m.reason, kMaxMessageBytes);
    }
};

}  // namespace

std::vector<std::uint8_t> encodeMessage(const Message& m) {
    Writer w;
    std::visit(Encoder{w}, m);
    return std::move(w.bytes());
}

Result<Message> decodeMessage(std::span<const std::uint8_t> bytes, const io::DeltaLimits& limits) {
    try {
        Reader r(bytes);
        switch (r.u8()) {
            case kHello: {
                msg::Hello m;
                m.protocol = static_cast<std::uint32_t>(r.varint());
                m.name = getString(r, kMaxNameBytes);
                return Message{std::move(m)};
            }
            case kWelcome: {
                msg::Welcome m;
                m.peer = static_cast<PeerId>(r.varint());
                m.role = getRole(r);
                m.sequence = r.varint();
                m.range = getRange(r);
                m.hostName = getString(r, kMaxNameBytes);
                const std::size_t n = r.count(3);
                for (std::size_t i = 0; i < n; ++i)
                    m.peers.push_back(getPeer(r));
                auto rest = r.rest();
                m.snapshot.assign(rest.begin(), rest.end());
                return Message{std::move(m)};
            }
            case kDenied:
                return Message{msg::Denied{getString(r, kMaxMessageBytes)}};
            case kOp: {
                Result<Op> op = decodeOp(r.rest(), limits);
                if (!op)
                    return op.error();
                return Message{msg::OpMsg{std::move(op.value())}};
            }
            case kAccepted: {
                Result<Accepted> a = decodeAccepted(r.rest(), limits);
                if (!a)
                    return a.error();
                return Message{msg::AcceptedMsg{std::move(a.value())}};
            }
            case kRefused: {
                msg::Refused m;
                m.clientSeq = r.varint();
                std::uint8_t reason = r.u8();
                if (reason > static_cast<std::uint8_t>(RejectReason::TooLarge))
                    throw FormatError("unknown reason");
                m.reason = static_cast<RejectReason>(reason);
                m.message = getString(r, kMaxMessageBytes);
                if (r.u8()) {
                    std::uint8_t space = r.u8();
                    if (space > kLastSpace)
                        throw FormatError("unknown space");
                    m.key = Key{static_cast<Space>(space), r.varint()};
                }
                m.currentVersion = r.varint();
                m.byPeer = static_cast<PeerId>(r.varint());
                m.byName = getString(r, kMaxNameBytes);
                return Message{std::move(m)};
            }
            case kRangeRequest:
                return Message{msg::RangeRequest{}};
            case kRangeGrant:
                return Message{msg::RangeGrant{getRange(r)}};
            case kPeerJoined:
                return Message{msg::PeerJoined{getPeer(r)}};
            case kPeerLeft:
                return Message{msg::PeerLeft{static_cast<PeerId>(r.varint())}};
            case kRoleChanged: {
                msg::RoleChanged m;
                m.peer = static_cast<PeerId>(r.varint());
                m.role = getRole(r);
                return Message{m};
            }
            case kBye:
                return Message{msg::Bye{getString(r, kMaxMessageBytes)}};
            default:
                return makeError(ErrorCode::InvalidArgument, "unknown message");
        }
    } catch (const FormatError& e) {
        return makeError(ErrorCode::InvalidArgument, std::string("bad message: ") + e.what());
    }
}

}  // namespace tbcad::collab
