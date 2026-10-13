#pragma once

#include <memory>
#include <span>
#include <string>

#include "tbcad/core/color.h"
#include "tbcad/render/camera.h"
#include "tbcad/render/image.h"
#include "tbcad/render/overlay.h"
#include "tbcad/render/scene.h"
#include "tbcad/render/stats.h"

namespace tbcad::render {

// Returns the address of an OpenGL function, or null. Pass glfwGetProcAddress, an EGL/WGL
// lookup, etc. (cast to this type).
using GLLoadProc = void* (*)(const char* name);

// How highlighted (hover) and selected entities are drawn: their strokes are drawn again, solid
// and translucent, wider than the original by `extraWidthPx` pixels.
struct HighlightStyle {
    Rgba color{51, 153, 255, 110};
    double extraWidthPx = 5.0;
};

// OpenGL 3.3 core renderer. All GL calls live in src/gl/ so a Direct3D 11 backend can be added
// later. Every method must be called with the context current on one thread.
//
//   renderer.beginFrame();
//   renderer.draw(scene);      // drawing, highlight and selection
//   renderer.draw(overlay);    // previews, grips, snap markers, selection window
//   RenderStats stats = renderer.endFrame();
//
// Rendering goes to whatever framebuffer is bound.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Loads the GL functions and builds shaders. Needs OpenGL 3.3 core or newer.
    bool init(GLLoadProc loader, std::string* error = nullptr);
    bool initialized() const;
    // Frees all GL objects (context must be current). Also called by the destructor.
    void shutdown();

    void resize(int widthPx, int heightPx);
    // Also takes the viewport size from the camera.
    void setCamera(const Camera2D& camera);
    const Camera2D& camera() const;
    void setBackground(Rgba color);
    // When off every line is drawn one pixel wide. On by default.
    void setLineweightDisplay(bool on);
    void setHighlightStyle(const HighlightStyle& style);
    void setSelectionStyle(const HighlightStyle& style);

    void beginFrame();
    // Images go first (they are underlays); then the scene, then the overlay.
    void draw(std::span<const ImageDraw> images);
    void draw(Scene& scene);
    void draw(const Overlay& overlay);
    RenderStats endFrame();
    const RenderStats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tbcad::render
