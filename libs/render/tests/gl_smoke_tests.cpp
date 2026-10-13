// GL smoke tests: draw into an offscreen framebuffer and read pixels back. They run wherever an
// OpenGL 3.3 context can be created without a window (Mesa llvmpipe through EGL on Linux CI, the
// GPU driver on Windows). Without one they are skipped with a message, unless the environment
// variable TBCAD_REQUIRE_GL is set, which turns "no context" into a failure.
#include <doctest/doctest.h>
#include <glad/gl.h>

#include <iostream>
#include <memory>

#include "gl_context.h"
#include "tbcad/render/renderer.h"

using namespace tbcad;
using namespace tbcad::render;

namespace {

constexpr int kW = 256;
constexpr int kH = 128;
constexpr Rgba kBackground{0, 0, 0, 255};

struct Rgb {
    int r, g, b;
};

class GlFixture {
public:
    // Returns null (after reporting) when GL is unavailable.
    static GlFixture* get() {
        static GlFixture* instance = create();
        return instance;
    }

    Renderer renderer;

    void beginTarget() { glBindFramebuffer(GL_FRAMEBUFFER, fbo_); }

    Rgb pixel(int x, int yFromBottom) const {
        unsigned char px[4] = {};
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glReadPixels(x, yFromBottom, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        return {px[0], px[1], px[2]};
    }

    int countNonBackground() const {
        std::vector<unsigned char> buf(static_cast<std::size_t>(kW) * kH * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
        int n = 0;
        for (std::size_t i = 0; i < buf.size(); i += 4)
            if (buf[i] || buf[i + 1] || buf[i + 2])
                ++n;
        return n;
    }

private:
    static GlFixture* create() {
        static std::unique_ptr<testing::OffscreenGLContext> context;
        static GlFixture fixture;
        std::string why;
        context = testing::OffscreenGLContext::create(&why);
        if (!context) {
            report("no OpenGL context: " + why);
            return nullptr;
        }
        std::string error;
        if (!fixture.renderer.init(&testing::OffscreenGLContext::loadProc, &error)) {
            report("renderer init failed: " + error);
            return nullptr;
        }
        glGenTextures(1, &fixture.tex_);
        glBindTexture(GL_TEXTURE_2D, fixture.tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &fixture.fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fixture.fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fixture.tex_,
                               0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            report("offscreen framebuffer is incomplete");
            return nullptr;
        }
        std::cout << "[render gl smoke] " << reinterpret_cast<const char*>(glGetString(GL_RENDERER))
                  << ", GL " << reinterpret_cast<const char*>(glGetString(GL_VERSION)) << "\n";
        fixture.renderer.setBackground(kBackground);
        return &fixture;
    }

    static void report(const std::string& msg) {
        if (testing::envIsSet("TBCAD_REQUIRE_GL")) {
            FAIL(msg);
        } else {
            std::cout << "[render gl smoke] SKIPPED: " << msg << "\n";
        }
    }

    GLuint tex_ = 0, fbo_ = 0;
};

Camera2D camera(Vec2 center = {0, 0}, double ppmm = 1) {
    Camera2D c;
    c.center = center;
    c.pixelsPerMm = ppmm;
    c.viewportW = kW;
    c.viewportH = kH;
    return c;
}

DisplayStyle style(Rgba color, double lw = 0, LayerId layer = {1}) {
    DisplayStyle s;
    s.color = color;
    s.lineweight = lw;
    s.layer = layer;
    return s;
}

DisplayList line(Vec2 a, Vec2 b, DisplayStyle st) {
    DisplayList l;
    l.items.push_back(DisplayPolyline{{a, b}, false, st});
    return l;
}

bool isRed(Rgb p) {
    return p.r > 200 && p.g < 60 && p.b < 60;
}
bool isBlack(Rgb p) {
    return p.r < 8 && p.g < 8 && p.b < 8;
}

void drawFrame(GlFixture& f, Scene& scene, const Camera2D& cam, const Overlay* overlay = nullptr) {
    f.beginTarget();
    f.renderer.setCamera(cam);
    f.renderer.beginFrame();
    f.renderer.draw(scene);
    if (overlay)
        f.renderer.draw(*overlay);
    f.renderer.endFrame();
}

constexpr Rgba kRed{255, 0, 0, 255};

}  // namespace

TEST_CASE("gl: one thin line shows up where it should") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    Scene scene;
    // World y = 0.5 is the middle of pixel row 64 (counted from the bottom).
    scene.upsert({1}, line({-50, 0.5}, {50, 0.5}, style(kRed)), 0);
    drawFrame(*f, scene, camera());

