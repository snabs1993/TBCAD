#include "app.h"

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <glad/gl.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "gui.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/dwgopen/dwgopen.h"
#include "tbcad/platform/crash.h"
#include "tbcad/platform/paths.h"
#include "tbcad/render/camera.h"

namespace tbcad::app {

namespace {

void printUsage() {
    std::puts("TBCAD " TBCAD_VERSION
              "\n"
              "Usage: tbcad [file.tbcad|file.dxf] [options]\n"
              "  --script \"CMD;CMD\"   run command lines after start\n"
              "  --script-file F      run the command lines in text file F (one per line, # = "
              "comment)\n"
              "  --frames N           draw N frames, then exit\n"
              "  --screenshot F.ppm   save the last frame\n"
              "  --gate [--report F]  run the performance gate (1 M lines) and exit\n"
              "  --gate-file F        time open, pan/zoom and select-all on drawing F, then exit\n"
              "  --gate-lines N       use N lines for the gate instead of 1 000 000\n"
              "  --list-commands F    write every command with its aliases (Markdown) to F "
              "(default: stdout)\n"
              "  --self-test          hidden window; exit code 77 when no display or OpenGL 3.3\n"
              "  --mcp                turn the MCP server on (AI access to the drawing)\n"
              "  --no-vsync           do not wait for the monitor\n");
}

void glfwError(int code, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, description);
}

}  // namespace

Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (a == "--script")
            o.script = next();
        else if (a == "--script-file")
            o.scriptFile = next();
        else if (a == "--frames")
            o.frames = std::atoi(next().c_str());
        else if (a == "--screenshot")
            o.screenshot = next();
        else if (a == "--report")
            o.report = next();
        else if (a == "--gate")
            o.gate = true;
        else if (a == "--gate-file")
            o.gateFile = next();
        else if (a == "--gate-lines")
            o.gateLines = std::max(1000, std::atoi(next().c_str()));
        else if (a == "--self-test")
            o.selfTest = true;
        else if (a == "--list-commands") {
            // No file name (or the next argument is another option): print to stdout.
            const bool hasValue = i + 1 < argc && (argv[i + 1][0] != '-' || argv[i + 1][1] == '\0');
            o.listCommands = hasValue ? next() : "-";
        } else if (a == "--mcp")
            o.mcp = true;
        else if (a == "--no-vsync")
            o.vsync = false;
        else if (a == "--help" || a == "-h")
            o.help = true;
        else if (!a.empty() && a[0] != '-')
            o.openFile = a;
    }
    return o;
}

App::App(Options options) : options_(std::move(options)) {}

App::~App() {
    shutdownWindow();
}

bool App::initWindow(std::string& why) {
    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) {
        why = "could not initialise GLFW (no display?)";
        return false;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, options_.selfTest ? GLFW_FALSE : GLFW_TRUE);
    window_ = glfwCreateWindow(1440, 900, "TBCAD", nullptr, nullptr);
    if (!window_) {
        why = "could not create an OpenGL 3.3 window";
        return false;
    }
    // Keep the window inside the usable screen (not behind the taskbar): scaled to a high-DPI
    // monitor 1440x900 can be larger than the work area.
    if (!options_.selfTest && !options_.gate) {
        int wx = 0, wy = 0, ww = 0, wh = 0;
        glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &wx, &wy, &ww, &wh);
        int fl = 0, ft = 0, fr = 0, fb = 0;
        glfwGetWindowFrameSize(window_, &fl, &ft, &fr, &fb);
        int cw = 0, ch = 0;
        glfwGetWindowSize(window_, &cw, &ch);
        if (ww > 0 && wh > 0) {
            if (cw + fl + fr > ww || ch + ft + fb > wh) {
                glfwMaximizeWindow(window_);
            } else {
                glfwSetWindowPos(window_, wx + std::max(fl, (ww - cw - fl - fr) / 2),
                                 wy + std::max(ft, (wh - ch - ft - fb) / 2));
            }
        }
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(options_.vsync && !options_.gate ? 1 : 0);

    std::string err;
    if (!renderer_.init(reinterpret_cast<render::GLLoadProc>(glfwGetProcAddress), &err)) {
        why = "renderer: " + err;
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingWithShift = false;
    iniPath_ = (paths_.configDir / "imgui.ini").string();
    platform::ensureDirectory(paths_.configDir);
    io.IniFilename = options_.selfTest ? nullptr : iniPath_.c_str();
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    imguiReady_ = true;
    windowReady_ = true;
    return true;
}

