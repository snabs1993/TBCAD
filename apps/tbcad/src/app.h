#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/mcp/session.h"
#include "tbcad/mcpapp/controller.h"
#include "tbcad/platform/paths.h"
#include "tbcad/plugin/plugin_host.h"
#include "tbcad/render/renderer.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"
#include "viewport_target.h"

struct GLFWwindow;

namespace tbcad::app {

class Gui;

struct Options {
    std::string openFile{};       // drawing to open at start
    std::string script{};         // command lines run after start, separated by ';'
    std::string scriptFile{};     // a text file with one command line per line (# starts a comment)
    std::string screenshot{};     // write the last frame to this .ppm
    std::string report{};         // --gate: also write the report here
    std::string captureScript{};  // --capture-script F: run F offscreen, @shot lines write PNGs
    std::string
        captureOut{};  // --capture-out DIR: where shots and {out} go (default: the script's folder)
    int captureW = 1600;  // --size WxH of the hidden capture window (logical pixels)
    int captureH = 900;
    int frames = 0;           // > 0: draw this many frames, then exit
    int gateLines = 1000000;  // --gate: how many lines DEVGEN makes
    bool selfTest = false;    // hidden window, exit code 77 when no display / OpenGL
    std::string gateFile{};   // --gate-file: time open, pan/zoom and select-all on this file
    bool gate = false;        // run the performance gate and exit
    bool vsync = true;
    bool mcp = false;  // --mcp: turn the MCP server on (and remember it)
    bool help = false;
    std::string listCommands{};  // --list-commands F: write the command list (Markdown) and exit
};

Options parseOptions(int argc, char** argv);

// The program: window, renderer, Dear ImGui, the main loop.
class App {
public:
    explicit App(Options options);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run();

    // Renders `area` (world box; empty = the current view) offscreen for the MCP capture_view
    // tool. The user's view is left as it was. Owner thread only.
    std::optional<mcp::Image> captureView(int maxPx, const BBox2& area);

    // Used by the gate.
    ui::AppCore& core() { return *core_; }
    render::Renderer& renderer() { return renderer_; }
    // Draws the drawing into the viewport target (no UI) and waits for the GPU.
    void renderDrawingOnce(bool finish);
    ViewportTarget& target() { return target_; }
    GLFWwindow* window() { return window_; }

private:
    bool initWindow(std::string& why);
    void shutdownWindow();
    void runScript(const std::string& script);
    void runScriptFile(const std::string& file);
    void frame();
    void drawScene();
    void drawTiles();
    int runGate();
    int runGateFile();
    bool saveScreenshot(const std::string& path);
    int runCapture();
    void captureFrames(int n);
    bool recordItems_ = false;     // capture mode: widgets are recorded for @clickitem
    std::size_t historySeen_ = 0;  // capture mode: command-line lines already logged
    std::size_t framesDrawn_ = 0;
    // Startup timing: the time from the start of the program to the end of each phase, up to the
    // first frame. Printed (and added to the command log with TBCAD_STARTUP_TRACE=1) once drawn.
    void startupMark(const char* phase);
    void reportStartup();
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, double>> startupPhases_;
    bool startupReported_ = false;
    bool hidden() const { return options_.selfTest || !options_.captureScript.empty(); }

    Options options_;
    platform::AppPaths paths_;
    // Destroyed after the registry: the plugins' code must stay loaded as long as its commands.
    std::unique_ptr<plugin::PluginHost> plugins_;
    std::unique_ptr<cmd::Registry> registry_;
    std::unique_ptr<ui::UiRegistry> uiRegistry_;
    std::unique_ptr<ui::AppCore> core_;
    std::unique_ptr<mcpapp::McpController> mcp_;
    std::unique_ptr<Gui> gui_;
    GLFWwindow* window_ = nullptr;
    render::Renderer renderer_;
    ViewportTarget target_;
    ViewportTarget captureTarget_;
    // Pictures of recent drawings for the start page: taken when a drawing is opened or saved,
    // shown as textures by path (0 = no picture).
    void takeThumbnail();
    unsigned thumbnailTexture(const std::filesystem::path& png);
    ViewportTarget thumbTarget_;
    std::map<std::filesystem::path, unsigned> thumbTextures_;
    // The model tiled viewports (VPORTS) other than the active one, by tile index.
    std::vector<std::unique_ptr<ViewportTarget>> tileTargets_;
    std::atomic<bool> windowReady_{false};  // glfwPostEmptyEvent is only legal after glfwInit
    render::Overlay underlay_;
    render::Overlay overlay_;
    std::string iniPath_{};
    bool imguiReady_ = false;
    int framebufferW_ = 0;
    int framebufferH_ = 0;
    std::string lastTitle_{};
    double smoothedFps_ = 0;
    double lastFrameStart_ = 0;
};

/// --gate-file profiling: phase times of the frames drawn since the last call, one line.
void profileStart();
void profileReport(const char* label, double gpuWaitMs, int frames);

}  // namespace tbcad::app
