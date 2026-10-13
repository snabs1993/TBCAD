#pragma once

#include <doctest/doctest.h>

#include <deque>
#include <memory>

#include "tbcad/collab/host.h"
#include "tbcad/collab/replica.h"
#include "tbcad/collab/snapshot.h"
#include "tbcad/core/dump.h"

namespace tbcad::collab::testing {

template <class T>
T must(Result<T> r) {
    if (!r.ok())
        FAIL(r.error().message);
    return std::move(r).value();
}

inline Entity line(LayerId layer, Vec2 a, Vec2 b) {
    Entity e;
    e.common.layer = layer;
    e.data = LineData{a, b};
    return e;
}

// A session in one process. Messages queue up and are delivered when the test says so, which is
// how the tests create "two people edit at the same time".
struct Session {
    struct Guest {
        PeerId peer = 0;
        std::unique_ptr<Document> doc = std::make_unique<Document>();
        std::unique_ptr<Replica> replica{};
        std::deque<std::vector<std::uint8_t>> toHost{};    // encoded Ops on the way to the host
        std::deque<std::vector<std::uint8_t>> fromHost{};  // encoded Accepted / refusals
        std::size_t rejected = 0;
        std::vector<std::string> refusals{};
    };

    Document hostDoc;
    Host host;
    std::vector<std::unique_ptr<Guest>> guests;
    std::uint64_t now = 0;
    std::vector<RejectReason> reasons{};
    // The host's answers to a guest: broadcast bytes for everyone, or a refusal for one.
    struct Refusal {
        static constexpr std::uint8_t kTag = 0xFF;
    };

    explicit Session(HostConfig cfg = {}) : host(hostDoc, std::move(cfg)) {
        host.onAccepted = [this](const Accepted& a) {
            std::vector<std::uint8_t> bytes = encodeAccepted(a);
            for (auto& g : guests)
                g->fromHost.push_back(bytes);
        };
    }

    // A guest that starts from the host's current document (as a join snapshot would).
    Guest& join(Role role = Role::Editor) {
        auto g = std::make_unique<Guest>();
        g->peer = host.addPeer(role);
        Snapshot s =
            must(loadSnapshot(must(saveSnapshot(hostDoc, host.versions(), host.sequence()))));
        g->doc = std::move(s.document);
        g->replica = std::make_unique<Replica>(*g->doc, g->peer);
        g->replica->restore(std::move(s.versions), s.sequence);
        (void)g->replica->addRange(host.grantRange(g->peer));
        Guest* raw = g.get();
        g->replica->onLocalOp = [raw](const Op& op) { raw->toHost.push_back(encodeOp(op)); };
        guests.push_back(std::move(g));
        return *raw;
    }

    // Host handles the oldest queued op of one guest. False when there was none.
    bool hostReceiveOne(Guest& g) {
        if (g.toHost.empty())
            return false;
        auto bytes = std::move(g.toHost.front());
        g.toHost.pop_front();
        Submitted s = host.submitEncoded(g.peer, bytes, now += 10);
        if (!s.ok()) {
            // Tag 0xFF + clientSeq: the refusal goes to the author only.
            Result<Op> op = decodeOp(bytes);
            std::vector<std::uint8_t> msg{Refusal::kTag};
            std::uint64_t seq = op.ok() ? op.value().clientSeq : 0;
            for (int i = 0; i < 8; ++i)
                msg.push_back(static_cast<std::uint8_t>(seq >> (8 * i)));
            g.fromHost.push_back(std::move(msg));
            g.refusals.push_back(s.rejection->message);
            reasons.push_back(s.rejection->reason);
        }
        return true;
    }
    // Guest handles the oldest message the host sent it.
    bool guestReceiveOne(Guest& g) {
        if (g.fromHost.empty())
            return false;
        auto bytes = std::move(g.fromHost.front());
        g.fromHost.pop_front();
        if (bytes[0] == Refusal::kTag) {
            std::uint64_t seq = 0;
            for (int i = 0; i < 8; ++i)
                seq |= std::uint64_t(bytes[1 + std::size_t(i)]) << (8 * i);
            auto r = g.replica->reject(seq);
            REQUIRE(r.ok());
            ++g.rejected;
        } else {
            auto a = must(decodeAccepted(bytes));
            auto r = g.replica->apply(a);
            if (!r.ok())
                FAIL(r.error().message);
        }
        return true;
    }
    void hostReceive(Guest& g) {
        while (hostReceiveOne(g)) {
        }
    }
    void guestReceive(Guest& g) {
        while (guestReceiveOne(g)) {
        }
    }
    // Everything in flight arrives, until nothing is left.
    void settle() {
        for (int round = 0; round < 1000; ++round) {
            bool busy = false;
            for (auto& g : guests) {
                busy = busy || !g->toHost.empty() || !g->fromHost.empty();
                hostReceive(*g);
            }
            for (auto& g : guests)
                guestReceive(*g);
            if (!busy)
                return;
        }
        FAIL("session did not settle");
    }
};

}  // namespace tbcad::collab::testing