void App::shutdownWindow() {
    windowReady_ = false;
    gui_.reset();
    if (imguiReady_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiReady_ = false;
    }
    target_.release();
    if (window_) {
        renderer_.shutdown();
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
}

void App::runScript(const std::string& script) {
    std::size_t start = 0;
    while (start <= script.size()) {
        std::size_t end = script.find(';', start);
        std::string line =
            script.substr(start, end == std::string::npos ? std::string::npos : end - start);
        core_->submitText(line);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
}

void App::runScriptFile(const std::string& file) {
    std::ifstream in(platform::pathFromUtf8(file));
    if (!in) {
        std::fprintf(stderr, "tbcad: cannot read script file %s\n", file.c_str());
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line.front() == '#')
            continue;
        core_->submitText(line);  // an empty line is Enter
    }
}

namespace {
// TBCAD_PROFILE=1 prints where each frame spends its time (milliseconds, averaged over 60 frames).
struct PhaseTimer {
    static bool& forced() {
        static bool f = false;
        return f;
    }
    static bool enabled() {
        static const bool on = platform::processEnvironment("TBCAD_PROFILE").has_value();
        return on || forced();
    }
    double t0 = glfwGetTime();
    double lap() {
        double t = glfwGetTime();
        double d = (t - t0) * 1000.0;
        t0 = t;
        return d;
    }
};
double gPhase[8] = {};
int gPhaseFrames = 0;
}  // namespace

void profileStart() {
    PhaseTimer::forced() = true;
    for (double& v : gPhase)
        v = 0;
    gPhaseFrames = 0;
}

void profileReport(const char* label, double gpuWaitMs, int frames) {
    const double n = frames > 0 ? frames : 1;
    if (label)
        std::printf(
            "  %-26s update %.2f  ui %.2f  composite %.2f  scene.draw %.2f  overlay.build %.2f  "
            "overlay.draw %.2f  underlay %.2f  gpu wait %.2f\n",
            label, gPhase[0] / n, gPhase[1] / n, gPhase[2] / n, gPhase[3] / n, gPhase[4] / n,
            gPhase[5] / n, gPhase[6] / n, gpuWaitMs / n);
    for (double& v : gPhase)
        v = 0;
    gPhaseFrames = 0;
}

void App::drawScene() {
    PhaseTimer pt;
    const render::Camera2D& cam = core_->camera();
    target_.resize(cam.viewportW, cam.viewportH);
    target_.bind();
    renderer_.setBackground(gui_->viewportBackground());
    renderer_.setLineweightDisplay(gui_->lineweights());
    renderer_.setCamera(cam);
    renderer_.beginFrame();
    renderer_.draw(core_->underlayImages());
    core_->buildUnderlay(underlay_, uiRegistry_.get());
    renderer_.draw(underlay_);
    if (PhaseTimer::enabled())
        gPhase[6] += pt.lap();
    renderer_.draw(core_->renderScene());
    if (PhaseTimer::enabled())
        gPhase[3] += pt.lap();
    core_->buildOverlay(overlay_, uiRegistry_.get());
    if (PhaseTimer::enabled())
        gPhase[4] += pt.lap();
    renderer_.draw(overlay_);
    renderer_.endFrame();
    if (PhaseTimer::enabled())
        gPhase[5] += pt.lap();
}

void App::renderDrawingOnce(bool finish) {
    const render::Camera2D& cam = core_->camera();
    target_.resize(cam.viewportW, cam.viewportH);
    target_.bind();
    renderer_.setBackground(ui::darkTheme().bgViewport);
    renderer_.setCamera(cam);
    renderer_.beginFrame();
    renderer_.draw(core_->underlayImages());
    renderer_.draw(core_->renderScene());
    renderer_.endFrame();
    if (finish)
        glFinish();
}

void App::frame() {
    PhaseTimer pt;
    core_->updateFrame();
    if (PhaseTimer::enabled())
        gPhase[0] += pt.lap();

    const std::string title = core_->windowTitle();
    if (title != lastTitle_) {
        glfwSetWindowTitle(window_, title.c_str());
        lastTitle_ = title;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ViewportRect rect = gui_->frame();
    mcp_->drawDialogs();
    if (mcp_->consumeSettingsRequest())
        gui_->openPanel("mcp");
    const ui::AppCore& c = *core_;
    target_.resize(c.camera().viewportW, c.camera().viewportH);
    gui_->showViewport(target_.texture(), rect);
    ImGui::Render();
    if (PhaseTimer::enabled())
        gPhase[1] += pt.lap();

    drawScene();
    if (PhaseTimer::enabled())
        pt.lap();

    glfwGetFramebufferSize(window_, &framebufferW_, &framebufferH_);
    ViewportTarget::unbindToWindow(framebufferW_, framebufferH_);
    const Rgba bg = gui_->theme().bgApp;
    glClearColor(bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (PhaseTimer::enabled()) {
        gPhase[2] += pt.lap();
        if (!PhaseTimer::forced() && ++gPhaseFrames == 60) {
            const auto& st = renderer_.stats();
            std::fprintf(stderr,
                         "profile ms/frame: update %.2f  ui %.2f  composite %.2f  scene.draw %.2f  "
                         "overlay.build %.2f  overlay.draw %.2f  (draw calls %u, verts %llu)\n",
                         gPhase[0] / 60, gPhase[1] / 60, gPhase[2] / 60, gPhase[3] / 60,
                         gPhase[4] / 60, gPhase[5] / 60, st.drawCalls,
                         static_cast<unsigned long long>(st.vertices));
            for (double& v : gPhase)
                v = 0;
            gPhaseFrames = 0;
        }
    }
}

bool App::saveScreenshot(const std::string& path) {
    if (framebufferW_ <= 0 || framebufferH_ <= 0)
        return false;
    std::vector<unsigned char> px(static_cast<std::size_t>(framebufferW_) * framebufferH_ * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, framebufferW_, framebufferH_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out << "P6\n" << framebufferW_ << " " << framebufferH_ << "\n255\n";
    for (int y = framebufferH_ - 1; y >= 0; --y)
        for (int x = 0; x < framebufferW_; ++x) {
            const unsigned char* p = &px[(static_cast<std::size_t>(y) * framebufferW_ + x) * 4];
            out.put(static_cast<char>(p[0]))
                .put(static_cast<char>(p[1]))
                .put(static_cast<char>(p[2]));
        }
    return static_cast<bool>(out);
}

std::optional<mcp::Image> App::captureView(int maxPx, const BBox2& area) {
    if (!window_ || !core_)
        return std::nullopt;
    const render::Camera2D saved = core_->camera();
    const BBox2 box = area.empty() ? saved.visibleWorld() : area;
    const double w = box.max.x - box.min.x;
    const double h = box.max.y - box.min.y;
    if (!(w > 0) || !(h > 0))
        return std::nullopt;
    maxPx = std::clamp(maxPx, 16, 4096);
    const int pw = w >= h ? maxPx : std::max(1, static_cast<int>(std::lround(maxPx * w / h)));
    const int ph = w >= h ? std::max(1, static_cast<int>(std::lround(maxPx * h / w))) : maxPx;

    render::Camera2D cam = saved;
    cam.viewportW = pw;
    cam.viewportH = ph;
    cam.center = {(box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2};
    cam.pixelsPerMm = std::min(pw / w, ph / h);
    // Curves are tessellated for the zoom, so let the core do it for the capture's zoom first.
    core_->setCamera(cam);
    core_->updateFrame();

    captureTarget_.resize(pw, ph);
    captureTarget_.bind();
    renderer_.setBackground(gui_ ? gui_->viewportBackground() : ui::darkTheme().bgViewport);
    renderer_.setCamera(cam);
    renderer_.beginFrame();
    renderer_.draw(core_->underlayImages());
    renderer_.draw(core_->renderScene());
    renderer_.endFrame();
    glFinish();

    mcp::Image img;
    img.width = pw;
    img.height = ph;
    std::vector<std::uint8_t> bottomUp(static_cast<std::size_t>(pw) * ph * 4);
    const bool ok = captureTarget_.read(bottomUp.data());
    img.rgba.resize(bottomUp.size());
    for (int y = 0; y < ph; ++y)
        std::copy_n(&bottomUp[static_cast<std::size_t>(ph - 1 - y) * pw * 4],
                    static_cast<std::size_t>(pw) * 4,
                    &img.rgba[static_cast<std::size_t>(y) * pw * 4]);
    img.worldBox = cam.visibleWorld();
    img.mmPerPixel = cam.pixelSizeMm();

    ViewportTarget::unbindToWindow(framebufferW_, framebufferH_);
    core_->setCamera(saved);
    core_->updateFrame();
    if (!ok)
        return std::nullopt;
    return img;
}

int App::run() {
    if (options_.help) {
        printUsage();
        return 0;
    }
    paths_ = platform::defaultAppPaths();
    platform::installCrashHandler(paths_.crashDir(), TBCAD_VERSION);
    registry_ = std::make_unique<cmd::Registry>();
    registerAllCommands(*registry_);
    core_ = std::make_unique<ui::AppCore>(*registry_, paths_);
    dwgopen::registerForeignFormats(*core_);
    if (!options_.listCommands.empty()) {
        const std::string table = cmd::commandListMarkdown(*registry_);
        if (options_.listCommands == "-") {
            std::fputs(table.c_str(), stdout);
            return 0;
        }
        std::ofstream out(options_.listCommands, std::ios::binary | std::ios::trunc);
        out << table;
        out.flush();
        if (!out) {
            std::fprintf(stderr, "tbcad: cannot write %s\n", options_.listCommands.c_str());
            return 1;
        }
        return 0;
    }
    {
        mcpapp::McpController::Hooks hooks;
        hooks.capture = [this](int maxPx, const BBox2& area) { return captureView(maxPx, area); };
        hooks.wake = [this] {
            if (windowReady_)
                glfwPostEmptyEvent();
        };
#ifdef _WIN32
        hooks.bridgeExe = platform::pathFromUtf8(findBesideExecutable("tbcad-mcp.exe"));
#else
        hooks.bridgeExe = platform::pathFromUtf8(findBesideExecutable("tbcad-mcp"));
#endif
        mcp_ = std::make_unique<mcpapp::McpController>(*core_, std::move(hooks), TBCAD_VERSION);
        mcp_->registerCommands(*registry_);
        if (options_.mcp)
            core_->settings().setBool(mcpapp::kSettingEnabled, true);
    }
    // Plugins: shared libraries in <settings folder>/plugins that add commands
    // (docs/plugin-api.md).
    plugins_ = std::make_unique<plugin::PluginHost>();
    const std::filesystem::path pluginFolder = paths_.configDir / "plugins";
    if (core_->settings().getBool("plugins.enabled", true))
        plugins_->loadFolder(pluginFolder, *registry_);
    plugins_->registerListCommand(*registry_, pluginFolder);
    for (const std::string& line : plugins_->takeLog())
        core_->print(line);
    uiRegistry_ = std::make_unique<ui::UiRegistry>();
    registerAllUi(*uiRegistry_);
    plugins_->registerUi(*uiRegistry_);
    mcp_->registerUi(*uiRegistry_);
    core_->registerBuiltinUi(*uiRegistry_);
    core_->applyUiKeybindings(*uiRegistry_);

    std::string why;
    if (!initWindow(why)) {
        std::fprintf(stderr, "tbcad: %s\n", why.c_str());
        // 77 = skipped for ctest, unless the CI job says a display must be there.
        const bool required = platform::processEnvironment("TBCAD_REQUIRE_GUI").has_value();
        return options_.selfTest && !required ? 77 : 1;
    }
    gui_ = std::make_unique<Gui>(*core_, *uiRegistry_, window_);
    {
        const float total =
            gui_->dpiScale() * static_cast<float>(core_->settings().getNumber("ui.scale", 1.0));
        buildFonts(total);
        applyTheme(gui_->theme(), total);
    }
    glfwSetWindowUserPointer(window_, gui_.get());
    glfwSetDropCallback(window_, [](GLFWwindow* w, int count, const char** paths) {
        auto* gui = static_cast<Gui*>(glfwGetWindowUserPointer(w));
        for (int i = 0; i < count; ++i)
            gui->queueDroppedFile(paths[i]);
    });
    glfwSetWindowContentScaleCallback(window_, [](GLFWwindow* w, float xs, float) {
        static_cast<Gui*>(glfwGetWindowUserPointer(w))->setDpiScale(xs);
    });

    if (options_.gate)
        return runGate();
    if (!options_.gateFile.empty())
        return runGateFile();

    // Give the viewport a size before the first command runs (DEVGEN fits the view).
    {
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        core_->setViewportSize(std::max(1, w - 600), std::max(1, h - 200));
    }
    if (!options_.openFile.empty())
        (void)core_->openFile(platform::pathFromUtf8(options_.openFile));
    if (!options_.script.empty())
        runScript(options_.script);
    if (!options_.scriptFile.empty())
        runScriptFile(options_.scriptFile);

    int framesDrawn = 0;
    double last = glfwGetTime();
    while (!glfwWindowShouldClose(window_) && !gui_->wantsExit()) {
        const bool scripted = options_.frames > 0;
        if (scripted || gui_->consumeActivity())
            glfwPollEvents();
        else
            glfwWaitEventsTimeout(0.5);
        if (glfwWindowShouldClose(window_) && !gui_->requestClose())
            glfwSetWindowShouldClose(window_, GLFW_FALSE);

        mcp_->pump();
        frame();
        ++framesDrawn;
        if (scripted && framesDrawn >= options_.frames) {
            if (!options_.screenshot.empty() && !saveScreenshot(options_.screenshot))
                std::fprintf(stderr, "tbcad: cannot write %s\n", options_.screenshot.c_str());
            break;
        }
        glfwSwapBuffers(window_);

        double now = glfwGetTime();
        double dt = std::max(1e-4, now - last);
        last = now;
        smoothedFps_ = smoothedFps_ == 0 ? 1.0 / dt : smoothedFps_ * 0.9 + (1.0 / dt) * 0.1;
        gui_->setFps(smoothedFps_, dt * 1000.0);
    }

    int status = 0;
    if (options_.selfTest) {
        // The smoke test: the scripted drawing must have been built and drawn.
        const auto& stats = renderer_.stats();
        if (core_->scene().entityCount() == 0 || stats.drawCalls == 0) {
            std::fprintf(stderr, "tbcad: self-test failed (entities=%zu draw calls=%u)\n",
                         core_->scene().entityCount(), stats.drawCalls);
            status = 1;
        } else {
            std::printf("self-test ok: %zu entities, %u draw calls, %llu vertices\n",
                        core_->scene().entityCount(), stats.drawCalls,
                        static_cast<unsigned long long>(stats.vertices));
        }
    }
    mcp_.reset();
    core_->shutdown();
    return status;
}

}  // namespace tbcad::app
