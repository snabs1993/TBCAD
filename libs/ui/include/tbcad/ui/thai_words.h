#pragma once

#include <span>

namespace tbcad::ui {

// A Thai word people type when they look for a command ("วงกลม" for CIRCLE). The search box and
// the command palette find the command through it. UTF-8, matched as a substring (Thai is written
// without spaces between words).
struct ThaiWord {
    const char* word;
    const char* command;
};

std::span<const ThaiWord> thaiWords();

}  // namespace tbcad::ui
