#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "tbcad/core/core.h"

#if defined(__linux__)
#include <malloc.h>
#endif

using namespace tbcad;

namespace {

// Bytes currently allocated from the heap, or 0 where we cannot tell.
std::size_t heapInUse() {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 33)
    return mallinfo2().uordblks + mallinfo2().hblkhd;
#else
    return 0;
#endif
}

}  // namespace

TEST_CASE("one million lines in one transaction") {
    constexpr int kCount = 1'000'000;
    Document doc;
    std::size_t memBefore = heapInUse();
    auto t0 = std::chrono::steady_clock::now();
    ChangeSet cs;
    {
        Transaction tx = doc.begin("bulk");
        for (int i = 0; i < kCount; ++i) {
            double x = i;
            auto id = tx.add(Entity{{}, LineData{{x, 0}, {x, 1}}});
            REQUIRE(id);
        }
        cs = tx.commit();
    }
    auto t1 = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();
    std::size_t memAfter = heapInUse();

    CHECK(doc.entityCount() == kCount);
    CHECK(cs.entities.size() == kCount);
    std::printf("1M lines: %.3f s, heap %.1f MB (document + change set), sizeof(Entity) = %zu\n",
                seconds, double(memAfter - memBefore) / 1e6, sizeof(Entity));
#ifdef NDEBUG
    CHECK(seconds < 1.0);
#endif
    if (memBefore != 0)
        CHECK(memAfter - memBefore < 230u * 1000 * 1000);  // two copies of 1 M entities of 96 bytes

    // Undo and redo of the whole batch stay cheap and exact.
    doc.apply(cs, Direction::Undo);
    CHECK(doc.entityCount() == 0);
    doc.apply(cs, Direction::Redo);
    CHECK(doc.entityCount() == kCount);
    const Entity* last = doc.find(EntityId{kCount});
    REQUIRE(last);
    CHECK(entityAs<LineData>(*last)->a.x == kCount - 1);
}
