// render_bench: draws N random lines and measures frame times for a scripted pan/zoom.
//
//   render_bench [--lines N] [--frames N] [--size WxH] [--extent MM] [--thick]
//
// Works on any machine that can make an OpenGL 3.3 core context without a window (see
// testsupport/gl_context.h). On the owner's PC it runs on the real GPU; on a machine without a
// GPU it measures Mesa's software renderer, which says little about the GPU path. Frame times
// include glFinish, so they are the time until the picture is really done.
#include <glad/gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "gl_context.h"
#include "tbcad/render/renderer.h"

using namespace tbcad;
using namespace tbcad::render;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    long lines = 1000000;
    int frames = 240;
    int width = 1920;
    int height = 1080;
    double extent = 100000;  // mm; the lines fill [0, extent]^2
    bool thick = false;
};

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--lines" && argc > i + 1) {
            o.lines = std::atol(next());
        } else if (a == "--frames" && argc > i + 1) {
            o.frames = std::atoi(next());
        } else if (a == "--extent" && argc > i + 1) {
            o.extent = std::atof(next());
        } else if (a == "--size" && argc > i + 1) {
            char* end = nullptr;
            const char* v = next();
            o.width = static_cast<int>(std::strtol(v, &end, 10));
            if (*end != 'x')
                return false;
            o.height = static_cast<int>(std::strtol(end + 1, &end, 10));
            if (*end != '\0')
                return false;
        } else if (a == "--thick") {
            o.thick = true;
        } else {
            return false;
        }
    }
    return o.lines > 0 && o.frames > 0 && o.width > 0 && o.height > 0 && o.extent > 0;
}

double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Result {
    double avg = 0, worst = 0;
    RenderStats stats;
};

template <class CameraAt>
Result measure(Renderer& r, Scene& scene, int frames, CameraAt cameraAt) {
    std::vector<double> times;
    Result res;
    for (int i = 0; i < frames; ++i) {
        r.setCamera(cameraAt(static_cast<double>(i) / frames));
        auto t0 = Clock::now();
        r.beginFrame();
        r.draw(scene);
        res.stats = r.endFrame();
        glFinish();
        times.push_back(ms(t0, Clock::now()));
    }
    for (double t : times) {
        res.avg += t / times.size();
        res.worst = std::max(res.worst, t);
    }
    return res;
}

void report(const char* name, const Result& r) {
    std::printf(
        "%-34s %8.2f ms avg (%6.1f fps)  worst %8.2f ms  draws %5u  tiles %4u  verts %9llu\n", name,
        r.avg, 1000.0 / r.avg, r.worst, r.stats.drawCalls, r.stats.tilesDrawn,
        static_cast<unsigned long long>(r.stats.vertices));
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) {
        std::fprintf(stderr,
                     "usage: render_bench [--lines N] [--frames N] [--size WxH] [--extent MM] "
                     "[--thick]\n");
        return 2;
    }
    std::string why;
    auto context = testing::OffscreenGLContext::create(&why);
    if (!context) {
        std::fprintf(stderr, "no OpenGL context: %s\n", why.c_str());
        return 1;
    }
    Renderer renderer;
    std::string error;
    if (!renderer.init(&testing::OffscreenGLContext::loadProc, &error)) {
        std::fprintf(stderr, "renderer init failed: %s\n", error.c_str());
        return 1;
    }
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, opt.width, opt.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "framebuffer incomplete\n");
        return 1;
    }
    std::printf("GL: %s | %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    // Random lines: 4 layers, 7 colours, length up to 0.5% of the extent.
    BBox2 box;
    box.expand({0, 0});
    box.expand({opt.extent, opt.extent});
    Scene scene({.tileSizeMm = Scene::suggestTileSize(box)});
    const Rgba palette[7] = {{255, 0, 0, 255},    {255, 255, 0, 255},  {0, 255, 0, 255},
                             {0, 255, 255, 255},  {64, 128, 255, 255}, {255, 0, 255, 255},
                             {255, 255, 255, 255}};
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> pos(0, opt.extent), ang(0, kTwoPi),
        len(0.0005 * opt.extent * 0.2, 0.005 * opt.extent);
    auto t0 = Clock::now();
    for (long i = 1; i <= opt.lines; ++i) {
        const double x = pos(rng), y = pos(rng), a = ang(rng), l = len(rng);
        DisplayStyle st;
        st.color = palette[i % 7];
        st.layer = LayerId{static_cast<std::uint32_t>(1 + i % 4)};
        st.lineweight = opt.thick ? 0.5 : 0.0;
        DisplayList list;
        list.items.push_back(
            DisplayPolyline{{{x, y}, {x + l * std::cos(a), y + l * std::sin(a)}}, false, st});
        scene.upsert({static_cast<std::uint64_t>(i)}, list, static_cast<std::uint32_t>(i));
    }
    auto t1 = Clock::now();
    std::printf("scene: %ld lines, %zu tiles (%.0f mm), built in %.0f ms\n", opt.lines,
                scene.tiles().size(), scene.tileSize(), ms(t0, t1));

    renderer.setBackground({33, 40, 48, 255});
    auto base = [&](Vec2 center, double ppmm) {
        Camera2D c;
        c.center = center;
        c.pixelsPerMm = ppmm;
        c.viewportW = opt.width;
        c.viewportH = opt.height;
        return c;
    };
    const double fitZoom = std::min(opt.width, opt.height) / opt.extent;
    const Vec2 mid{opt.extent / 2, opt.extent / 2};

    // Upload everything once so the scenarios measure steady-state frames.
    renderer.setCamera(base(mid, fitZoom));
    renderer.beginFrame();
    renderer.draw(scene);
    RenderStats first = renderer.endFrame();
    glFinish();
    std::printf("first frame uploaded %.1f MB\n", first.uploadedBytes / 1048576.0);

    report("whole drawing (zoom extents)",
           measure(renderer, scene, opt.frames, [&](double) { return base(mid, fitZoom); }));
    report("zoom in/out (x1 .. x64)", measure(renderer, scene, opt.frames, [&](double t) {
               double z = fitZoom * std::pow(64.0, 0.5 - 0.5 * std::cos(kTwoPi * t));
               return base(mid, z);
           }));
    report("pan across at x8 zoom", measure(renderer, scene, opt.frames, [&](double t) {
               double x = opt.extent * (0.2 + 0.6 * t);
               return base({x, mid.y}, fitZoom * 8);
           }));
    report("deep zoom (x4096), panning", measure(renderer, scene, opt.frames, [&](double t) {
               return base({mid.x + t * 40, mid.y}, fitZoom * 4096);
           }));
    return 0;
}
