#pragma once

#include <span>
#include <string_view>

namespace tbcad::ui {

// A Thai word people type when they look for a command ("วงกลม" for CIRCLE). The search box and
// the command palette find the command through it. UTF-8, matched as a substring (Thai is written
// without spaces between words).
struct ThaiWord {
    const char* word;
    const char* command;
};

std::span<const ThaiWord> thaiWords();

// The command a whole Thai word stands for ("เส้น" -> "LINE"); nullptr when it is not one. Typed
// on the command line and confirmed with Enter, such a word starts its command.
const char* commandForThaiWord(std::string_view word);

}  // namespace tbcad::ui
