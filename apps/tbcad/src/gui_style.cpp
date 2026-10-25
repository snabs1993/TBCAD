#include <backends/imgui_impl_opengl3.h>
#include <imgui.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "gui.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace tbcad::app {

namespace fs = std::filesystem;

namespace {
ImFont* gMono = nullptr;
}  // namespace

ImFont* monoFont() {
    return gMono;
}

namespace {

fs::path executableDir() {
#if defined(_WIN32)
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
    if (n > 0 && n < 4096)
        return fs::path(buf).parent_path();
#elif defined(__linux__)
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return p.parent_path();
#endif
    return fs::current_path();
}

}  // namespace

std::string findAsset(const std::string& relative) {
    std::error_code ec;
    std::vector<fs::path> roots = {executableDir() / "assets", fs::path("assets"),
                                   fs::path(TBCAD_SOURCE_ASSETS_DIR)};
    for (const auto& root : roots) {
        fs::path p = root / relative;
        if (fs::exists(p, ec))
            return p.string();
    }
    return {};
}

std::string findSample(const std::string& name) {
    std::error_code ec;
    const fs::path source = fs::path(TBCAD_SOURCE_ASSETS_DIR).parent_path() / "samples";
    for (const fs::path& dir : {executableDir() / "samples", fs::path("samples"), source}) {
        fs::path p = dir / name;
        if (fs::exists(p, ec))
            return p.string();
    }
    return {};
}

std::string findBesideExecutable(const std::string& name) {
    std::error_code ec;
    for (const fs::path& dir : {executableDir(), fs::current_path(ec)}) {
        fs::path p = dir / name;
        if (fs::exists(p, ec))
            return p.string();
    }
    return {};
}

void buildFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendRendererUserData)  // none in the window-less GUI tests
        ImGui_ImplOpenGL3_DestroyFontsTexture();
    io.Fonts->Clear();

    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(io.Fonts->GetGlyphRangesThai());
    builder.AddText(
        "\xC2\xA9\xC2\xB0\xC2\xB1\xE2\x80\x93\xE2\x80\x94\xE2\x80\xA6");  // (c) deg +- – — …
    static ImVector<ImWchar> ranges;
    ranges.clear();
    builder.BuildRanges(&ranges);

    const float size = 15.0f * scale;
    std::string lato = findAsset("fonts/Lato-Regular.ttf");
    std::string sarabun = findAsset("fonts/Sarabun-Regular.ttf");
    ImFont* base = nullptr;
    if (!lato.empty()) {
        base = io.Fonts->AddFontFromFileTTF(lato.c_str(), size, nullptr, ranges.Data);
    }
    if (!sarabun.empty()) {
        // Thai goes in the Latin font's atlas slot when Lato is missing, otherwise it is merged.
        ImFontConfig cfg;
        cfg.MergeMode = base != nullptr;
        cfg.GlyphOffset.y = 1.0f * scale;
        static const ImWchar thai[] = {0x0E01, 0x0E5B, 0};
        io.Fonts->AddFontFromFileTTF(sarabun.c_str(), size + 1.0f * scale, &cfg,
                                     base ? thai : ranges.Data);
    }
    gMono = nullptr;
    if (std::string mono = findAsset("fonts/LiberationMono-Regular.ttf"); !mono.empty()) {
        static const ImWchar latin[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2026, 0};
        gMono = io.Fonts->AddFontFromFileTTF(mono.c_str(), size, nullptr, latin);
        if (gMono && !sarabun.empty()) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.GlyphOffset.y = 1.0f * scale;
            static const ImWchar thai[] = {0x0E01, 0x0E5B, 0};
            io.Fonts->AddFontFromFileTTF(sarabun.c_str(), size + 1.0f * scale, &cfg, thai);
        }
    }
    if (!base && sarabun.empty())
        io.Fonts->AddFontDefault();
    io.Fonts->Build();
    if (io.BackendRendererUserData)
        ImGui_ImplOpenGL3_CreateFontsTexture();
}

namespace {

ImVec4 toVec(Rgba c, double alpha = 1.0) {
    return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
                  static_cast<float>(c.a / 255.0 * alpha));
}

}  // namespace

