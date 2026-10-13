#include "viewport_target.h"

#include <glad/gl.h>

#include <algorithm>

namespace tbcad::app {

ViewportTarget::~ViewportTarget() {
    release();
}

void ViewportTarget::release() {
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
    if (fbo_ && width == width_ && height == height_)
        return;
    release();
    width_ = width;
    height_ = height;
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ViewportTarget::bind() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
}

void ViewportTarget::unbindToWindow(int windowWidth, int windowHeight) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowWidth, windowHeight);
}

bool ViewportTarget::read(std::uint8_t* rgba) const {
    if (!fbo_)
        return false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR;
}

}  // namespace tbcad::app
