#include "viewport_target.h"

#include <glad/gl.h>

#include <algorithm>

namespace tbcad::app {

namespace {
ViewportTarget* gBound = nullptr;  // the target last bound for drawing
}

ViewportTarget::~ViewportTarget() {
    release();
}

void ViewportTarget::release() {
    if (gBound == this)
        gBound = nullptr;
    if (msFbo_)
        glDeleteFramebuffers(1, &msFbo_);
    if (msColor_)
        glDeleteRenderbuffers(1, &msColor_);
    msFbo_ = msColor_ = 0;
    dirty_ = false;
    if (fbo_)
        glDeleteFramebuffers(1, &fbo_);
    if (texture_)
        glDeleteTextures(1, &texture_);
    fbo_ = texture_ = 0;
    width_ = height_ = 0;
}

void ViewportTarget::resize(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (fbo_ && width == width_ && height == height_ && builtSamples_ == samples_)
        return;
    release();
    width_ = width;
    height_ = height;
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    // Shown pixel for pixel: linear filtering would blur every line when the view's corner
    // falls between pixels.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
    builtSamples_ = 1;
    if (samples_ > 1) {
        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        const int n = std::min(samples_, static_cast<int>(maxSamples));
        if (n > 1) {
            glGenRenderbuffers(1, &msColor_);
            glBindRenderbuffer(GL_RENDERBUFFER, msColor_);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, n, GL_RGBA8, width_, height_);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glGenFramebuffers(1, &msFbo_);
            glBindFramebuffer(GL_FRAMEBUFFER, msFbo_);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                      msColor_);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                // No multisampling on this driver: draw straight into the texture.
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &msFbo_);
                glDeleteRenderbuffers(1, &msColor_);
                msFbo_ = msColor_ = 0;
            }
        }
    }
    // Remember the request even when it fell back, so resize does not retry every frame.
    builtSamples_ = samples_;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ViewportTarget::setSamples(int samples) {
    samples_ = std::clamp(samples, 1, 16);
}

void ViewportTarget::resolve() {
    if (!msFbo_ || !dirty_)
        return;
    dirty_ = false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ViewportTarget::bind() {
    if (gBound && gBound != this)
        gBound->resolve();
    gBound = this;
    dirty_ = msFbo_ != 0;
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo_ ? msFbo_ : fbo_);
    glViewport(0, 0, width_, height_);
}

void ViewportTarget::unbindToWindow(int windowWidth, int windowHeight) {
    if (gBound)
        gBound->resolve();
    gBound = nullptr;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowWidth, windowHeight);
}

bool ViewportTarget::read(std::uint8_t* rgba) {
    if (!fbo_)
        return false;
    resolve();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR;
}

}  // namespace tbcad::app
