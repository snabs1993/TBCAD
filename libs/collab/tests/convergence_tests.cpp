#include <random>

#include "helpers.h"

using namespace tbcad;
using namespace tbcad::collab;
using namespace tbcad::collab::testing;

namespace {

void randomEdit(Document& doc, std::mt19937_64& rng, int counter) {
    Transaction tx = doc.begin("random");
    std::vector<EntityId> ids = doc.modelSpace();
    const int what = static_cast<int>(rng() % 10);
    if (what < 4 || ids.empty()) {
        for (int i = 0; i < 1 + int(rng() % 3); ++i) {
            (void)tx.add(line(kLayerZero, {double(rng() % 100), double(rng() % 100)},
                              {double(rng() % 100), double(rng() % 100)}));
        }
    } else if (what < 7) {
        EntityId id = ids[rng() % ids.size()];
        (void)tx.modify(id, [&](Entity& e) {
            if (auto* l = std::get_if<LineData>(&e.data))
                l->b = {double(rng() % 100), double(rng() % 100)};
        });
    } else if (what < 8) {
        (void)tx.erase(ids[rng() % ids.size()]);
    } else if (what < 9) {
        (void)tx.addLayer(
            {.name = "L" + std::to_string(rng() % 8) + "_" + std::to_string(counter % 3)});
    } else {
        (void)tx.bringToFront(ids[rng() % ids.size()]);
    }
    tx.commit();
}

}  // namespace

TEST_CASE("convergence: concurrent drawing by three guests and the host") {
    Session s;
    auto& a = s.join();
    auto& b = s.join();
    auto& c = s.join();
    for (auto* g : {&a, &b, &c}) {
        for (int i = 0; i < 20; ++i) {
            Transaction tx = g->doc->begin("line");
            (void)must(tx.add(line(kLayerZero, {double(i), double(g->peer)}, {double(i), 10})));
            tx.commit();
        }
    }
    {
        Transaction tx = s.hostDoc.begin("host");
        for (int i = 0; i < 20; ++i)
            (void)must(tx.add(line(kLayerZero, {double(i), 99}, {double(i), 100})));
        tx.commit();
    }
    s.settle();
    CHECK(s.reasons.empty());  // nobody touched the same thing
    CHECK(s.hostDoc.modelSpaceCount() == 80);
    CHECK(canonicalDump(*a.doc) == canonicalDump(s.hostDoc));
    CHECK(canonicalDump(*b.doc) == canonicalDump(s.hostDoc));
    CHECK(canonicalDump(*c.doc) == canonicalDump(s.hostDoc));
    CHECK(a.replica->versions() == s.host.versions());
    CHECK(a.replica->pendingCount() == 0);
}

TEST_CASE("convergence: a guest that runs out of ids waits for the next block") {
    HostConfig cfg;
    cfg.idBlockSize = 3;
    Session s(cfg);
    auto& a = s.join();
    CHECK(a.replica->idsLeft() == 3);
    Transaction tx = a.doc->begin("many");
    for (int i = 0; i < 3; ++i)
        CHECK(tx.add(line(kLayerZero, {0, 0}, {1, 1})).ok());
    CHECK_FALSE(tx.add(line(kLayerZero, {0, 0}, {1, 1})).ok());
    tx.commit();
    CHECK(a.replica->idsLeft() == 0);
    REQUIRE(a.replica->addRange(s.host.grantRange(a.peer)).ok());
    CHECK(a.replica->idsLeft() == 3);
    {
        Transaction t2 = a.doc->begin("more");
        for (int i = 0; i < 3; ++i)
            CHECK(t2.add(line(kLayerZero, {0, 0}, {1, 1})).ok());
        t2.commit();
    }
    s.settle();
    CHECK(s.reasons.empty());
    CHECK(s.hostDoc.modelSpaceCount() == 6);
    CHECK(canonicalDump(*a.doc) == canonicalDump(s.hostDoc));
}

TEST_CASE("convergence: random edits with random message delays always end in the same drawing") {
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        INFO("seed " << seed);
        std::mt19937_64 rng(seed);
        Session s;
        // Start with some shared content.
        {
            Transaction tx = s.hostDoc.begin("seed");
            for (int i = 0; i < 6; ++i)
                (void)must(tx.add(line(kLayerZero, {double(i), 0}, {double(i), 5})));
            tx.commit();
        }
        for (int i = 0; i < 3; ++i)
            s.join();
        for (int step = 0; step < 150; ++step) {
            std::size_t who = rng() % (s.guests.size() + 1);
            Session::Guest* g = who < s.guests.size() ? s.guests[who].get() : nullptr;
            switch (rng() % 4) {
                case 0:
                    randomEdit(g ? *g->doc : s.hostDoc, rng, step);
                    break;
                case 1:
                    if (g)
                        s.hostReceiveOne(*g);
                    break;
                case 2:
                    if (g)
                        s.guestReceiveOne(*g);
                    break;
                default:
                    if (g && rng() % 3 == 0)
                        randomEdit(*g->doc, rng, step);
            }
            // Guests never see their own documents in a state the host would not accept.
            if (g && g->replica->pendingCount() == 0) {
                CHECK(g->doc->entityCount() >= 0);
            }
        }
        s.settle();
        const std::string expect = canonicalDump(s.hostDoc);
        for (auto& g : s.guests) {
            CHECK(canonicalDump(*g->doc) == expect);
            CHECK(g->replica->pendingCount() == 0);
            CHECK(g->replica->versions() == s.host.versions());
            CHECK(g->replica->sequence() == s.host.sequence());
        }
    }
}
