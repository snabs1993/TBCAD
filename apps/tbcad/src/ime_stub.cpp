#include "gui.h"

namespace tbcad::app {

// The candidate window of an input method follows the text cursor only on Windows for now.
void installImeHook(GLFWwindow*) {}

}  // namespace tbcad::app

namespace tbcad::app {

bool commandKeyboardTyping(std::string_view) { return false; }

}  // namespace tbcad::app
