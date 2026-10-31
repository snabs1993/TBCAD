#include "app.h"

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <glad/gl.h>
#ifdef TBCAD_HAS_PRO
#include <tbcad/pro/pro.h>
#endif
#include <imgui.h>
#include <imgui_internal.h>
#include <miniz.h>
#include <tbcad_imgui_items.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>

#include "gl_problem.h"
#include "gui.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/core/units.h"
#include "tbcad/dwgopen/dwgopen.h"
#include "tbcad/geom/entity.h"
#include "tbcad/layout/layout.h"
#include "tbcad/lispcmd/lispcmd.h"
#include "tbcad/mcp/png.h"
#include "tbcad/platform/crash.h"
#include "tbcad/platform/paths.h"
#include "tbcad/render/camera.h"
#include "tbcad/ui/selection.h"
#include "tbcad/underlay/image.h"

namespace tbcad::app {

namespace {

void printUsage() {
    std::puts(
        "TBCAD " TBCAD_VERSION
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
        "  --capture-out DIR    folder for shots, files and {out} in the script lines\n"
        "  --gate [--report F]  run the performance gate (1 M lines) and exit\n"
        "  --gate-file F        time open, pan/zoom and select-all on drawing F, then exit\n"
        "  --gate-lines N       use N lines for the gate instead of 1 000 000\n"
        "  --list-commands F    write every command with its aliases (Markdown) to F "
        "(default: stdout)\n"
        "  --self-test          hidden window; exit code 77 when no display or OpenGL 3.3\n"
        "  --mcp                turn the MCP server on (AI access to the drawing)\n"
        "  --no-vsync           do not wait for the monitor\n"
        "  --software-gl        draw with software OpenGL (needs the software-gl folder, Windows)\n"
        "  --child              a window opened by another TBCAD window (own autosave folder)\n"
        "  --window-pos X,Y     put the window here (with --window-size W,H: this big)\n");
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
        else if (a == "--capture-out")
            o.captureOut = next();
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
        else if (a == "--child")
            o.child = true;
        else if (a == "--software-gl")
            o.softwareGl = true;
        else if (a == "--window-pos")
            std::sscanf(next().c_str(), "%d,%d", &o.windowX, &o.windowY);
        else if (a == "--window-size")
            std::sscanf(next().c_str(), "%d,%d", &o.windowW, &o.windowH);
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

// Probe with a default (any version) context for the GPU name, then tell the user plainly instead
// of leaving a blank window or a silent exit.
void App::reportOpenGlProblem() {
    std::string renderer, version;
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    if (GLFWwindow* probe = glfwCreateWindow(64, 64, "TBCAD", nullptr, nullptr)) {
        glfwMakeContextCurrent(probe);
        using GetString = const unsigned char* (*)(unsigned int);
        if (auto get = reinterpret_cast<GetString>(glfwGetProcAddress("glGetString"))) {
            if (const unsigned char* r = get(0x1F01))  // GL_RENDERER
                renderer = reinterpret_cast<const char*>(r);
            if (const unsigned char* v = get(0x1F02))  // GL_VERSION
                version = reinterpret_cast<const char*>(v);
        }
        glfwMakeContextCurrent(nullptr);
        glfwDestroyWindow(probe);
    }
    const std::string text = openGlProblemText(renderer, version);
    std::fprintf(stderr, "tbcad: %s\n", text.c_str());
    const bool offer = !options_.softwareGl && softwareGlAvailable();
    if (showOpenGlProblem(text, offer))
        relaunchWithSoftwareGl();
}

bool App::initWindow(std::string& why) {
    glfwSetErrorCallback(glfwError);
    if ((options_.softwareGl || platform::processEnvironment("TBCAD_SOFTWARE_GL").has_value()) &&
        !useSoftwareGl())
        std::fprintf(stderr, "tbcad: software OpenGL (software-gl/opengl32.dll) not found\n");
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
    window_ = glfwCreateWindow(capturing ? options_.captureW : 1440,
                               capturing ? options_.captureH : 900, "TBCAD", nullptr, nullptr);
    if (!window_) {
        why = "could not create an OpenGL 3.3 window";
        if (!hidden() && !options_.selfTest && options_.captureScript.empty() && !options_.gate)
            reportOpenGlProblem();
        return false;
    }
    // Keep the window inside the usable screen (not behind the taskbar): scaled to a high-DPI
    // monitor 1440x900 can be larger than the work area.
    if (!hidden() && !options_.gate && options_.child && options_.windowW > 0) {
        placeChildWindow();
    } else if (!hidden() && !options_.gate) {
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
        if (!hidden() && !options_.selfTest && options_.captureScript.empty() && !options_.gate)
            reportOpenGlProblem();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingWithShift = false;
    if (!hidden()) {
        // Panels can be dragged out of the window into their own OS windows and back in.
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.ConfigViewportsNoTaskBarIcon = true;
    }
    iniPath_ = (paths_.configDir / "imgui.ini").string();
    platform::ensureDirectory(paths_.configDir);
    io.IniFilename = hidden() ? nullptr : iniPath_.c_str();
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    imguiReady_ = true;
    windowReady_ = true;
    return true;
}

// A window another window opened: on the screen where the tab was let go, as big as the window
// it came from (maximized when that fills the screen).
void App::placeChildWindow() {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    for (int i = 0; i < count; ++i) {
        int x = 0, y = 0, w = 0, h = 0;
        glfwGetMonitorWorkarea(monitors[i], &x, &y, &w, &h);
        if (options_.windowX >= x && options_.windowX < x + w && options_.windowY >= y &&
            options_.windowY < y + h)
            mon = monitors[i];
    }
    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetMonitorWorkarea(mon, &wx, &wy, &ww, &wh);
    int fl = 0, ft = 0, fr = 0, fb = 0;
    glfwGetWindowFrameSize(window_, &fl, &ft, &fr, &fb);
    const int w = std::max(480, options_.windowW), h = std::max(320, options_.windowH);
    if (ww <= 0 || wh <= 0)
        return;
    if (w + fl + fr >= ww * 9 / 10 || h + ft + fb >= wh * 9 / 10) {
        glfwSetWindowPos(window_, wx + fl, wy + ft);
        glfwMaximizeWindow(window_);
        return;
    }
    glfwSetWindowSize(window_, w, h);
    const int x = std::clamp(options_.windowX - 120, wx + fl, std::max(wx + fl, wx + ww - w - fr));
    const int y = std::clamp(options_.windowY - 20, wy + ft, std::max(wy + ft, wy + wh - h - fb));
    glfwSetWindowPos(window_, x, y);
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
    core_->submitScript(script);
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
        if (recordItems_)
            imgui_items::clear();  // keep the newest frame's widgets only
        frame();
        glfwSwapBuffers(window_);
        ++framesDrawn_;
        if (recordItems_) {
            // The command line's history goes to the log, so a failed check shows its cause.
            const auto& h = core_->history();
            if (h.size() < historySeen_)
                historySeen_ = 0;
            for (; historySeen_ < h.size(); ++historySeen_)
                std::printf("| %s\n", h[historySeen_].c_str());
            std::fflush(stdout);
        }
    }
}

// --capture-script: the lines are command lines, except those that start with '@'.
int App::runCapture() {
    std::ifstream in(platform::pathFromUtf8(options_.captureScript));
    if (!in) {
        std::fprintf(stderr, "tbcad: cannot read capture script %s\n",
                     options_.captureScript.c_str());
        return 1;
    }
    ImGuiIO& io = ImGui::GetIO();
    // Frames until the drawing is built and the picture has stopped changing.
    // A drawing opened on a worker thread counts too: real files can take a while.
    const auto settle = [&] {
        captureFrames(3);
        const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(15);
        while ((core_->sceneBuilding() || core_->opening()) &&
               std::chrono::steady_clock::now() < until)
            captureFrames(1);
        captureFrames(3);
    };
    const auto moveTo = [&](float x, float y) {
        io.AddMousePosEvent(x, y);
        captureFrames(2);
    };
    std::error_code dirEc;
    std::filesystem::path scriptDir =
        std::filesystem::absolute(platform::pathFromUtf8(options_.captureScript), dirEc)
            .parent_path();
    if (dirEc)
        scriptDir = platform::pathFromUtf8(options_.captureScript).parent_path();
    // Entities of the model space with a usable box, in draw order.
    struct Item {
        EntityKind kind;
        BBox2 box;
        EntityId id;
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
            v.push_back({kind(*e), b, id});
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
    static const char* const kKindNames[] = {"line",    "circle",    "arc",    "polyline", "point",
                                             "ellipse", "spline",    "text",   "mtext",    "hatch",
                                             "insert",  "dimension", "leader", "proxy",    "table",
                                             "ray",     "mline",     "mleader"};
    const std::filesystem::path outDir =
        options_.captureOut.empty() ? scriptDir : platform::pathFromUtf8(options_.captureOut);
    if (!options_.captureOut.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
    }
    const auto resolveShot = [&](const std::string& name) {
        std::filesystem::path f = platform::pathFromUtf8(name);
        return (f.is_absolute() ? f : outDir / f).string();
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
    // {name} values set by the generic steps (@enter-viewport, @hatch-pick, ...).
    std::map<std::string, std::string> vars{{"layout", ""}, {"pick", ""}, {"picked", ""}};
    struct Counts {
        std::size_t objects = 0, layouts = 0;
        std::map<int, std::size_t> byKind{};  // model space objects of each kind
    };
    std::map<std::string, Counts> counts;
    const auto countNow = [&] {
        Counts c{core_->document().modelSpaceCount(), core_->layoutNames().size(), {}};
        for (EntityId id : core_->document().modelSpace())
            if (const Entity* e = core_->document().find(id))
                ++c.byKind[static_cast<int>(kind(*e))];
        return c;
    };
    // "line +40, text -2": the kinds whose count changed, to tell what a round trip added.
    const auto kindDiff = [&](const Counts& then, const Counts& now) {
        std::map<int, long long> d;
        for (const auto& [k, n] : now.byKind)
            d[k] += static_cast<long long>(n);
        for (const auto& [k, n] : then.byKind)
            d[k] -= static_cast<long long>(n);
        std::string out;
        for (const auto& [k, n] : d) {
            if (n == 0)
                continue;
            const char* nm = k >= 0 && k < static_cast<int>(std::size(kKindNames))
                                 ? kKindNames[static_cast<std::size_t>(k)]
                                 : "other";
            out += (out.empty() ? "" : ", ") + std::string(nm) + (n > 0 ? " +" : " ") +
                   std::to_string(n);
        }
        return out;
    };
    double vpScaleBefore = 0;
    bool skipping = false;  // @skip-step-unless: until the next @step
    // @step: the time of each step, printed when the next one starts (and at the end).
    std::string stepName;
    auto stepStart = std::chrono::steady_clock::now();
    std::size_t stepFrames = 0;
    const auto endStep = [&] {
        if (stepName.empty())
            return;
        settle();
        const double s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - stepStart).count();
        // The frames drawn are part of the time: with a software renderer they dominate it.
        std::printf("step %s: %.3f s (%zu frames)\n", stepName.c_str(), s,
                    framesDrawn_ - stepFrames);
        std::fflush(stdout);
        stepName.clear();
    };
    // A world point on the window (points), through the viewport of the last frame.
    const auto toWindow = [&](double wx, double wy) {
        const ViewportRect& r = gui_->lastViewportRect();
        const double s = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0;
        const Vec2 p = core_->camera().worldToScreen({wx, wy});
        return ImVec2(static_cast<float>(r.x + p.x / s), static_cast<float>(r.y + p.y / s));
    };
    // Two clicks where the mouse is, read as a double click however long the frames between them
    // take: a big drawing can draw slower than the double-click time, a user's hand cannot.
    const auto doubleClickHere = [&] {
        const float keep = io.MouseDoubleClickTime;
        io.MouseDoubleClickTime = 3600.0f;
        for (int k = 0; k < 2; ++k) {
            io.AddMouseButtonEvent(0, true);
            captureFrames(1);
            io.AddMouseButtonEvent(0, false);
            captureFrames(1);
        }
        io.MouseDoubleClickTime = keep;
    };
    // "Enter", "Escape", "F8", "A", "ctrl+Z", "shift+Tab", ...
    const auto pressKey = [&](std::string name) {
        bool ctrl = false, shift = false;
        for (;;) {
            if (name.rfind("ctrl+", 0) == 0) {
                ctrl = true;
                name.erase(0, 5);
            } else if (name.rfind("shift+", 0) == 0) {
                shift = true;
                name.erase(0, 6);
            } else {
                break;
            }
        }
        ImGuiKey key = ImGuiKey_None;
        static const std::pair<const char*, ImGuiKey> kNamed[] = {
            {"Enter", ImGuiKey_Enter},    {"Escape", ImGuiKey_Escape},
            {"Delete", ImGuiKey_Delete},  {"Tab", ImGuiKey_Tab},
            {"Space", ImGuiKey_Space},    {"Backspace", ImGuiKey_Backspace},
            {"Up", ImGuiKey_UpArrow},     {"Down", ImGuiKey_DownArrow},
            {"Left", ImGuiKey_LeftArrow}, {"Right", ImGuiKey_RightArrow}};
        for (const auto& [n, k] : kNamed)
            if (name == n)
                key = k;
        if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
            key = static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'A'));
        if (name.size() >= 2 && name[0] == 'F') {
            const int f = std::atoi(name.c_str() + 1);
            if (f >= 1 && f <= 12)
                key = static_cast<ImGuiKey>(ImGuiKey_F1 + (f - 1));
        }
        if (key == ImGuiKey_None)
            return false;
        if (ctrl)
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (shift)
            io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(key, true);
        captureFrames(2);
        io.AddKeyEvent(key, false);
        if (shift)
            io.AddKeyEvent(ImGuiMod_Shift, false);
        if (ctrl)
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
        captureFrames(2);
        return true;
    };
    // @expect WHAT ...: a check; a failed one is printed and makes the exit status 1.
    const auto expect = [&](std::istringstream& ss, int lineNo, const std::string& lineText) {
        std::string what;
        ss >> what;
        std::string got;
        bool ok = false;
        const auto countIs = [&](std::size_t have) {
            std::size_t want = 0;
            ss >> want;
            got = std::to_string(have);
            return have == want;
        };
        if (what == "objects") {
            ok = countIs(core_->document().modelSpaceCount());
        } else if (what == "selected") {
            ok = countIs(core_->selection().size());
        } else if (what == "layer") {
            std::string name, state;
            ss >> name >> state;
            const Layer* l = core_->document().layerByName(name);
            got = !l ? "no such layer"
                     : std::string(l->on ? "on" : "off") + (l->frozen ? " frozen" : "") +
                           (l->locked ? " locked" : "");
            ok = l && ((state == "on" && l->on) || (state == "off" && !l->on) ||
                       (state == "frozen" && l->frozen) || (state == "thawed" && !l->frozen) ||
                       (state == "locked" && l->locked) || (state == "unlocked" && !l->locked));
        } else if (what == "file") {
            std::string name;
            ss >> name;
            std::error_code ec;
            const auto size =
                std::filesystem::file_size(platform::pathFromUtf8(resolveShot(name)), ec);
            got = ec ? "missing" : std::to_string(size) + " bytes";
            ok = !ec && size > 0;
        } else if (what == "endpoint") {
            // Line ends and polyline vertices at a point.
            double x = 0, y = 0;
            ss >> x >> y;
            const double tol = 1e-6 * std::max({1.0, std::abs(x), std::abs(y)});
            const auto at = [&](Vec2 p) {
                return std::abs(p.x - x) <= tol && std::abs(p.y - y) <= tol;
            };
            std::size_t have = 0;
            for (EntityId id : core_->document().modelSpace()) {
                const Entity* e = core_->document().find(id);
                if (!e)
                    continue;
                if (const auto* l = entityAs<LineData>(*e))
                    have += at(l->a) + at(l->b);
                else if (const auto* pl = entityAs<PolylineData>(*e))
                    for (const PolyVertex& v : pl->v)
                        have += at(v.p);
            }
            ok = countIs(have);
        } else if (what == "same" || what == "added") {
            // same NAME: as many objects and layouts as when @count NAME ran;
            // added NAME K: K more objects than then
            std::string name;
            long long k = 0;
            ss >> name >> k;
            const auto it = counts.find(name);
            const Counts now = countNow();
            got = std::to_string(now.objects) + " objects, " + std::to_string(now.layouts) +
                  " layouts";
            if (it == counts.end()) {
                got += "; no @count " + name;
            } else {
                got += " (then " + std::to_string(it->second.objects) + ", " +
                       std::to_string(it->second.layouts) + ")";
                ok = now.layouts == it->second.layouts &&
                     static_cast<long long>(now.objects) ==
                         static_cast<long long>(it->second.objects) + (what == "added" ? k : 0);
                if (!ok)
                    if (const std::string d = kindDiff(it->second, now); !d.empty())
                        got += "; by kind: " + d;
            }
        } else if (what == "viewport-zoomed") {
            // viewport-zoomed F: the viewport entered by @enter-viewport shows F times closer
            double f = 1;
            ss >> f;
            const auto l = layout::findLayout(core_->document(), vars["layout"]);
            const layout::Viewport* vp =
                l ? l->findViewport(std::atoi(vars["vpid"].c_str())) : nullptr;
            char buf[96] = "no viewport entered";
            if (vp) {
                std::snprintf(buf, sizeof buf, "1:%.6g, was 1:%.6g%s", vp->scaleN, vpScaleBefore,
                              vp->locked ? " (locked)" : "");
                const double want = vp->locked ? vpScaleBefore : vpScaleBefore / f;
                ok = std::abs(vp->scaleN - want) <= 1e-6 * std::max(1.0, want);
            }
            got = buf;
        } else if (what == "layout") {
            std::string name;
            ss >> name;
            got = core_->inPaperSpace() ? core_->activeLayout() : std::string("Model");
            ok = got == name;
        } else if (what == "viewport") {
            // viewport LAYOUT ID scale N | viewport LAYOUT ID center X Y
            std::string lay, field;
            int id = 0;
            ss >> lay >> id >> field;
            const auto l = layout::findLayout(core_->document(), lay);
            const layout::Viewport* vp = l ? l->findViewport(id) : nullptr;
            const auto near = [](double a, double b) {
                return std::abs(a - b) <= 1e-3 * std::max(1.0, std::abs(b));
            };
            char buf[96] = "no such viewport";
            if (vp && field == "scale") {
                double n = 0;
                ss >> n;
                std::snprintf(buf, sizeof buf, "1:%.6g", vp->scaleN);
                ok = near(vp->scaleN, n);
            } else if (vp && field == "center") {
                double x = 0, y = 0;
                ss >> x >> y;
                std::snprintf(buf, sizeof buf, "%.6g,%.6g", vp->center.x, vp->center.y);
                ok = near(vp->center.x, x) && near(vp->center.y, y);
            }
            got = buf;
        } else if (what == "text") {
            // text N SUBSTRING: TEXT and MTEXT whose contents hold the substring
            std::size_t want = 0;
            ss >> want;
            std::string rest;
            std::getline(ss, rest);
            while (!rest.empty() && rest.front() == ' ')
                rest.erase(0, 1);
            std::size_t have = 0;
            for (EntityId id : core_->document().modelSpace()) {
                const Entity* e = core_->document().find(id);
                if (!e)
                    continue;
                if (const auto* t = entityAs<TextData>(*e))
                    have += t->utf8.find(rest) != std::string::npos;
                else if (const auto* m = entityAs<MTextData>(*e))
                    have += m->utf8.find(rest) != std::string::npos;
            }
            got = std::to_string(have);
            ok = have == want;
        } else if (what == "on-layer" || what == "color") {
            // on-layer KIND LAYER N | color KIND ACI N (KIND "any" counts every kind)
            std::string k, value;
            ss >> k >> value;
            int want = -1;
            for (std::size_t i = 0; i < std::size(kKindNames); ++i)
                if (k == kKindNames[i])
                    want = static_cast<int>(i);
            const Layer* layer =
                what == "on-layer" ? core_->document().layerByName(value) : nullptr;
            const int aci = what == "color" ? std::atoi(value.c_str()) : -1;
            std::size_t have = 0;
            for (EntityId id : core_->document().modelSpace()) {
                const Entity* e = core_->document().find(id);
                if (!e || (k != "any" && static_cast<int>(kind(*e)) != want))
                    continue;
                if (layer ? e->common.layer == layer->id
                          : e->common.color.kind == ColorKind::Index &&
                                e->common.color.indexValue() == aci)
                    ++have;
            }
            ok = countIs(have);
        } else if (what == "contains" || what == "lacks") {
            // contains FILE TEXT / lacks FILE TEXT: the file holds the text or not (PDF streams are
            // inflated first)
            std::string name, rest;
            ss >> name;
            std::getline(ss, rest);
            while (!rest.empty() && rest.front() == ' ')
                rest.erase(0, 1);
            std::ifstream f(platform::pathFromUtf8(resolveShot(name)), std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
            got = !f.good() && bytes.empty() ? "missing" : std::to_string(bytes.size()) + " bytes";
            ok = !rest.empty() && bytes.find(rest) != std::string::npos;
            // PDF page content is deflated: look inside each stream as well.
            for (std::size_t at = 0; !ok && !rest.empty();) {
                at = bytes.find("stream", at);
                if (at == std::string::npos)
                    break;
                std::size_t begin = at + 6;
                if (begin < bytes.size() && bytes[begin] == '\r')
                    ++begin;
                if (begin < bytes.size() && bytes[begin] == '\n')
                    ++begin;
                const std::size_t end = bytes.find("endstream", begin);
                if (end == std::string::npos)
                    break;
                at = end + 9;
                for (mz_ulong cap = 1 << 16; cap <= (mz_ulong(1) << 24); cap *= 4) {
                    std::string out(cap, '\0');
                    mz_ulong n = cap;
                    const int r =
                        mz_uncompress(reinterpret_cast<unsigned char*>(out.data()), &n,
                                      reinterpret_cast<const unsigned char*>(bytes.data() + begin),
                                      static_cast<mz_ulong>(end - begin));
                    if (r == MZ_BUF_ERROR)
                        continue;
                    if (r == MZ_OK)
                        ok = std::string_view(out.data(), n).find(rest) != std::string_view::npos;
                    break;
                }
            }
            if (what == "lacks")
                ok = !rest.empty() && !ok && bytes.size() > 0;
        } else if (what == "setting") {
            // setting KEY VALUE: true/false, a number or text, as the settings hold it.
            std::string key, want;
            ss >> key;
            std::getline(ss, want);
            while (!want.empty() && want.front() == ' ')
                want.erase(0, 1);
            const auto& st = core_->settings();
            switch (st.kindOf(key)) {
                case platform::Settings::Kind::Bool:
                    got = st.getBool(key) ? "true" : "false";
                    break;
                case platform::Settings::Kind::Number: {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%g", st.getNumber(key));
                    got = buf;
                    break;
                }
                case platform::Settings::Kind::String:
                    got = st.getString(key);
                    break;
                case platform::Settings::Kind::Missing:
                    got = "missing";
                    break;
                default:
                    got = "a list";
            }
            ok = got == want;
        } else if (what == "current-layer") {
            std::string name;
            ss >> name;
            const Layer* l = core_->document().layer(core_->document().header().currentLayer);
            got = l ? l->name : "none";
            ok = l && l->name == name;
        } else if (what == "units") {
            std::string name;
            ss >> name;
            got = std::string(unitSuffix(core_->document().header().units));
            ok = got == name;
        } else if (what == "status") {
            // status LABEL on|off: a toggle of the status bar (OSNAP, ORTHO, ...).
            std::string label, state;
            ss >> label >> state;
            got = "no such toggle";
            for (const auto& t : uiRegistry_->statusToggles())
                if (t.label == label || t.id == label) {
                    got = t.get() ? "on" : "off";
                    ok = got == state;
                }
        } else if (what == "item") {
            // item LABEL: a widget with this label was drawn in the last frame.
            std::string label;
            std::getline(ss, label);
            while (!label.empty() && label.front() == ' ')
                label.erase(0, 1);
            got = "not drawn";
            for (const imgui_items::Item& it : imgui_items::items())
                if (!label.empty() &&
                    (it.label == label || it.label.substr(0, it.label.find("##")) == label)) {
                    got = "in " + it.window;
                    ok = true;
                }
        } else if (what == "warnings") {
            // warnings N: lines the program printed at startup about the settings files.
            ok = countIs(core_->startupWarnings().size());
        } else if (what == "startup-ms") {
            // startup-ms N: the first frame was drawn within N milliseconds.
            double limit = 0;
            ss >> limit;
            const double ms = startupPhases_.empty() ? -1 : startupPhases_.back().second;
            got = std::to_string(std::lround(ms)) + " ms";
            ok = ms >= 0 && ms <= limit;
        } else if (what == "log") {
            std::string rest;
            std::getline(ss, rest);
            while (!rest.empty() && rest.front() == ' ')
                rest.erase(0, 1);
            got = "not in the command history";
            for (const std::string& h : core_->history())
                if (h.find(rest) != std::string::npos) {
                    got = h;
                    ok = !rest.empty();
                }
        } else if (what == "prompt") {
            std::string rest;
            std::getline(ss, rest);
            while (!rest.empty() && rest.front() == ' ')
                rest.erase(0, 1);
            got = core_->promptText();
            ok = got.find(rest) != std::string::npos;
        } else {
            int want = -1;
            for (std::size_t k = 0; k < std::size(kKindNames); ++k)
                if (what == kKindNames[k])
                    want = static_cast<int>(k);
            if (want >= 0) {
                std::size_t have = 0;
                for (EntityId id : core_->document().modelSpace())
                    if (const Entity* e = core_->document().find(id);
                        e && static_cast<int>(kind(*e)) == want)
                        ++have;
                ok = countIs(have);
            } else {
                got = "unknown check";
            }
        }
        std::printf("%s line %d: %s (got %s)\n", ok ? "ok" : "FAILED", lineNo, lineText.c_str(),
                    got.c_str());
        std::fflush(stdout);
        return ok;
    };
    // Widgets by label (imgui_items records them while capture mode runs).
    const auto findItem = [](const std::string& label, const std::string& window,
                             int nth) -> const imgui_items::Item* {
        const auto visible = [](const std::string& l) {
            std::string v = l.substr(0, l.find("##"));
            const auto a = v.find_first_not_of(' ');
            return a == std::string::npos ? std::string{}
                                          : v.substr(a, v.find_last_not_of(' ') - a + 1);
        };
        std::vector<const imgui_items::Item*> hits;
        for (const imgui_items::Item& it : imgui_items::items()) {
            const bool match =
                (!it.label.empty() && (it.label == label || visible(it.label) == label)) ||
                (it.seed != 0 && ImHashStr(label.c_str(), 0, it.seed) == it.id);
            if (match && (window.empty() || it.window.find(window) != std::string::npos))
                hits.push_back(&it);
        }
        std::sort(hits.begin(), hits.end(), [](const auto* a, const auto* b) {
            return a->min.y != b->min.y ? a->min.y < b->min.y : a->min.x < b->min.x;
        });
        hits.erase(std::unique(hits.begin(), hits.end(),
                               [](const auto* a, const auto* b) { return a->id == b->id; }),
                   hits.end());
        return nth >= 0 && nth < static_cast<int>(hits.size()) ? hits[std::size_t(nth)] : nullptr;
    };
    imgui_items::setRecording(ImGui::GetCurrentContext(), true);
    recordItems_ = true;
    settle();  // the window, the viewport size and the first view are in place before line 1
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
        // {env:NAME}, {env:NAME|fallback}, {stem:NAME} (the file name of the path in NAME,
        // without its extension) and the values of the generic steps.
        for (std::size_t at = 0; (at = line.find('{', at)) != std::string::npos;) {
            const std::size_t close = line.find('}', at);
            if (close == std::string::npos)
                break;
            const std::string key = line.substr(at + 1, close - at - 1);
            std::optional<std::string> value;
            const auto env = [](const std::string& n) {
                const char* v = std::getenv(n.c_str());
                return v ? std::string(v) : std::string();
            };
            if (key.rfind("env:", 0) == 0) {
                const std::string spec = key.substr(4);
                const auto bar = spec.find('|');
                value = env(spec.substr(0, bar));
                if (value->empty() && bar != std::string::npos)
                    value = spec.substr(bar + 1);
            } else if (key.rfind("stem:", 0) == 0) {
                // Spaces become '_': @shot and @expect file take a name without spaces.
                value = platform::pathToUtf8(platform::pathFromUtf8(env(key.substr(5))).stem());
                std::replace(value->begin(), value->end(), ' ', '_');
            } else if (const auto v = vars.find(key.substr(0, key.find('|'))); v != vars.end()) {
                value = v->second;  // {name|fallback}: the fallback when the step found nothing
                if (value->empty() && key.find('|') != std::string::npos)
                    value = key.substr(key.find('|') + 1);
            }
            if (!value) {
                at = close + 1;
                continue;
            }
            line.replace(at, close - at + 1, *value);
            at += value->size();
        }
        for (std::size_t at; (at = line.find("{out}")) != std::string::npos;)
            line.replace(at, 5, platform::pathToUtf8(outDir));
        for (std::size_t at; (at = line.find("{dir}")) != std::string::npos;)
            line.replace(at, 5, platform::pathToUtf8(scriptDir));
        if (skipping) {
            if (line.rfind("@step", 0) != 0)
                continue;
            skipping = false;
        }
        // "@10,5" and "@20<45" are relative points typed at a prompt, not capture lines.
        const bool relativePoint = line.size() > 1 && line.front() == '@' &&
                                   (std::isdigit(static_cast<unsigned char>(line[1])) ||
                                    line[1] == '-' || line[1] == '.' || line[1] == '<');
        if (line.empty() || line.front() != '@' || relativePoint) {
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
        } else if (op == "mousew" && (ss >> a >> b)) {
            const ImVec2 w = toWindow(a, b);
            moveTo(w.x, w.y);
        } else if (op == "clickw" && (ss >> a >> b)) {
            const ImVec2 w = toWindow(a, b);
            moveTo(w.x, w.y);
            io.AddMouseButtonEvent(0, true);
            captureFrames(2);
            io.AddMouseButtonEvent(0, false);
            captureFrames(2);
        } else if (op == "dblclickw" && (ss >> a >> b)) {
            const ImVec2 w = toWindow(a, b);
            moveTo(w.x, w.y);
            doubleClickHere();
            captureFrames(2);
        } else if (op == "dragw" && (ss >> a >> b >> c >> d)) {
            const ImVec2 w0 = toWindow(a, b), w1 = toWindow(c, d);
            moveTo(w0.x, w0.y);
            io.AddMouseButtonEvent(0, true);
            captureFrames(2);
            moveTo((w0.x + w1.x) / 2, (w0.y + w1.y) / 2);
            moveTo(w1.x, w1.y);
            io.AddMouseButtonEvent(0, false);
            captureFrames(2);
        } else if (op == "clickitem" || op == "dblclickitem" || op == "rclickitem") {
            // @clickitem LABEL [| WINDOW [| NTH]]: a widget by its label, as the GUI tests do
            std::string rest;
            std::getline(ss, rest);
            std::vector<std::string> parts;
            for (std::size_t from = 0;;) {
                const std::size_t bar = rest.find(" | ", from);
                parts.push_back(rest.substr(from, bar == std::string::npos ? bar : bar - from));
                if (bar == std::string::npos)
                    break;
                from = bar + 3;
            }
            for (std::string& part : parts) {
                const auto first = part.find_first_not_of(' ');
                part = first == std::string::npos
                           ? std::string{}
                           : part.substr(first, part.find_last_not_of(' ') - first + 1);
            }
            const imgui_items::Item* it =
                findItem(parts[0], parts.size() > 1 ? parts[1] : std::string{},
                         parts.size() > 2 ? std::atoi(parts[2].c_str()) : 0);
            if (!it) {
                std::fprintf(stderr, "tbcad: line %d: no widget '%s'\n", lineNo, rest.c_str());
                ++failed, ++failedSinceShot;
            } else {
                const float w = it->max.x - it->min.x;
                const float x = w > 220.0f ? it->min.x + 24.0f : (it->min.x + it->max.x) * 0.5f;
                const float y = (it->min.y + it->max.y) * 0.5f;
                const int button = op == "rclickitem" ? 1 : 0;
                moveTo(x, y);
                if (op == "dblclickitem") {
                    doubleClickHere();
                } else {
                    io.AddMouseButtonEvent(button, true);
                    captureFrames(1);
                    io.AddMouseButtonEvent(button, false);
                    captureFrames(1);
                }
                captureFrames(2);
            }
        } else if (op == "items") {
            // @items [WINDOW]: the widgets of the last frame, to write @clickitem lines with
            std::string win;
            std::getline(ss, win);
            if (!win.empty() && win.front() == ' ')
                win.erase(0, 1);
            for (const imgui_items::Item& it : imgui_items::items())
                if (!it.label.empty() && (win.empty() || it.window.find(win) != std::string::npos))
                    std::printf("item \"%s\" in \"%s\" at %.0f,%.0f\n", it.label.c_str(),
                                it.window.c_str(), it.min.x, it.min.y);
            std::fflush(stdout);
        } else if (op == "require-env") {
            // @require-env NAME...: without them the scenario is skipped (exit 77, as ctest wants)
            for (std::string n; ss >> n;) {
                const char* v = std::getenv(n.c_str());
                if (!v || !*v) {
                    std::printf("skipped: %s is not set\n", n.c_str());
                    std::fflush(stdout);
                    return 77;
                }
            }
        } else if (op == "skip-step-unless") {
            // @skip-step-unless NAME: when the generic step found nothing ({NAME} empty), the
            // lines up to the next @step are left out (a drawing without layouts or text).
            std::string name;
            ss >> name;
            if (vars[name].empty()) {
                std::printf("step %s: skipped, nothing for {%s} in this drawing\n",
                            stepName.c_str(), name.c_str());
                std::fflush(stdout);
                stepName.clear();
                skipping = true;
            }
        } else if (op == "step") {
            endStep();
            // A step that went wrong can leave its command asking: end it, so the next step
            // starts at the command prompt.
            for (int k = 0; k < 5 && core_->runner().active(); ++k) {
                if (k == 0) {
                    std::printf("FAILED line %d: a command was still running; Esc\n", lineNo);
                    ++failed, ++failedSinceShot;
                }
                core_->pressEscape();
                captureFrames(2);
            }
            std::getline(ss, stepName);
            while (!stepName.empty() && stepName.front() == ' ')
                stepName.erase(0, 1);
            if (stepName.empty())
                stepName = "line " + std::to_string(lineNo);
            stepStart = std::chrono::steady_clock::now();
            stepFrames = framesDrawn_;
        } else if (op == "count") {
            std::string name;
            ss >> name;
            settle();
            counts[name] = countNow();
            std::printf("count %s: %zu objects, %zu layouts\n", name.c_str(), counts[name].objects,
                        counts[name].layouts);
            std::fflush(stdout);
        } else if (op == "enter-viewport") {
            // The first layout with a viewport that is on (an unlocked one if there is one): the
            // sheet is shown and the viewport double-clicked. Sets {layout}, {vpid}.
            vars["layout"].clear();
            vars["vpid"].clear();
            std::string name;
            std::optional<layout::Viewport> pick;
            const std::vector<layout::Layout> all = layout::layouts(core_->document());
            for (const bool lockedToo : {false, true})
                for (const layout::Layout& l : all)
                    for (const layout::Viewport& v : l.viewports)
                        if (!pick && v.on && (lockedToo || !v.locked)) {
                            name = l.name;
                            pick = v;
                        }
            if (!pick) {
                std::printf("enter-viewport: no layout has a viewport\n");
                std::fflush(stdout);
                continue;
            }
            vars["layout"] = name;
            vars["vpid"] = std::to_string(pick->id);
            vpScaleBefore = pick->scaleN;
            core_->setActiveLayout(name);
            settle();
            // The sheet's saved view may not show the viewport (sheets drawn far from the paper's
            // origin): zoom to it, with a margin, as a user would before double-clicking.
            {
                const BBox2& p = pick->paper;
                const double mx = p.width() * 0.25, my = p.height() * 0.25;
                core_->zoomToBox(BBox2{{p.min.x - mx, p.min.y - my}, {p.max.x + mx, p.max.y + my}});
                settle();
            }
            // The middle first, then other points inside it (a clipped viewport, or a note on
            // the sheet over the middle), as a user would try.
            const BBox2 box = pick->paper;
            std::vector<Vec2> tries{{(box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2}};
            for (int gy = 1; gy < 6; ++gy)
                for (int gx = 1; gx < 6; ++gx)
                    tries.push_back(
                        {box.min.x + box.width() * gx / 6, box.min.y + box.height() * gy / 6});
            int attempts = 0;
            for (const Vec2 at : tries) {
                if (!core_->inPaperSpace() || attempts == 6)
                    break;
                if (!layout::viewportContains(*pick, at))
                    continue;
                if (attempts == 0)
                    if (auto id = ui::pickEntity(core_->document(), core_->spatialIndex(), at,
                                                 core_->apertureMm()))
                        if (const Entity* e = core_->document().find(*id)) {
                            const Layer* l = core_->document().layer(e->common.layer);
                            std::printf(
                                "enter-viewport: a %s on layer %s is under %.6g,%.6g\n",
                                kKindNames[std::min<std::size_t>(static_cast<std::size_t>(kind(*e)),
                                                                 std::size(kKindNames) - 1)],
                                l ? l->name.c_str() : "?", at.x, at.y);
                        }
                ++attempts;
                const ImVec2 w = toWindow(at.x, at.y);
                moveTo(w.x, w.y);
                doubleClickHere();
                settle();
                if (core_->inPaperSpace()) {
                    const ViewportRect& r = gui_->lastViewportRect();
                    const bool inside =
                        w.x >= r.x && w.y >= r.y && w.x < r.x + r.w && w.y < r.y + r.h;
                    std::printf(
                        "enter-viewport: a double click at %.6g,%.6g stayed on the sheet "
                        "(window %.0f,%.0f, drawing area %.0f,%.0f %.0fx%.0f%s)\n",
                        at.x, at.y, w.x, w.y, r.x, r.y, r.w, r.h, inside ? "" : ", outside it");
                }
            }
            std::printf(
                "enter-viewport: layout \"%s\" viewport %d at 1:%.6g%s, on the sheet "
                "%.6g,%.6g to %.6g,%.6g%s\n",
                name.c_str(), pick->id, pick->scaleN, pick->locked ? " (locked)" : "",
                pick->paper.min.x, pick->paper.min.y, pick->paper.max.x, pick->paper.max.y,
                pick->clip.empty() ? "" : " (clipped)");
            for (const layout::Layout& l : all)
                if (l.name == name) {
                    const bool onPaper = pick->paper.max.x > 0 && pick->paper.max.y > 0 &&
                                         pick->paper.min.x < l.widthMm &&
                                         pick->paper.min.y < l.heightMm;
                    std::printf("enter-viewport: the paper is %.6g x %.6g mm%s\n", l.widthMm,
                                l.heightMm, onPaper ? "" : "; the viewport lies off it");
                }
            if (core_->inPaperSpace()) {
                std::printf("enter-viewport: the double click did not enter it\n");
                ++failed, ++failedSinceShot;
            } else if (core_->editingLayout() != name || core_->editingViewport() != pick->id) {
                // Another viewport lies over the middle of this one: the checks follow the one
                // the double click entered.
                const auto l = layout::findLayout(core_->document(), core_->editingLayout());
                const layout::Viewport* vp =
                    l ? l->findViewport(core_->editingViewport()) : nullptr;
                std::printf("enter-viewport: the double click entered viewport %d of \"%s\"%s\n",
                            core_->editingViewport(), core_->editingLayout().c_str(),
                            vp && vp->locked ? " (locked)" : "");
                vars["layout"] = core_->editingLayout();
                vars["vpid"] = std::to_string(core_->editingViewport());
                vpScaleBefore = vp ? vp->scaleN : 0;
            }
            std::fflush(stdout);
        } else if (op == "select-kind") {
            // @select-kind KIND[|KIND...] [i]: selects the i-th such object and shows it.
            // Sets {picked}: the point in its middle.
            std::string kinds;
            int index = 0;
            ss >> kinds >> index;
            vars["picked"].clear();
            std::vector<int> wanted;
            for (std::size_t from = 0; from <= kinds.size();) {
                const std::size_t bar = std::min(kinds.find('|', from), kinds.size());
                const std::string k = kinds.substr(from, bar - from);
                for (std::size_t i = 0; i < std::size(kKindNames); ++i)
                    if (k == kKindNames[i])
                        wanted.push_back(static_cast<int>(i));
                from = bar + 1;
            }
            int seen = 0;
            bool found = false;
            for (const Item& it : items()) {
                if (std::find(wanted.begin(), wanted.end(), static_cast<int>(it.kind)) ==
                        wanted.end() ||
                    seen++ != index)
                    continue;
                const double half = std::max({it.box.width(), it.box.height(), 1e-3}) * 1.5;
                const Vec2 mid{(it.box.min.x + it.box.max.x) / 2,
                               (it.box.min.y + it.box.max.y) / 2};
                core_->zoomToBox(BBox2{{mid.x - half, mid.y - half}, {mid.x + half, mid.y + half}});
                core_->setSelection({it.id});
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.9g,%.9g", mid.x, mid.y);
                vars["picked"] = buf;
                std::printf("select-kind: %s number %d at %s\n", kinds.c_str(), index, buf);
                found = true;
                break;
            }
            if (!found)
                std::printf("select-kind: no %s number %d\n", kinds.c_str(), index);
            std::fflush(stdout);
            captureFrames(2);
        } else if (op == "hatch-pick") {
            // A point inside a closed polyline, with that polyline in view: sets {pick} for
            // HATCH. The polyline's middle must be inside it and nothing else may reach into
            // its box, so the pick finds that area and no islands.
            bool found = false;
            vars["pick"].clear();
            const std::vector<Item> all = items();
            for (const Item& it : all) {
                if (it.kind != EntityKind::Polyline)
                    continue;
                const Entity* e = core_->document().find(it.id);
                const auto* pl = e ? entityAs<PolylineData>(*e) : nullptr;
                if (!pl || !pl->closed || pl->v.size() < 3 || !(it.box.width() > 0) ||
                    !(it.box.height() > 0))
                    continue;
                bool straight = true;
                for (const PolyVertex& v : pl->v)
                    straight = straight && v.bulge == 0;
                if (!straight)
                    continue;
                const Vec2 mid{(it.box.min.x + it.box.max.x) / 2,
                               (it.box.min.y + it.box.max.y) / 2};
                bool inside = false;
                for (std::size_t i = 0, j = pl->v.size() - 1; i < pl->v.size(); j = i++) {
                    const Vec2 pa = pl->v[i].p, pb = pl->v[j].p;
                    if ((pa.y > mid.y) != (pb.y > mid.y) &&
                        mid.x < (pb.x - pa.x) * (mid.y - pa.y) / (pb.y - pa.y) + pa.x)
                        inside = !inside;
                }
                if (!inside)
                    continue;
                // Nothing else may reach into the polyline's box (an outline around all of it
                // is fine): a line across it would split the area.
                bool alone = true;
                for (const Item& o : all) {
                    const bool overlaps = o.box.min.x < it.box.max.x &&
                                          o.box.max.x > it.box.min.x &&
                                          o.box.min.y < it.box.max.y && o.box.max.y > it.box.min.y;
                    const bool around = o.box.min.x <= it.box.min.x &&
                                        o.box.max.x >= it.box.max.x &&
                                        o.box.min.y <= it.box.min.y && o.box.max.y >= it.box.max.y;
                    if (o.id != it.id && overlaps && !around) {
                        alone = false;
                        break;
                    }
                }
                if (!alone)
                    continue;
                const double half = std::max(it.box.width(), it.box.height()) * 0.75;
                core_->zoomToBox(BBox2{{mid.x - half, mid.y - half}, {mid.x + half, mid.y + half}});
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.9g,%.9g", mid.x, mid.y);
                vars["pick"] = buf;
                std::printf("hatch-pick: inside a closed polyline at %s\n", buf);
                found = true;
                break;
            }
            if (!found)
                std::printf("hatch-pick: no closed polyline with an empty middle\n");
            std::fflush(stdout);
            captureFrames(2);
        } else if (op == "key") {
            std::string k;
            ss >> k;
            if (!pressKey(k)) {
                std::fprintf(stderr, "tbcad: line %d: unknown key '%s'\n", lineNo, k.c_str());
                ++failed, ++failedSinceShot;
            }
        } else if (op == "type") {
            std::string rest;
            std::getline(ss, rest);
            if (!rest.empty() && rest.front() == ' ')
                rest.erase(0, 1);
            io.AddInputCharactersUTF8(rest.c_str());
            captureFrames(2);
        } else if (op == "dump") {
            // The first objects of the drawing with their main points, for writing checks.
            int shown = 0;
            for (EntityId id : core_->document().modelSpace()) {
                const Entity* e = core_->document().find(id);
                if (!e || ++shown > 50)
                    continue;
                if (const auto* l = entityAs<LineData>(*e))
                    std::printf("dump: line %.6g,%.6g to %.6g,%.6g\n", l->a.x, l->a.y, l->b.x,
                                l->b.y);
                else if (const auto* ci = entityAs<CircleData>(*e))
                    std::printf("dump: circle %.6g,%.6g r %.6g\n", ci->center.x, ci->center.y,
                                ci->radius);
                else
                    std::printf("dump: %s\n", kKindNames[static_cast<std::size_t>(kind(*e))]);
            }
            std::fflush(stdout);
        } else if (op == "wait") {
            settle();
        } else if (op == "expect") {
            settle();
            if (!expect(ss, lineNo, line))
                ++failed, ++failedSinceShot;
        } else if (op == "zoom" && (ss >> a >> b >> c >> d)) {
            core_->zoomToBox(
                BBox2{{std::min(a, c), std::min(b, d)}, {std::max(a, c), std::max(b, d)}});
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
                const double cx = (it.box.min.x + it.box.max.x) / 2,
                             cy = (it.box.min.y + it.box.max.y) / 2;
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
                const Vec2 mid{(it.box.min.x + it.box.max.x) / 2,
                               (it.box.min.y + it.box.max.y) / 2};
                const double half = w * std::max(factor, 1.0) / 2;
                core_->zoomToBox(BBox2{{mid.x - half, mid.y - half}, {mid.x + half, mid.y + half}});
                std::printf("zoom-kind %s %d: box %.3f,%.3f to %.3f,%.3f\n", kn.c_str(), index,
                            it.box.min.x, it.box.min.y, it.box.max.x, it.box.max.y);
                found = true;
                break;
            }
            if (!found) {
                std::fprintf(stderr, "tbcad: line %d: no %s number %d\n", lineNo, kn.c_str(),
                             index);
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
                std::filesystem::remove(platform::pathFromUtf8(target),
                                        ec);  // never leave a stale one
            std::printf("shot %s%s\n", name.c_str(),
                        failedSinceShot > 0
                            ? ": WARNING a step before it failed (see the messages above)"
                            : "");
            printStats();
            failedSinceShot = 0;
            if (target.empty() || !saveScreenshot(target)) {
                std::fprintf(stderr, "tbcad: line %d: cannot write %s\n", lineNo, name.c_str());
                ++failed;
            }
            glfwSwapBuffers(window_);
        } else {
            std::fprintf(stderr, "tbcad: line %d: unknown capture line '%s'\n", lineNo,
                         line.c_str());
            ++failed;
        }
    }
    endStep();
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

// The inactive model tiles show the drawing only (no grid, grips or rubber bands), each with its
// own view, like AutoCAD's other viewports.
void App::drawTiles() {
    const float sc = ImGui::GetIO().DisplayFramebufferScale.x;
    for (const auto& [i, r] : gui_->inactiveTiles()) {
        if (i >= tileTargets_.size() || !tileTargets_[i])
            continue;
        const int w = std::max(1, static_cast<int>(std::lround(r.w * sc)));
        const int h = std::max(1, static_cast<int>(std::lround(r.h * sc)));
        tileTargets_[i]->resize(w, h);
        tileTargets_[i]->bind();
        renderer_.setCamera(core_->tileCamera(i, w, h));
        renderer_.beginFrame();
        renderer_.draw(core_->underlayImages());
        renderer_.draw(core_->renderScene());
        renderer_.endFrame();
    }
    if (gui_->inactiveTiles().empty() && !tileTargets_.empty())
        tileTargets_.clear();
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

void App::takeThumbnail() {
    const std::string file = core_->pendingThumbnail();
    if (file.empty())
        return;
    core_->thumbnailTaken();
    // The drawing as the view shows it, without grips, cursor or snap marks.
    const render::Camera2D& cam = core_->camera();
    if (cam.viewportW < 8 || cam.viewportH < 8)
        return;
    thumbTarget_.resize(cam.viewportW, cam.viewportH);
    thumbTarget_.bind();
    renderer_.setBackground(gui_->viewportBackground());
    renderer_.setCamera(cam);
    renderer_.beginFrame();
    renderer_.draw(core_->underlayImages());
    renderer_.draw(core_->renderScene());
    renderer_.endFrame();
    const int w = thumbTarget_.width(), h = thumbTarget_.height();
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    const bool read = thumbTarget_.read(px.data());
    thumbTarget_.release();  // a window-sized texture, needed once per open or save
    if (!read)
        return;
    // The middle of the view at the card's shape (16:11), averaged down to 320 x 220.
    constexpr int kW = 320, kH = 220;
    int cw = w, ch = h;
    if (cw * kH > ch * kW)
        cw = ch * kW / kH;
    else
        ch = cw * kH / kW;
    const int x0 = (w - cw) / 2, y0 = (h - ch) / 2;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kW) * kH * 4);
    for (int ty = 0; ty < kH; ++ty) {
        // Rows top first; the target's rows are bottom first.
        const int sy0 = y0 + (kH - 1 - ty) * ch / kH,
                  sy1 = std::max(sy0 + 1, y0 + (kH - ty) * ch / kH);
        for (int tx = 0; tx < kW; ++tx) {
            const int sx0 = x0 + tx * cw / kW, sx1 = std::max(sx0 + 1, x0 + (tx + 1) * cw / kW);
            unsigned sum[3] = {0, 0, 0};
            unsigned n = 0;
            for (int y = sy0; y < sy1; ++y)
                for (int x = sx0; x < sx1; ++x) {
                    const std::uint8_t* p =
                        &px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                             static_cast<std::size_t>(x)) *
                            4];
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    ++n;
                }
            std::uint8_t* q =
                &out[(static_cast<std::size_t>(ty) * kW + static_cast<std::size_t>(tx)) * 4];
            for (int c = 0; c < 3; ++c)
                q[c] = static_cast<std::uint8_t>(sum[c] / n);
            q[3] = 255;
        }
    }
    const std::vector<std::uint8_t> png = mcp::encodePng(out, kW, kH);
    const std::filesystem::path dest = core_->thumbnailFile(file);
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    std::ofstream f(dest, std::ios::binary);
    if (png.empty() || !f)
        return;
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    f.close();
    // Shown again from the new file.
    if (auto it = thumbTextures_.find(dest); it != thumbTextures_.end()) {
        if (it->second) {
            const GLuint t = it->second;
            glDeleteTextures(1, &t);
        }
        thumbTextures_.erase(it);
    }
}

unsigned App::thumbnailTexture(const std::filesystem::path& png) {
    if (auto it = thumbTextures_.find(png); it != thumbTextures_.end())
        return it->second;
    unsigned& tex = thumbTextures_[png];
    std::error_code ec;
    if (!std::filesystem::exists(png, ec))
        return tex = 0;
    auto img = underlay::loadImage(png);
    if (!img || img.value()->width <= 0 || img.value()->height <= 0)
        return tex = 0;
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.value()->width, img.value()->height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, img.value()->rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex = t;
}

void App::frame() {
    PhaseTimer pt;
    core_->updateFrame();
    if (PhaseTimer::enabled())
        gPhase[0] += pt.lap();

    // "TBCAD 0.1.0-test.14 - drawing.dxf*": the build right after the program name.
    const std::string title = std::string("TBCAD " TBCAD_VERSION " - ") + core_->displayName() +
                              (core_->modified() ? "*" : "");
    if (title != lastTitle_) {
        glfwSetWindowTitle(window_, title.c_str());
        lastTitle_ = title;
    }

    gui_->applyPendingScale();
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
    for (const auto& [i, r] : gui_->inactiveTiles()) {
        if (tileTargets_.size() <= i)
            tileTargets_.resize(i + 1);
        if (!tileTargets_[i])
            tileTargets_[i] = std::make_unique<ViewportTarget>();
        gui_->showTile(tileTargets_[i]->texture(), r);
    }
    ImGui::Render();
    if (PhaseTimer::enabled())
        gPhase[1] += pt.lap();

    drawScene();
    drawTiles();
    takeThumbnail();
    if (PhaseTimer::enabled())
        pt.lap();

    glfwGetFramebufferSize(window_, &framebufferW_, &framebufferH_);
    ViewportTarget::unbindToWindow(framebufferW_, framebufferH_);
    const Rgba bg = gui_->theme().bgApp;
    glClearColor(bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }
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
    if (!startupReported_)
        reportStartup();
}

void App::startupMark(const char* phase) {
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_)
            .count();
    startupPhases_.emplace_back(phase, ms);
}