    CHECK(isRed(f->pixel(128, 64)));
    CHECK(isRed(f->pixel(100, 64)));
    CHECK(isBlack(f->pixel(128, 70)));
    CHECK(isBlack(f->pixel(128, 58)));
    CHECK(isBlack(f->pixel(10, 64)));   // left of x = -50
    CHECK(isBlack(f->pixel(250, 64)));  // right of x = 50
    const RenderStats& st = f->renderer.stats();
    CHECK(st.drawCalls >= 1);
    CHECK(st.vertices >= 2);
    CHECK(st.tilesDrawn == 1);
}

TEST_CASE("gl: thick lines expand to the requested width") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    Scene scene;
    scene.upsert({1}, line({0.5, -40}, {0.5, 40}, style(kRed, 8.0)), 0);  // 8 px wide
    drawFrame(*f, scene, camera());
    for (int dx = -3; dx <= 3; ++dx)
        CHECK(isRed(f->pixel(128 + dx, 64)));
    CHECK(isBlack(f->pixel(128 + 6, 64)));
    CHECK(isBlack(f->pixel(128 - 6, 64)));

    f->renderer.setLineweightDisplay(false);  // everything one pixel wide again
    drawFrame(*f, scene, camera());
    CHECK(isRed(f->pixel(128, 64)));
    CHECK(isBlack(f->pixel(128 + 3, 64)));
    f->renderer.setLineweightDisplay(true);
}

TEST_CASE("gl: dashed lines alternate on the screen") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    const std::vector<double> pattern = {10, -10};
    DisplayStyle st = style(kRed);
    st.pattern = &pattern;
    Scene scene;
    scene.upsert({1}, line({-120, 0.5}, {120, 0.5}, st), 0);
    drawFrame(*f, scene, camera());
    int red = 0, black = 0;
    for (int x = 20; x < 236; ++x) {
        Rgb p = f->pixel(x, 64);
        if (isRed(p))
            ++red;
        else if (isBlack(p))
            ++black;
    }
    // About half the pixels are dash, and both occur in long runs.
    CHECK(red > 80);
    CHECK(black > 80);
    CHECK(red + black >= 214);
    // Distance 0 of the line is at world x = -120 -> screen x = 8; dashes cover 8..18.
    CHECK(isRed(f->pixel(12, 64)));
    CHECK(isBlack(f->pixel(23, 64)));
    CHECK(isRed(f->pixel(32, 64)));
}

TEST_CASE("gl: dashes become solid when they would shimmer") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    const std::vector<double> pattern = {0.5, -0.5};  // 1 mm period at 1 px/mm
    DisplayStyle st = style(kRed);
    st.pattern = &pattern;
    Scene scene;
    scene.upsert({1}, line({-100, 0.5}, {100, 0.5}, st), 0);
    drawFrame(*f, scene, camera());
    for (int x = 40; x < 216; ++x)
        CHECK(isRed(f->pixel(x, 64)));
}

TEST_CASE("gl: fills with holes") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    DisplayFill fill;
    fill.rings = {{{-40, -40}, {40, -40}, {40, 40}, {-40, 40}},
                  {{-10, -10}, {10, -10}, {10, 10}, {-10, 10}}};
    fill.style = style({0, 255, 0, 255});
    DisplayList l;
    l.items.push_back(fill);
    Scene scene;
    scene.upsert({1}, l, 0);
    drawFrame(*f, scene, camera());
    Rgb solid = f->pixel(128 + 30, 64);
    CHECK(solid.g > 200);
    CHECK(solid.r < 20);
    CHECK(isBlack(f->pixel(128, 64)));       // the hole
    CHECK(isBlack(f->pixel(128 + 60, 64)));  // outside
}

TEST_CASE("gl: far from the origin the line is still pixel exact") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    const Vec2 c{1e8 + 0.25, -1e8 + 0.25};
    Scene scene({.tileSizeMm = 1024});
    // Same picture as the first test, translated by (1e8, -1e8).
    scene.upsert({1}, line({c.x - 50, c.y + 0.25}, {c.x + 50, c.y + 0.25}, style(kRed)), 0);
    Camera2D cam = camera(c, 1);
    // World y = c.y + 0.25 is exactly the middle of pixel row 64 again.
    cam.center.y = c.y - 0.25;
    drawFrame(*f, scene, cam);
    CHECK(isRed(f->pixel(128, 64)));
    CHECK(isBlack(f->pixel(128, 63)));
    CHECK(isBlack(f->pixel(128, 65)));

    // At extreme zoom a 1e-3 mm shift moves the line by one pixel at 1000 px/mm.
    Scene fine({.tileSizeMm = 1024});
    fine.upsert({1}, line({c.x - 0.05, c.y + 0.0005}, {c.x + 0.05, c.y + 0.0005}, style(kRed)), 0);
    Camera2D zoomed = camera({c.x, c.y}, 1000);
    drawFrame(*f, fine, zoomed);
    // y offset 0.0005 mm * 1000 px/mm = 0.5 px above the centre line: pixel row 64 (centre 64.5).
    CHECK(isRed(f->pixel(128, 64)));
    CHECK(isBlack(f->pixel(128, 63)));
    CHECK(isBlack(f->pixel(128, 66)));
}

