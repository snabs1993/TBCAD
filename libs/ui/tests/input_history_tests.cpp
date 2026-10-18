#include <doctest/doctest.h>

#include "tbcad/ui/input_history.h"

using tbcad::ui::InputHistory;

TEST_CASE("Up goes back through the typed lines, Down comes forward to the draft") {
    InputHistory h;
    h.push("LINE");
    h.push("CIRCLE 0,0 5");
    h.push("ZOOM E");
    CHECK(h.older("MO") == "ZOOM E");
    CHECK(h.older("ignored") == "CIRCLE 0,0 5");
    CHECK(h.older("") == "LINE");
    CHECK(h.older("") == "LINE");  // stops at the oldest
    CHECK(h.newer("") == "CIRCLE 0,0 5");
    CHECK(h.newer("") == "ZOOM E");
    CHECK(h.newer("") == "MO");  // the half-typed text comes back
    CHECK_FALSE(h.browsing());
    CHECK(h.newer("abc") == "abc");  // Down without Up changes nothing
}

TEST_CASE("empty lines and repeats are not stored, an Enter starts over from the newest") {
    InputHistory h;
    h.push("");
    h.push("LINE");
    h.push("LINE");
    CHECK(h.lines().size() == 1);
    CHECK(h.older("") == "LINE");
    h.push("MOVE");
    CHECK_FALSE(h.browsing());
    CHECK(h.older("") == "MOVE");
}

TEST_CASE("the list is capped and no history leaves the box alone") {
    InputHistory empty;
    CHECK(empty.older("keep") == "keep");
    InputHistory h(3);
    for (const char* l : {"A", "B", "C", "D", "E"})
        h.push(l);
    CHECK(h.lines() == std::vector<std::string>{"C", "D", "E"});
}
