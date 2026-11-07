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

    // Anti-aliasing: the drawing is rendered into a multisampled buffer with this many samples
    // (1 = off), then resolved into the texture. Takes effect at the next resize.
    void setSamples(int samples);
    int samples() const { return samples_; }
    // (Re)creates the target when the size changed. Sizes below 1 are clamped.
    void resize(int width, int height);
    void bind();
    // Copies the multisampled picture into the texture (no-op without anti-aliasing). Binding
    // another target, unbindToWindow and read() do it for the bound target.
    void resolve();
    static void unbindToWindow(int windowWidth, int windowHeight);
    // OpenGL texture name, for ImGui::Image.
    std::uint32_t texture() const { return texture_; }
    int width() const { return width_; }
    int height() const { return height_; }
    // Copies the pixels (RGBA, bottom row first) for screenshots and tests.
    bool read(std::uint8_t* rgba);
    void release();

private:
    std::uint32_t fbo_ = 0;
    std::uint32_t texture_ = 0;
    std::uint32_t msFbo_ = 0;    // multisampled framebuffer drawn into, when samples_ > 1
    std::uint32_t msColor_ = 0;  // its colour renderbuffer
    int samples_ = 1;
    int builtSamples_ = 1;
    bool dirty_ = false;  // drawn into msFbo_ since the last resolve
    int width_ = 0;
    int height_ = 0;
};

}  // namespace tbcad::app
