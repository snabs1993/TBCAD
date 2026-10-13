// Moves the input method's composition and candidate windows to the text cursor of the
// command line, so Thai (and any other IME) input appears where the user is typing.
#include <imgui.h>
#include <windows.h>

#include "gui.h"

namespace tbcad::app {

namespace {

void setImeData(ImGuiContext*, ImGuiViewport* viewport, ImGuiPlatformImeData* data) {
    auto hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
    if (hwnd == nullptr)
        return;
    HIMC himc = ImmGetContext(hwnd);
    if (himc == nullptr)
        return;
    if (data->WantVisible) {
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_FORCE_POSITION;
        cf.ptCurrentPos.x = static_cast<LONG>(data->InputPos.x);
        cf.ptCurrentPos.y = static_cast<LONG>(data->InputPos.y + data->InputLineHeight);
        ImmSetCompositionWindow(himc, &cf);
        CANDIDATEFORM cand{};
        cand.dwIndex = 0;
        cand.dwStyle = CFS_CANDIDATEPOS;
        cand.ptCurrentPos = cf.ptCurrentPos;
        ImmSetCandidateWindow(himc, &cand);
    }
    ImmReleaseContext(hwnd, himc);
}

}  // namespace

void installImeHook(GLFWwindow*) {
    ImGui::GetPlatformIO().Platform_SetImeDataFn = setImeData;
}

}  // namespace tbcad::app
