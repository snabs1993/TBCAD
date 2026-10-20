#include <doctest/doctest.h>

#include "tbcad/text/text.h"
#include "tbcad/textcmd/format_edits.h"
#include "tbcad/textcmd/textcmd.h"

using namespace tbcad;
using namespace tbcad::textcmd;

TEST_CASE("formatting wraps the selection, or the cursor, and stacks fractions") {
    TextSel s{"plan view", 0, 4};
    s = wrapSelection(s, "{\\b1;", "}");
    CHECK(s.text == "{\\b1;plan} view");
    CHECK(s.text.substr(s.start, s.end - s.start) == "plan");
    TextSel c{"ab", 1, 1};
    c = wrapSelection(c, "{\\L", "}");
    CHECK(c.text == "a{\\L}b");
    CHECK(c.start == 4);  // typing goes inside

    TextSel f{"width 3/4 in", 6, 9};
    REQUIRE(stackSelection(f));
    CHECK(f.text == "width \\S3/4; in");
    TextSel g{"plain", 0, 5};
    CHECK_FALSE(stackSelection(g));
    CHECK(g.text == "plain");

    // The stacked fraction is one piece when parsed.
    const auto pieces = text::parseMarkup(editableToMarkup(f.text), 2.5);
    bool stacked = false;
    for (const auto& p : pieces)
        stacked = stacked || (p.stacked && p.num == "3" && p.den == "4");
    CHECK(stacked);
}

TEST_CASE("bullets, numbers and letters make a list with a hanging indent, Off undoes it") {
    TextSel s{"Notes:\nfirst\nsecond\nend", 7, 19};  // "first" and "second"
    s = makeList(s, ListKind::Numbers, 5);
    CHECK(s.text == "Notes:\n\\pxi-5,l5,t5;1.\tfirst\n2.\tsecond\n\\pxi0,l0;end");
    // Bullets over the same lines replace the numbers.
    s = makeList(s, ListKind::Bullets, 5);
    CHECK(s.text ==
          "Notes:\n\\pxi-5,l5,t5;\xE2\x80\xA2\tfirst\n\xE2\x80\xA2\tsecond\n\\pxi0,l0;end");
    s = makeList(s, ListKind::Letters, 5);
    CHECK(s.text.find("a.\tfirst\nb.\tsecond") != std::string::npos);
    s = makeList(s, ListKind::Off, 5);
    CHECK(s.text == "Notes:\nfirst\nsecond\n\\pxi0,l0;end");

    // The list parses into paragraphs with the hanging indent.
    TextSel one{"a\nb", 0, 3};
    one = makeList(one, ListKind::Bullets, 4);
    const auto pieces = text::parseMarkup(editableToMarkup(one.text), 2.5);
    bool indented = false;
    for (const auto& p : pieces)
        if (p.para)
            indented = indented || (p.para->firstIndent == -4 && p.para->leftIndent == 4 &&
                                    p.para->tabs == std::vector<double>{4});
    CHECK(indented);
}

TEST_CASE("paragraph codes go at the start of the cursor's paragraph and replace the old one") {
    CHECK(paragraphCode(0, 0, 0, {}, 0, 0, "") == "");
    CHECK(paragraphCode(-3, 3, 0, {10, 20}, 1, 0, "c") == "\\pxi-3,l3,qc,t10,20,sb1;");
    TextSel s{"one\ntwo", 5, 5};
    s = setParagraphCode(s, "\\pxqc;");
    CHECK(s.text == "one\n\\pxqc;two");
    CHECK(s.start == 11);
    s = setParagraphCode(s, "\\pxqr;");
    CHECK(s.text == "one\n\\pxqr;two");
    s = setParagraphCode(s, "");
    CHECK(s.text == "one\ntwo");
}

TEST_CASE("find and replace inside the text") {
    std::size_t n = 0;
    CHECK(replaceAll("Wall wall WALL", "wall", "Door", false, n) == "Door Door Door");
    CHECK(n == 3);
    CHECK(replaceAll("Wall wall", "wall", "x", true, n) == "Wall x");
    CHECK(n == 1);
    CHECK(replaceAll("ผนัง ผนัง", "ผนัง", "ประตู", true, n) == "ประตู ประตู");
    CHECK(n == 2);
}
