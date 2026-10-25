#include <doctest/doctest.h>

#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::text;

TEST_CASE("clusters: Latin and empty") {
    CHECK(clusterBoundaries("") == std::vector<std::size_t>{0});
    CHECK(clusterBoundaries("abc") == std::vector<std::size_t>{0, 1, 2, 3});
}

TEST_CASE("clusters: Thai marks stay with their base") {
    // ห + MAI THO, อ, ง: "ห้อง"
    std::string s = "ห้อง";
    CHECK(clusterBoundaries(s) == std::vector<std::size_t>{0, 6, 9, 12});
    // ก + SARA I + MAI EK: one cluster
    std::string t = "กิ่";
    CHECK(clusterBoundaries(t) == std::vector<std::size_t>{0, 9});
    // SARA AM stays with its consonant
    CHECK(clusterBoundaries("กำ") == std::vector<std::size_t>{0, 6});
}

TEST_CASE("clusters: a Thai leading vowel stays with the following consonant") {
    // เก = SARA E + KO KAI
    CHECK(clusterBoundaries("เก") == std::vector<std::size_t>{0, 6});
    // เกาะ: เก | า | ะ
    CHECK(clusterBoundaries("เกาะ") == std::vector<std::size_t>{0, 6, 9, 12});
}

TEST_CASE("clusters: invalid UTF-8 counts one cluster per bad byte") {
    std::string s =
        "a\xFF"
        "b";
    CHECK(clusterBoundaries(s) == std::vector<std::size_t>{0, 1, 2, 3});
}

TEST_CASE("clusters: next and prev step over whole clusters, like clusterBoundaries") {
    const std::string samples[] = {"",
                                   "abc",
                                   "ห้องนอน",
                                   "กิ่ง กำ เกาะ",
                                   "a\xFF"
                                   "b",
                                   "e\xCC\x81x",
                                   "ก\xE2\x80\x8D"
                                   "ข",
                                   "\xF0\x9F\x98\x80ก"};
    for (const std::string& s : samples) {
        const auto b = clusterBoundaries(s);
        for (std::size_t k = 0; k + 1 < b.size(); ++k) {
            CHECK(nextClusterStart(s, b[k]) == b[k + 1]);
            CHECK(prevClusterStart(s, b[k + 1]) == b[k]);
        }
        for (std::size_t idx = 0; idx <= s.size(); ++idx) {
            // from anywhere: the next boundary above and the previous one below
            std::size_t up = s.size(), down = 0;
            for (std::size_t x : b) {
                if (x > idx) {
                    up = x;
                    break;
                }
            }
            for (std::size_t x : b)
                if (x < idx)
                    down = x;
            const bool atCodePoint = idx == s.size() || (s[idx] & 0xC0) != 0x80;
            if (idx < s.size() && atCodePoint)
                CHECK_MESSAGE(nextClusterStart(s, idx) == up, s);
            if (idx > 0 && atCodePoint)
                CHECK_MESSAGE(prevClusterStart(s, idx) == down, s);
        }
    }
    CHECK(nextClusterStart("ห้อง", 0) == 6);
    CHECK(nextClusterStart("ห้อง", 6) == 9);
    CHECK(prevClusterStart("ห้อง", 12) == 9);
    CHECK(prevClusterStart("ห้อง", 6) == 0);
    CHECK(nextClusterStart("ab", 2) == 2);
    CHECK(prevClusterStart("ab", 0) == 0);
}
