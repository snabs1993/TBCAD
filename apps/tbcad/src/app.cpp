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
#include <filesystem>
#include <fstream>
#include <sstream>

#include "gui.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/dwgopen/dwgopen.h"
#include "tbcad/geom/entity.h"
#include "tbcad/mcp/png.h"
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
              "  --capture-script F   hidden window (never takes focus): run F, which holds command\n"
              "                       lines and @ lines: @size W H, @frames N, @mouse X Y, @click X Y,\n"
              "                       @drag X0 Y0 X1 Y1, @shot name.png (PNG of the whole window)\n"
              "  --size WxH           window size for --capture-script (default 1600x900)\n"
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
        else if (a == "--capture-script")
            o.captureScript = next();
        else if (a == "--size") {
            const std::string v = next();
            int w = 0, h = 0;
            if (std::sscanf(v.c_str(), "%dx%d", &w, &h) == 2 && w >= 200 && h >= 200) {
                o.captureW = w;
                o.captureH = h;
            }
        } else if (a == "--report")
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
    glfwWindowHint(GLFW_VISIBLE, hidden() ? GLFW_FALSE : GLFW_TRUE);
    if (hidden()) {
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);  // captures are the same on any screen
    }
    const bool capturing = !options_.captureScript.empty();
    window_ = glfwCreateWindow(capturing ? options_.captureW : 1440, capturing ? options_.captureH : 900,
                               "TBCAD", nullptr, nullptr);
    if (!window_) {
        why = "could not create an OpenGL 3.3 window";
        return false;
    }
    // Keep the window inside the usable screen (not behind the taskbar): scaled to a high-DPI
    // monitor 1440x900 can be larger than the work area.
    if (!hidden() && !options_.gate) {
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
    io.IniFilename = hidden() ? nullptr : iniPath_.c_str();
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

// Draws n frames without presenting anything (the window is hidden).
void App::captureFrames(int n) {
    for (int i = 0; i < n; ++i) {
        glfwPollEvents();
        mcp_->pump();
        frame();
        glfwSwapBuffers(window_);
    }
}

// --capture-script: the lines are command lines, except those that start with '@'.
int App::runCapture() {
    std::ifstream in(platform::pathFromUtf8(options_.captureScript));
    if (!in) {
        std::fprintf(stderr, "tbcad: cannot read capture script %s\n", options_.captureScript.c_str());
        return 1;
    }
    ImGuiIO& io = ImGui::GetIO();
    // Frames until the drawing is built and the picture has stopped changing.
    const auto settle = [&] {
        captureFrames(3);
        for (int i = 0; i < 600 && core_->sceneBuilding(); ++i)
            captureFrames(1);
        captureFrames(3);
    };
    const auto moveTo = [&](float x, float y) {
        io.AddMousePosEvent(x, y);
        captureFrames(2);
    };
    const std::filesystem::path scriptDir =
        platform::pathFromUtf8(options_.captureScript).parent_path();
    // Entities of the model space with a usable box, in draw order.
    struct Item {
        EntityKind kind;
        BBox2 box;
    };
    const auto items = [&] {
        std::vector<Item> v;
        for (EntityId id : core_->document().modelSpace()) {
            const Entity* e = core_->document().find(id);
            if (!e)
                continue;
            const BBox2 b = geom::bounds(*e, core_->document());
            if (b.empty() || !std::isfinite(b.min.x) || !std::isfinite(b.max.x) ||
                !std::isfinite(b.min.y) || !std::isfinite(b.max.y))
                continue;
            v.push_back({kind(*e), b});
        }
        return v;
    };
    // The box of the middle 96 % of the entity centres, so a stray object far away does not
    // decide the view.
    const auto robustBox = [&](const std::vector<Item>& v) {
        std::vector<double> xs, ys;
        for (const Item& it : v) {
            xs.push_back((it.box.min.x + it.box.max.x) / 2);
            ys.push_back((it.box.min.y + it.box.max.y) / 2);
        }
        BBox2 b;
        if (xs.empty())
            return b;
        std::sort(xs.begin(), xs.end());
        std::sort(ys.begin(), ys.end());
        const std::size_t lo = xs.size() * 2 / 100, hi = xs.size() - 1 - lo;
        b.min = {xs[lo], ys[lo]};
        b.max = {xs[hi], ys[hi]};
        return b;
    };
    static const char* const kKindNames[] = {"line",   "circle",    "arc",    "polyline", "point",
                                             "ellipse", "spline",    "text",   "mtext",    "hatch",
                                             "insert", "dimension", "leader", "proxy",    "table",
                                             "ray",    "mline",     "mleader"};
    const auto resolveShot = [&](const std::string& name) {
        std::filesystem::path f = platform::pathFromUtf8(name);
        return (f.is_absolute() ? f : scriptDir / f).string();
    };
    const auto printStats = [&] {
        const BBox2 view = core_->camera().visibleWorld();
        std::size_t total[kEntityKindCount] = {}, inView[kEntityKindCount] = {};
        for (const Item& it : items()) {
            ++total[static_cast<std::size_t>(it.kind)];
            if (it.box.intersects(view))
                ++inView[static_cast<std::size_t>(it.kind)];
        }
        std::printf("stats: view %.3f,%.3f to %.3f,%.3f; selected %zu; in view / in drawing:",
                    view.min.x, view.min.y, view.max.x, view.max.y, core_->selection().size());
        for (std::size_t k = 0; k < kEntityKindCount; ++k)
            if (total[k] > 0)
                std::printf(" %s %zu/%zu", kKindNames[k], inView[k], total[k]);
        std::printf("\n");
        std::fflush(stdout);
    };
    int failed = 0;
    int failedSinceShot = 0;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (lineNo == 1 && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            line.erase(0, 3);  // a byte order mark from an editor
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line.front() == '#')
            continue;
        if (line.empty() || line.front() != '@') {
            core_->submitText(line);
            captureFrames(1);
            continue;
        }
        std::istringstream ss(line.substr(1));
        std::string op;
        ss >> op;
        float a = 0, b = 0, c = 0, d = 0;
        if (op == "size" && (ss >> a >> b)) {
            glfwSetWindowSize(window_, static_cast<int>(a), static_cast<int>(b));
            captureFrames(3);
        } else if (op == "frames" && (ss >> a)) {
            captureFrames(std::max(1, static_cast<int>(a)));
        } else if (op == "mouse" && (ss >> a >> b)) {
            moveTo(a, b);
        } else if (op == "click" && (ss >> a >> b)) {
            moveTo(a, b);
            io.AddMouseButtonEvent(0, true);
            captureFrames(2);
            io.AddMouseButtonEvent(0, false);
            captureFrames(2);
        } else if (op == "drag" && (ss >> a >> b >> c >> d)) {
            moveTo(a, b);
            io.AddMouseButtonEvent(0, true);
            captureFrames(2);
            moveTo((a + c) / 2, (b + d) / 2);
            moveTo(c, d);
            io.AddMouseButtonEvent(0, false);
            captureFrames(2);
        } else if (op == "zoom" && (ss >> a >> b >> c >> d)) {
            core_->zoomToBox(BBox2{{std::min(a, c), std::min(b, d)}, {std::max(a, c), std::max(b, d)}});
            captureFrames(2);
        } else if (op == "zoom-all") {
            const BBox2 box = robustBox(items());
            if (box.empty())
                ++failed, ++failedSinceShot;
            else
                core_->zoomToBox(box.inflated(0.03 * std::max(box.width(), box.height())));
            captureFrames(2);
        } else if (op == "zoom-dense") {
            int n = 40;
            ss >> n;
            n = std::clamp(n, 2, 400);
            const std::vector<Item> v = items();
            BBox2 box = robustBox(v);
            if (!box.empty() && (!(box.width() > 0) || !(box.height() > 0))) {
                box = BBox2{};  // a handful of objects: all of them
                for (const Item& it : v) {
                    box.expand(it.box.min);
                    box.expand(it.box.max);
                }
            }
            if (box.empty() || !(box.width() > 0) || !(box.height() > 0)) {
                ++failed, ++failedSinceShot;
                continue;
            }
            std::vector<int> count(static_cast<std::size_t>(n) * n, 0);
            for (const Item& it : v) {
                const double cx = (it.box.min.x + it.box.max.x) / 2, cy = (it.box.min.y + it.box.max.y) / 2;
                const int ix = static_cast<int>(std::floor((cx - box.min.x) / box.width() * n));
                const int iy = static_cast<int>(std::floor((cy - box.min.y) / box.height() * n));
                if (ix >= 0 && ix < n && iy >= 0 && iy < n)
                    ++count[static_cast<std::size_t>(iy) * n + ix];
            }
            const auto best = std::max_element(count.begin(), count.end()) - count.begin();
            const double cw = box.width() / n, ch = box.height() / n;
            const double x0 = box.min.x + (best % n) * cw, y0 = box.min.y + (best / n) * ch;
            std::printf("zoom-dense: %d objects in the cell at %.3f,%.3f (cell %.3f x %.3f)\n",
                        count[static_cast<std::size_t>(best)], x0, y0, cw, ch);
            core_->zoomToBox(BBox2{{x0, y0}, {x0 + cw, y0 + ch}});
            captureFrames(2);
        } else if (op == "zoom-kind") {
            std::string kn;
            int index = 0;
            double factor = 3.0;
            ss >> kn >> index >> factor;
            int want = -1;
            for (std::size_t k = 0; k < std::size(kKindNames); ++k)
                if (kn == kKindNames[k])
                    want = static_cast<int>(k);
            if (want < 0) {
                std::fprintf(stderr, "tbcad: line %d: unknown kind '%s'\n", lineNo, kn.c_str());
                ++failed, ++failedSinceShot;
                continue;
            }
            int seen = 0;
            bool found = false;
            for (const Item& it : items()) {
                if (static_cast<int>(it.kind) != want || seen++ != index)
                    continue;
                const double w = std::max({it.box.width(), it.box.height(), 1e-3});
                const Vec2 mid{(it.box.min.x + it.box.max.x) / 2, (it.box.min.y + it.box.max.y) / 2};
                const double half = w * std::max(factor, 1.0) / 2;
                core_->zoomToBox(BBox2{{mid.x - half, mid.y - half}, {mid.x + half, mid.y + half}});
                std::printf("zoom-kind %s %d: box %.3f,%.3f to %.3f,%.3f\n", kn.c_str(), index,
                            it.box.min.x, it.box.min.y, it.box.max.x, it.box.max.y);
                found = true;
                break;
            }
            if (!found) {
                std::fprintf(stderr, "tbcad: line %d: no %s number %d\n", lineNo, kn.c_str(), index);
                ++failed, ++failedSinceShot;
            }
            captureFrames(2);
        } else if (op == "stats") {
            printStats();
        } else if (op == "deselect") {
            core_->setSelection({});
            captureFrames(2);
        } else if (op == "shot") {
            std::string name;
            ss >> name;
            settle();
            // The frame is drawn again so that the pixels are those of the back buffer.
            glfwPollEvents();
            mcp_->pump();
            frame();
            const std::string target = name.empty() ? std::string{} : resolveShot(name);
            std::error_code ec;
            if (!target.empty())
                std::filesystem::remove(platform::pathFromUtf8(target), ec);  // never leave a stale one
            std::printf("shot %s%s\n", name.c_str(),
                        failedSinceShot > 0 ? ": WARNING a step before it failed (see the messages above)" : "");
            printStats();
            failedSinceShot = 0;
            if (target.empty() || !saveScreenshot(target)) {
                std::fprintf(stderr, "tbcad: line %d: cannot write %s\n", lineNo, name.c_str());
                ++failed;
            }
            glfwSwapBuffers(window_);
        } else {
            std::fprintf(stderr, "tbcad: line %d: unknown capture line '%s'\n", lineNo, line.c_str());
            ++failed;
        }
    }
    return failed == 0 ? 0 : 1;
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
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0) {
        // Rows top first, opaque.
        std::vector<std::uint8_t> rgba(px.size());
        const std::size_t row = static_cast<std::size_t>(framebufferW_) * 4;
        for (int y = 0; y < framebufferH_; ++y) {
            std::memcpy(&rgba[static_cast<std::size_t>(framebufferH_ - 1 - y) * row],
                        &px[static_cast<std::size_t>(y) * row], row);
        }
        for (std::size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 255;
        const std::vector<std::uint8_t> png = mcp::encodePng(rgba, framebufferW_, framebufferH_);
        std::ofstream pout(platform::pathFromUtf8(path), std::ios::binary);
        if (png.empty() || !pout)
            return false;
        pout.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        return static_cast<bool>(pout);
    }
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

    if (!options_.captureScript.empty()) {
        const int status = runCapture();
        mcp_.reset();
        core_->shutdown();
        return status;
    }

    int framesDrawn = 0;
    double last = glfwGetTime();
    while (!glfwWindowShouldClose(window_) && !gui_->wantsExit()) {
        const bool scripted = options_.frames > 0;
        if (scripted || gui_->consumeActivity() || core_->sceneBuilding())
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
