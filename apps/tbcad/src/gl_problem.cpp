#include "gl_problem.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace tbcad {

void showOpenGlProblem(const std::string& text) {
#ifdef _WIN32
    MessageBoxA(nullptr, text.c_str(), "TBCAD cannot start", MB_OK | MB_ICONERROR);
#else
    (void)text;
#endif
}

}  // namespace tbcad