void App::reportStartup() {
    startupReported_ = true;
    startupMark("first frame");
    std::string line = "Startup: first frame after " +
                       std::to_string(std::lround(startupPhases_.back().second)) + " ms (";
    double prev = 0;
    for (std::size_t i = 0; i < startupPhases_.size(); ++i) {
        const auto& [name, at] = startupPhases_[i];
        line += (i ? ", " : "") + name + " " + std::to_string(std::lround(at - prev));
        prev = at;
    }
    line += ")";
    // Capture runs report on stdout with the rest of their log.
    std::fprintf(options_.captureScript.empty() ? stderr : stdout, "%s\n", line.c_str());
    if (platform::processEnvironment("TBCAD_STARTUP_TRACE"))
        core_->print(line);
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
        pout.write(reinterpret_cast<const char*>(png.data()),
                   static_cast<std::streamsize>(png.size()));
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
    const std::filesystem::path linkDir = paths_.dataDir / "windows";
    if (options_.child) {
        // A window opened by another one keeps its logs and the like apart, but autosaves into the
        // same folder (each process names its autosaves apart), so if it crashes the next start
        // offers its drawings.
        childDataDir_ = linkDir / ("data-" + std::to_string(WindowLink::currentProcessId()));
        paths_.autosaveOverride = paths_.autosaveDir();
        paths_.dataDir = childDataDir_;
    }
    registry_ = std::make_unique<cmd::Registry>();
    registerAllCommands(*registry_);
    startupMark("commands");
    core_ = std::make_unique<ui::AppCore>(*registry_, paths_);
    dwgopen::registerForeignFormats(*core_);
    lispcmd::loadStartup(*registry_, core_->settingsFolder(), [](const std::string& line) {
        std::fprintf(stderr, "tbcad: %s\n", line.c_str());
    });
    startupMark("settings and drawing");
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
    startupMark("plugins");
    uiRegistry_ = std::make_unique<ui::UiRegistry>();
    registerAllUi(*uiRegistry_);
    plugins_->registerUi(*uiRegistry_);
#ifdef TBCAD_HAS_PRO
    extensionPump_ = tbcad::pro::attach(*core_, *registry_, *uiRegistry_, [this] {
        if (windowReady_)
            glfwPostEmptyEvent();
    });
#endif
    mcp_->registerUi(*uiRegistry_);
    core_->registerBuiltinUi(*uiRegistry_);
    core_->applyUiKeybindings(*uiRegistry_);
    startupMark("panels");

    std::string why;
    const bool windowOk = initWindow(why);
    startupMark("window");
    if (!windowOk) {
        std::fprintf(stderr, "tbcad: %s\n", why.c_str());
        // 77 = skipped for ctest, unless the CI job says a display must be there.
        const bool required = platform::processEnvironment("TBCAD_REQUIRE_GUI").has_value();
        return options_.selfTest && !required ? 77 : 1;
    }
    gui_ = std::make_unique<Gui>(*core_, *uiRegistry_, window_);
    if (!hidden()) {
        // Drawing tabs can be dragged to other windows of the program (and out to a new one).
        windowLink_ = std::make_unique<WindowLink>(linkDir);
        gui_->setWindowLink(windowLink_.get());
    }
    gui_->setThumbnailLoader(
        [this](const std::filesystem::path& png) { return thumbnailTexture(png); });
    {
        const float total =
            gui_->dpiScale() * static_cast<float>(core_->settings().getNumber("ui.scale", 1.0));
        buildFonts(total);
        applyTheme(gui_->theme(), total);
    }
    startupMark("interface and fonts");
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
    if (!options_.openFile.empty()) {
        (void)core_->openFile(platform::pathFromUtf8(options_.openFile));
        startupMark("open file");
    }
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

    // A new version is looked for once a day, only in a normal run (not scripted or capture runs)
    // and only when the setting allows it.
    if (options_.frames == 0 && !options_.selfTest && !options_.child &&
        core_->setting("update.check").value_or("1") != "0" &&
        !std::getenv("TBCAD_NO_UPDATE_CHECK")) {
        const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        long long last = 0;
        try {
            last = std::stoll(core_->setting("update.lastCheck").value_or("0"));
        } catch (...) {
        }
        if (now - last > 20 * 3600) {
            core_->setSetting("update.lastCheck", std::to_string(now));
            gui_->checkForUpdates(false);
        }
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
        if (extensionPump_)
            extensionPump_();
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
    windowLink_.reset();
    core_->shutdown();
    removeChildData();
    gui_->launchPendingInstaller();
    return status;
}

void App::removeChildData() {
    if (childDataDir_.empty())
        return;
    std::error_code ec;
    std::filesystem::remove_all(childDataDir_, ec);
}

}  // namespace tbcad::app
