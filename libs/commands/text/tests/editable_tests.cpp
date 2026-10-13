#include <doctest/doctest.h>

#include "tbcad/textcmd/textcmd.h"

using namespace tbcad::textcmd;

TEST_CASE("editor form: paragraphs become newlines and back, other markup is untouched") {
    CHECK(markupToEditable("one\\Ptwo") == "one\ntwo");
    CHECK(markupToEditable("{\\b1;bold}\\P{\\C1;red}") == "{\\b1;bold}\n{\\C1;red}");
    CHECK(markupToEditable("a\\\\Pb") == "a\\\\Pb");  // an escaped backslash followed by P
    CHECK(editableToMarkup("one\ntwo") == "one\\Ptwo");
    CHECK(editableToMarkup("x\r\ny") == "x\\Py");
    CHECK(editableToMarkup("a\\\\Pb") == "a\\\\Pb");
    const std::string m = "{\\i1;slanted}\\Pnext \\{ brace \\}";
    CHECK(editableToMarkup(markupToEditable(m)) == m);
}
