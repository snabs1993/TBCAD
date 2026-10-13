#pragma once

#include <cstdint>

namespace tbcad::app {

// An offscreen colour target the drawing is rendered into; the UI then shows it in the central
// area between the docked panels. Needs a current OpenGL context (loaded through the renderer).
class ViewportTarget {
public:
    ViewportTarget() = default;
    ~ViewportTarget();
    ViewportTarget(const ViewportTarget&) = delete;
    ViewportTarget& operator=(const ViewportTarget&) = delete;

    // (Re)creates the target when the size changed. Sizes below 1 are clamped.
    void resize(int width, int height);
    void bind();
    static void unbindToWindow(int windowWidth, int windowHeight);
    // OpenGL texture name, for ImGui::Image.
    std::uint32_t texture() const { return texture_; }
    int width() const { return width_; }
    int height() const { return height_; }
    // Copies the pixels (RGBA, bottom row first) for screenshots and tests.
    bool read(std::uint8_t* rgba) const;
    void release();

private:
    std::uint32_t fbo_ = 0;
    std::uint32_t texture_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace tbcad::app