bool systemPrefersDark() {
#if defined(_WIN32)
    DWORD value = 1;
    DWORD size = sizeof value;
    if (RegGetValueW(
            HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value == 0;
#endif
    return true;
}

void applyTheme(const ui::Theme& t, float scale) {
    ImGuiStyle style;  // fresh defaults, so repeated calls do not accumulate scaling
    style.WindowRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.FrameBorderSize =
        1.0f;  // an empty check box or field must still read as one on a dark panel
    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(6, 6);
    style.DockingSeparatorSize = 2.0f;

    const Rgba frame = ui::mix(t.bgPanel, t.bgViewport, 0.6);
    const Rgba hover = ui::mix(t.bgPanel, t.accent, 0.35);
    const Rgba active = ui::mix(t.bgPanel, t.accent, 0.55);
    const Rgba button = ui::mix(t.bgPanel, t.text, 0.10);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = toVec(t.text);
    c[ImGuiCol_TextDisabled] = toVec(t.textMuted);
    c[ImGuiCol_WindowBg] = toVec(t.bgPanel);
    c[ImGuiCol_ChildBg] = toVec(t.bgPanel, 0.0);
    c[ImGuiCol_PopupBg] = toVec(t.bgPanel);
    c[ImGuiCol_Border] = toVec(ui::mix(t.border, t.text, 0.12));
    c[ImGuiCol_BorderShadow] = toVec(t.border, 0.0);
    c[ImGuiCol_FrameBg] = toVec(frame);
    c[ImGuiCol_FrameBgHovered] = toVec(hover, 0.6);
    c[ImGuiCol_FrameBgActive] = toVec(active, 0.7);
    c[ImGuiCol_TitleBg] = toVec(t.bgApp);
    c[ImGuiCol_TitleBgActive] = toVec(t.bgApp);
    c[ImGuiCol_TitleBgCollapsed] = toVec(t.bgApp);
    c[ImGuiCol_MenuBarBg] = toVec(t.bgApp);
    c[ImGuiCol_ScrollbarBg] = toVec(t.bgPanel, 0.0);
    c[ImGuiCol_ScrollbarGrab] = toVec(t.border);
    c[ImGuiCol_ScrollbarGrabHovered] = toVec(t.textMuted);
    c[ImGuiCol_ScrollbarGrabActive] = toVec(t.accent);
    c[ImGuiCol_CheckMark] = toVec(t.accent);
    c[ImGuiCol_SliderGrab] = toVec(t.accent);
    c[ImGuiCol_SliderGrabActive] = toVec(t.select);
    c[ImGuiCol_Button] = toVec(button);
    c[ImGuiCol_ButtonHovered] = toVec(hover);
    c[ImGuiCol_ButtonActive] = toVec(active);
    c[ImGuiCol_Header] = toVec(t.accent, 0.35);
    c[ImGuiCol_HeaderHovered] = toVec(t.accent, 0.5);
    c[ImGuiCol_HeaderActive] = toVec(t.accent, 0.65);
    c[ImGuiCol_Separator] = toVec(t.border);
    c[ImGuiCol_SeparatorHovered] = toVec(t.accent);
    c[ImGuiCol_SeparatorActive] = toVec(t.select);
    c[ImGuiCol_ResizeGrip] = toVec(t.border);
    c[ImGuiCol_ResizeGripHovered] = toVec(t.accent);
    c[ImGuiCol_ResizeGripActive] = toVec(t.select);
    c[ImGuiCol_Tab] = toVec(t.bgApp);
    c[ImGuiCol_TabHovered] = toVec(hover);
    c[ImGuiCol_TabSelected] = toVec(t.bgPanel);
    c[ImGuiCol_TabSelectedOverline] = toVec(t.accent);
    c[ImGuiCol_TabDimmed] = toVec(t.bgApp);
    c[ImGuiCol_TabDimmedSelected] = toVec(t.bgPanel);
    c[ImGuiCol_TabDimmedSelectedOverline] = toVec(t.border);
    c[ImGuiCol_DockingPreview] = toVec(t.accent, 0.5);
    c[ImGuiCol_DockingEmptyBg] = toVec(t.bgViewport);
    c[ImGuiCol_TextSelectedBg] = toVec(t.accent, 0.4);
    c[ImGuiCol_NavCursor] = toVec(t.accent);
    c[ImGuiCol_DragDropTarget] = toVec(t.snap);
    c[ImGuiCol_TableHeaderBg] = toVec(t.bgApp);
    c[ImGuiCol_TableBorderStrong] = toVec(t.border);
    c[ImGuiCol_TableBorderLight] = toVec(t.border, 0.6);
    c[ImGuiCol_TableRowBgAlt] = toVec(t.text, 0.03);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;
}

}  // namespace tbcad::app
