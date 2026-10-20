#pragma once

// The editing steps behind the MTEXT formatting bar (in the in-place editor and the Text panel),
// kept free of the user interface so they can be tested. They work on the editable form of the
// markup (see markupToEditable): a newline is a paragraph break, every other code is as written.
// `start` and `end` are byte offsets of the selection (equal when nothing is selected).

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tbcad::textcmd {

struct TextSel {
    std::string text;
    std::size_t start = 0, end = 0;
};

// Puts `open` before and `close` after the selection, which stays selected. With nothing
// selected both go in at the cursor, which ends up between them.
TextSel wrapSelection(TextSel s, std::string_view open, std::string_view close);
// Replaces the selection (or inserts at the cursor) with `text`; the cursor goes after it.
TextSel insertText(TextSel s, std::string_view text);
// "1/2", "1#2" or "1^2" selected becomes the stacked fraction \S1/2; (the separator is kept).
// False (and `s` unchanged) when the selection holds no separator.
bool stackSelection(TextSel& s);

enum class ListKind { Off, Bullets, Numbers, Letters };
// Turns the paragraphs the selection touches into a list: each starts with "•", "1." or "a."
// and a tab, the first gets a hanging indent of `indent` drawing units (a \p code) and the
// paragraph after the list gets the indent back to none. Off takes the marks and indent away.
TextSel makeList(TextSel s, ListKind kind, double indent);
// Puts the paragraph code `code` ("\pqc;", "\pxi-3,l3,t3;") at the start of the paragraph that
// holds the cursor, replacing the \p code already there.
TextSel setParagraphCode(TextSel s, std::string_view code);
// The \p code for these settings (lengths in drawing units); align is one of "", "l", "c", "r",
// "j". Empty when everything is at its default.
std::string paragraphCode(double firstIndent, double leftIndent, double rightIndent,
                          const std::vector<double>& tabs, double spaceBefore, double spaceAfter,
                          std::string_view align);
// Every occurrence of `find` replaced by `with`; `count` says how many.
std::string replaceAll(std::string_view text, std::string_view find, std::string_view with,
                       bool matchCase, std::size_t& count);

}  // namespace tbcad::textcmd
