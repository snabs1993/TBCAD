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
    std::string s = "a\xFF" "b";
    CHECK(clusterBoundaries(s) == std::vector<std::size_t>{0, 1, 2, 3});
}
