#pragma once

#include <memory>
#include <string>

namespace tbcad::render::testing {

// True when the environment variable is set to a non-empty value.
bool envIsSet(const char* name);

// An OpenGL 3.3 core context without a visible window, for tests and the benchmark.
//   Linux:   EGL surfaceless context (Mesa llvmpipe on machines without a GPU), libEGL is
//            loaded at run time.
//   Windows: a hidden window with a WGL context (the GPU driver, or Mesa's opengl32.dll).
// create() returns null with `why` set when no context can be made.
class OffscreenGLContext {
public:
    static std::unique_ptr<OffscreenGLContext> create(std::string* why);
    ~OffscreenGLContext();
    OffscreenGLContext(const OffscreenGLContext&) = delete;
    OffscreenGLContext& operator=(const OffscreenGLContext&) = delete;

    // Suitable as a GLLoadProc once a context created by this class is current.
    static void* loadProc(const char* name);

private:
    OffscreenGLContext();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tbcad::render::testing
