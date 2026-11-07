#include <doctest/doctest.h>

#include "tbcad/ui/command_keys.h"

using namespace tbcad::ui;

TEST_CASE("Thai Kedmanee letters read as the English key in the same place") {
    const char* th = "thai-kedmanee";
    CHECK(toCommandKeys(th, "ส", false) == "l");          // LINE
    CHECK(toCommandKeys(th, "แ", false) == "c");          // CIRCLE
    CHECK(toCommandKeys(th, "ไำพ", false) == "wer");
    CHECK(toCommandKeys(th, "ๅจจ", false) == "100");      // numbers on the top row
    CHECK(toCommandKeys(th, "๑ๅจจมจ", false) == "@100,0");  // Shift+2 is @, the comma key is ม
    CHECK(toCommandKeys(th, "ฉ", false) == "C");          // Shift gives the capital
    CHECK(toCommandKeys(th, "ใ", false) == ".");
}

TEST_CASE("ASCII the Thai layout puts on other keys moves only while that layout types") {
    const char* th = "thai-kedmanee";
    // "/" and "-" are on the 2 and 3 keys in Kedmanee.
    CHECK(toCommandKeys(th, "/จจจ", true) == "2000");
    CHECK(toCommandKeys(th, "-", true) == "3");
    // Typed on an English keyboard, they stay.
    CHECK(toCommandKeys(th, "/2,-3", false) == "/2,-3");
    CHECK(toCommandKeys(th, "L 100", true) == "L 100");  // letters and digits are not Thai keys
}

TEST_CASE("Off and unknown layouts leave the text alone") {
    CHECK(toCommandKeys("off", "สแ/", true) == "สแ/");
    CHECK(toCommandKeys("nope", "ส", true) == "ส");
    REQUIRE(commandKeyboards().size() >= 2);
    CHECK(std::string(commandKeyboards()[0].id) == "off");
}
