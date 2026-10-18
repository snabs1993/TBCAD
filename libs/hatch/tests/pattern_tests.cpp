#include <doctest/doctest.h>

#include <algorithm>

#include "tbcad/hatch/pattern.h"

using namespace tbcad;
using namespace tbcad::hatch;

TEST_CASE("the built-in set has every pattern the spec asks for") {
    for (const char* name :
         {"SOLID", "LINE45", "CROSS45", "LINE0", "BRICK", "GRID", "DOTS", "SAND", "EARTH"}) {
        INFO(name);
        const Pattern* p = findBuiltinPattern(name);
        REQUIRE(p != nullptr);
        CHECK_FALSE(p->description.empty());
        if (p->solid)
            CHECK(p->families.empty());
        else
            CHECK_FALSE(p->families.empty());
    }
    CHECK(findBuiltinPattern("line45") == findBuiltinPattern("LINE45"));
    CHECK(findBuiltinPattern("NOPE") == nullptr);
}

TEST_CASE("pattern parameters are read in the documented units") {
    const Pattern* brick = findBuiltinPattern("BRICK");
    REQUIRE(brick != nullptr);
    REQUIRE(brick->families.size() == 3);
    CHECK(brick->families[0].angle == doctest::Approx(0));
    CHECK(brick->families[1].angle == doctest::Approx(kPi / 2));
    CHECK(brick->families[1].delta.y == doctest::Approx(12));
    CHECK(brick->families[1].dashes == std::vector<double>{6, -6});
    CHECK(brick->families[1].cycleLength() == doctest::Approx(12));
    CHECK(brick->families[2].origin.x == doctest::Approx(6));
}

TEST_CASE("SAND is deterministic: a fixed set of dot families") {
    const Pattern* sand = findBuiltinPattern("SAND");
    REQUIRE(sand != nullptr);
    CHECK(sand->families.size() == 8);
    for (const PatternFamily& f : sand->families) {
        REQUIRE(f.dashes.size() == 2);
        CHECK(f.dashes[0] == 0);  // a dot
        CHECK(f.dashes[1] < 0);
    }
}

TEST_CASE("parsePatterns reports the offending line") {
    CHECK(parsePatterns("[A]\nline = 0 | 0,0 | 0,5 |\n").ok());
    auto bad = parsePatterns("[A]\nline = 0 | 0,0 | 0,0 |\n");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("line 2") != std::string::npos);
    CHECK_FALSE(parsePatterns("line = 0 | 0,0 | 0,5 |\n").ok());           // no section
    CHECK_FALSE(parsePatterns("[A]\nline = x | 0,0 | 0,5 |\n").ok());      // angle
    CHECK_FALSE(parsePatterns("[A]\nline = 0 | 0 | 0,5 |\n").ok());        // origin
    CHECK_FALSE(parsePatterns("[A]\nline = 0 | 0,0 | 0,5 | 0,0\n").ok());  // zero cycle
    CHECK_FALSE(parsePatterns("[A]\nfoo = 1\n").ok());                     // unknown key
    CHECK_FALSE(parsePatterns("[A]\n").ok());                              // no lines, not solid
    CHECK_FALSE(parsePatterns("[A]\nsolid=yes\n[A]\nsolid=yes\n").ok());   // duplicate
    CHECK(parsePatterns("# only a comment\n").ok());
}

TEST_CASE("user patterns shadow built-in ones and show up in names()") {
    PatternRegistry::clearUser();
    auto parsed =
        parsePatterns("[MYHATCH]\nline = 30 | 0,0 | 0,2.5 | 5,-2\n[LINE45]\nsolid = yes\n");
    REQUIRE(parsed.ok());
    for (Pattern& p : parsed.value())
        PatternRegistry::add(p);
    REQUIRE(PatternRegistry::find("myhatch") != nullptr);
    CHECK(PatternRegistry::find("LINE45")->solid);  // shadowed
    auto names = PatternRegistry::names();
    CHECK(std::find(names.begin(), names.end(), "MYHATCH") != names.end());
    CHECK(std::count(names.begin(), names.end(), "LINE45") == 1);
    PatternRegistry::clearUser();
    CHECK_FALSE(PatternRegistry::find("LINE45")->solid);
    CHECK(PatternRegistry::find("MYHATCH") == nullptr);
}

TEST_CASE("pattern names from other programs resolve: ANSI31, AR-CONC, AR-B816") {
    const Pattern* ansi = PatternRegistry::find("ansi31");
    REQUIRE(ansi);
    REQUIRE(ansi->families.size() == 1);
    CHECK(ansi->families[0].delta.y == doctest::Approx(3.175));
    CHECK(PatternRegistry::find("AR-CONC") == PatternRegistry::find("CONCRETE"));
    CHECK(PatternRegistry::find("AR-B816") == PatternRegistry::find("BRICK"));
}
