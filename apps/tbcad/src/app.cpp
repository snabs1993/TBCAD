#include "app.h"

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <glad/gl.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "gui.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/platform/paths.h"

namespace tbcad::app {

namespace {

void printUsage() {
    std::puts("TBCAD " TBCAD_VERSION
              "\n"
              "Usage: tbcad [file.tbcad] [options]\n"
              "  --script \"CMD;CMD\"   run command lines after start\n"
              "  --frames N           draw N frames, then exit\n"
              "  --screenshot F.ppm   save the last frame\n"
              "  --gate [--report F]  run the performance gate (1 M lines) and exit\n"
              "  --gate-lines N       use N lines for the gate instead of 1 000 000\n"
              "  --list-commands F    write every command with its aliases (Markdown) to F "
              "(default: stdout)\n"
              "  --self-test          hidden window; exit code 77 when no display or OpenGL 3.3\n"
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
        else if (a == "--frames")
            o.frames = std::atoi(next().c_str());
        else if (a == "--screenshot")
            o.screenshot = next();
        else if (a == "--report")
            o.report = next();
        else if (a == "--gate")
            o.gate = true;
        else if (a == "--self-test")
            o.selfTest = true;
        else if (a == "--list-commands") {
            // No file name (or the next argument is another option): print to stdout.
            const bool hasValue = i + 1 < argc && (argv[i + 1][0] != '-' || argv[i + 1][1] == '\0');
            o.listCommands = hasValue ? next() : "-";
        } else if (a == "--no-vsync")
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
    return true;
}

void App::shutdownWindow() {
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

void App::drawScene() {
    const render::Camera2D& cam = core_->camera();
    target_.resize(cam.viewportW, cam.viewportH);
    target_.bind();
    renderer_.setBackground(backgroundFor(gui_->dark()));
    renderer_.setLineweightDisplay(gui_->lineweights());
    renderer_.setCamera(cam);
    renderer_.beginFrame();
    renderer_.draw(core_->scene());
    core_->buildOverlay(overlay_, uiRegistry_.get());
    renderer_.draw(overlay_);
    renderer_.endFrame();
}

void App::renderDrawingOnce(bool finish) {
    const render::Camera2D& cam = core_->camera();
    target_.resize(cam.viewportW, cam.viewportH);
    target_.bind();
    renderer_.setBackground(backgroundFor(true));
    renderer_.setCamera(cam);
    renderer_.beginFrame();
    renderer_.draw(core_->scene());
    renderer_.endFrame();
    if (finish)
        glFinish();
}

void App::frame() {
    core_->updateFrame();

    const std::string title = core_->windowTitle();
    if (title != lastTitle_) {
        glfwSetWindowTitle(window_, title.c_str());
        lastTitle_ = title;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ViewportRect rect = gui_->frame();
    const ui::AppCore& c = *core_;
    target_.resize(c.camera().viewportW, c.camera().viewportH);
    gui_->showViewport(target_.texture(), rect);
    ImGui::Render();

    drawScene();

    glfwGetFramebufferSize(window_, &framebufferW_, &framebufferH_);
    ViewportTarget::unbindToWindow(framebufferW_, framebufferH_);
    const Rgba bg = backgroundFor(gui_->dark());
    glClearColor(bg.r / 255.0f * 0.8f, bg.g / 255.0f * 0.8f, bg.b / 255.0f * 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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

int App::run() {
    if (options_.help) {
        printUsage();
        return 0;
    }
    paths_ = platform::defaultAppPaths();
    registry_ = std::make_unique<cmd::Registry>();
    registerAllCommands(*registry_);
    core_ = std::make_unique<ui::AppCore>(*registry_, paths_);
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
    uiRegistry_ = std::make_unique<ui::UiRegistry>();
    registerAllUi(*uiRegistry_);
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
        applyTheme(gui_->dark(), total);
    }
    glfwSetWindowUserPointer(window_, gui_.get());
    glfwSetWindowContentScaleCallback(window_, [](GLFWwindow* w, float xs, float) {
        static_cast<Gui*>(glfwGetWindowUserPointer(w))->setDpiScale(xs);
    });

    if (options_.gate)
        return runGate();

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
    core_->shutdown();
    return status;
}

}  // namespace tbcad::app