TEST_CASE("gl: hidden layers and removed entities disappear") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    Scene scene;
    scene.upsert({1}, line({-50, 0.5}, {50, 0.5}, style(kRed, 0, {7})), 0);
    drawFrame(*f, scene, camera());
    CHECK(f->countNonBackground() > 0);
    scene.setLayerVisible({7}, false);
    drawFrame(*f, scene, camera());
    CHECK(f->countNonBackground() == 0);
    scene.setLayerVisible({7}, true);
    drawFrame(*f, scene, camera());
    CHECK(f->countNonBackground() > 0);
    scene.remove({1});
    drawFrame(*f, scene, camera());
    CHECK(f->countNonBackground() == 0);
    CHECK(f->renderer.stats().drawCalls == 0);
}

TEST_CASE("gl: moving an entity re-uploads its batch") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    Scene scene;
    scene.upsert({1}, line({-50, 0.5}, {50, 0.5}, style(kRed)), 0);
    drawFrame(*f, scene, camera());
    CHECK(isRed(f->pixel(128, 64)));
    scene.upsert({1}, line({-50, 30.5}, {50, 30.5}, style(kRed)), 0);
    drawFrame(*f, scene, camera());
    CHECK(isBlack(f->pixel(128, 64)));
    CHECK(isRed(f->pixel(128, 94)));
    CHECK(f->renderer.stats().uploadedBytes > 0);
    drawFrame(*f, scene, camera());  // unchanged: nothing to upload
    CHECK(f->renderer.stats().uploadedBytes == 0);
}

TEST_CASE("gl: selected entities get a glow, others do not") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    Scene scene;
    scene.upsert({1}, line({-50, 0.5}, {50, 0.5}, style(kRed)), 0);
    scene.upsert({2}, line({-50, 40.5}, {50, 40.5}, style(kRed)), 0);
    EntityId sel[] = {{1}};
    scene.setSelected(sel);
    drawFrame(*f, scene, camera());
    Rgb glow = f->pixel(128, 64 + 2);  // 2 px above the selected line (glow is 5 px wide)
    CHECK(!isBlack(glow));
    CHECK(isBlack(f->pixel(128, 64 + 40 + 2)));  // the unselected line has none
    scene.setSelected({});
    drawFrame(*f, scene, camera());
    CHECK(isBlack(f->pixel(128, 64 + 2)));
}

TEST_CASE("gl: overlay draws in screen space with y down") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    Scene scene;
    Overlay overlay;
    overlay.line(Space::Screen, {20, 20.5}, {120, 20.5}, OverlayStyle{kRed, 1.0, 0, 0});
    overlay.marker(Space::World, {0, 0}, MarkerKind::Square, 20, {0, 255, 0, 255}, true);
    drawFrame(*f, scene, camera(), &overlay);
    // Screen row 20 from the top is row kH-1-20 from the bottom.
    CHECK(isRed(f->pixel(60, kH - 1 - 20)));
    CHECK(isBlack(f->pixel(60, kH - 1 - 30)));
    Rgb marker = f->pixel(128, 64);
    CHECK(marker.g > 200);
    CHECK(isBlack(f->pixel(128 + 30, 64)));
}

TEST_CASE("gl: the background is cleared and white switches to black on a light one") {
    GlFixture* f = GlFixture::get();
    if (!f)
        return;
    Scene scene;
    scene.upsert({1}, line({-50, 0.5}, {50, 0.5}, style({255, 255, 255, 255})), 0);
    f->renderer.setBackground({230, 230, 230, 255});
    drawFrame(*f, scene, camera());
    Rgb bg = f->pixel(10, 10);
    CHECK(bg.r == 230);
    Rgb line = f->pixel(128, 64);
    CHECK(line.r < 20);  // white became black
    f->renderer.setBackground(kBackground);
}
