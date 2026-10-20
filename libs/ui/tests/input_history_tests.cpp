#include <doctest/doctest.h>

#include "tbcad/ui/input_history.h"

using tbcad::ui::InputHistory;

TEST_CASE("Up goes back through the typed lines, Down comes forward to the draft") {
    InputHistory h;
    h.push("LINE");
    h.push("CIRCLE 0,0 5");
    h.push("ZOOM E");
    CHECK(h.older("") == "ZOOM E");
    CHECK(h.older("ZOOM E") == "CIRCLE 0,0 5");
    CHECK(h.older("") == "LINE");
    CHECK(h.older("") == "LINE");  // stops at the oldest
    CHECK(h.newer("") == "CIRCLE 0,0 5");
    CHECK(h.newer("") == "ZOOM E");
    CHECK(h.newer("") == "");  // the box is empty again, as it was
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

TEST_CASE("typed letters limit Up and Down to the lines that start with them") {
    InputHistory h;
    for (const char* l : {"LINE", "CIRCLE 5", "MOVE", "circle 9", "ERASE"})
        h.push(l);
    CHECK(h.older("ci") == "circle 9");
    CHECK(h.older("circle 9") == "CIRCLE 5");
    CHECK(h.older("CIRCLE 5") == "CIRCLE 5");  // nothing older matches
    CHECK(h.newer("CIRCLE 5") == "circle 9");
    CHECK(h.newer("circle 9") == "ci");  // back to what was typed
    CHECK(h.older("zz") == "zz");        // no match: the box is left alone
    CHECK_FALSE(h.browsing());
}
