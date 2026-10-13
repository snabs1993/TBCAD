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

std::string findBesideExecutable(const std::string& name) {
    std::error_code ec;
    for (const fs::path& dir : {executableDir(), fs::current_path(ec)}) {
        fs::path p = dir / name;
        if (fs::exists(p, ec))
            return p.string();
    }
    return {};
}

Rgba backgroundFor(bool dark) {
    return dark ? Rgba{33, 40, 48, 255} : Rgba{255, 255, 255, 255};
}

void buildFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
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
    if (!base && sarabun.empty())
        io.Fonts->AddFontDefault();
    io.Fonts->Build();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

void applyTheme(bool dark, float scale) {
    ImGuiStyle style;  // fresh defaults, so repeated calls do not accumulate scaling
    if (dark)
        ImGui::StyleColorsDark(&style);
    else
        ImGui::StyleColorsLight(&style);
    style.WindowRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowPadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(8, 5);
    style.DockingSeparatorSize = 2.0f;
    ImVec4* c = style.Colors;
    if (dark) {
        c[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
        c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.13f, 1.00f);
        c[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.11f, 0.12f, 1.00f);
        c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.11f, 0.12f, 1.00f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
        c[ImGuiCol_Tab] = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
        c[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.42f, 0.66f, 1.00f);
        c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.33f, 0.52f, 1.00f);
        c[ImGuiCol_Header] = ImVec4(0.20f, 0.33f, 0.52f, 0.55f);
        c[ImGuiCol_Button] = ImVec4(0.20f, 0.23f, 0.28f, 1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.40f, 0.60f, 1.00f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.36f, 0.56f, 1.00f);
        c[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);
    } else {
        c[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.95f, 0.96f, 1.00f);
        c[ImGuiCol_ChildBg] = ImVec4(0.98f, 0.98f, 0.99f, 1.00f);
        c[ImGuiCol_MenuBarBg] = ImVec4(0.90f, 0.91f, 0.93f, 1.00f);
        c[ImGuiCol_Button] = ImVec4(0.84f, 0.87f, 0.91f, 1.00f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.66f, 0.77f, 0.92f, 1.00f);
        c[ImGuiCol_FrameBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    }
    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;
}

}  // namespace tbcad::app
