#include "gl_context.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

#include <cstdlib>

namespace tbcad::render::testing {

bool envIsSet(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr)
        return false;
    bool set = value[0] != '\0';
    std::free(value);
    return set;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
#endif
}

#if defined(__linux__)
namespace {

using EGLint = int;
using EGLBoolean = unsigned int;
using EGLenum = unsigned int;
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;

constexpr EGLint kEglNone = 0x3038;
constexpr EGLint kEglSurfaceType = 0x3033;
constexpr EGLint kEglPbufferBit = 0x0001;
constexpr EGLint kEglRenderableType = 0x3040;
constexpr EGLint kEglOpenglBit = 0x0008;
constexpr EGLenum kEglOpenglApi = 0x30A2;
constexpr EGLint kEglContextMajorVersion = 0x3098;
constexpr EGLint kEglContextMinorVersion = 0x30FB;
constexpr EGLint kEglContextProfileMask = 0x30FD;
constexpr EGLint kEglContextCoreProfileBit = 0x1;
constexpr EGLenum kEglPlatformSurfacelessMesa = 0x31DD;

struct Egl {
    void* lib = nullptr;
    void* (*getProcAddress)(const char*) = nullptr;
    EGLDisplay (*getDisplay)(void*) = nullptr;
    EGLDisplay (*getPlatformDisplay)(EGLenum, void*, const EGLint*) = nullptr;
    EGLBoolean (*initialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
    EGLBoolean (*bindApi)(EGLenum) = nullptr;
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    EGLBoolean (*destroyContext)(EGLDisplay, EGLContext) = nullptr;
    EGLBoolean (*terminate)(EGLDisplay) = nullptr;
};

Egl& egl() {
    static Egl e;
    return e;
}

template <class T>
bool sym(T& out, const char* name) {
    out = reinterpret_cast<T>(dlsym(egl().lib, name));
    return out != nullptr;
}

}  // namespace

struct OffscreenGLContext::Impl {
    EGLDisplay display = nullptr;
    EGLContext context = nullptr;
};

OffscreenGLContext::OffscreenGLContext() : impl_(std::make_unique<Impl>()) {}

OffscreenGLContext::~OffscreenGLContext() {
    Egl& e = egl();
    if (impl_->display) {
        e.makeCurrent(impl_->display, nullptr, nullptr, nullptr);
        if (impl_->context)
            e.destroyContext(impl_->display, impl_->context);
        e.terminate(impl_->display);
    }
}

std::unique_ptr<OffscreenGLContext> OffscreenGLContext::create(std::string* why) {
    auto fail = [&](const char* msg) -> std::unique_ptr<OffscreenGLContext> {
        if (why)
            *why = msg;
        return nullptr;
    };
    Egl& e = egl();
    if (!e.lib) {
        e.lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!e.lib)
            return fail("libEGL.so.1 not found");
    }
    if (!sym(e.getProcAddress, "eglGetProcAddress") || !sym(e.getDisplay, "eglGetDisplay") ||
        !sym(e.initialize, "eglInitialize") || !sym(e.bindApi, "eglBindAPI") ||
        !sym(e.chooseConfig, "eglChooseConfig") || !sym(e.createContext, "eglCreateContext") ||
        !sym(e.makeCurrent, "eglMakeCurrent") || !sym(e.destroyContext, "eglDestroyContext") ||
        !sym(e.terminate, "eglTerminate"))
        return fail("libEGL.so.1 lacks required functions");

    std::unique_ptr<OffscreenGLContext> ctx(new OffscreenGLContext());
    EGLDisplay display = nullptr;
    // Prefer Mesa's surfaceless platform: needs no display server and no GPU.
    e.getPlatformDisplay = reinterpret_cast<decltype(e.getPlatformDisplay)>(
        e.getProcAddress("eglGetPlatformDisplayEXT"));
    EGLint major = 0, minor = 0;
    if (e.getPlatformDisplay) {
        display = e.getPlatformDisplay(kEglPlatformSurfacelessMesa, nullptr, nullptr);
        if (display && !e.initialize(display, &major, &minor))
            display = nullptr;
    }
    if (!display) {
        display = e.getDisplay(nullptr);
        if (!display || !e.initialize(display, &major, &minor))
            return fail("eglInitialize failed");
    }
    ctx->impl_->display = display;
    if (!e.bindApi(kEglOpenglApi))
        return fail("EGL has no desktop OpenGL support");
    // The default surface type is EGL_WINDOW_BIT, which surfaceless displays do not offer.
    const EGLint cfgAttribs[] = {kEglSurfaceType, kEglPbufferBit, kEglRenderableType, kEglOpenglBit,
                                 kEglNone};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!e.chooseConfig(display, cfgAttribs, &config, 1, &count) || count < 1)
        return fail("no EGL config for OpenGL");
    const EGLint ctxAttribs[] = {
        kEglContextMajorVersion,   3,       kEglContextMinorVersion, 3, kEglContextProfileMask,
        kEglContextCoreProfileBit, kEglNone};
    ctx->impl_->context = e.createContext(display, config, nullptr, ctxAttribs);
    if (!ctx->impl_->context)
        return fail("could not create an OpenGL 3.3 core context");
    if (!e.makeCurrent(display, nullptr, nullptr, ctx->impl_->context))
        return fail("eglMakeCurrent failed (no surfaceless context support)");
    return ctx;
}

void* OffscreenGLContext::loadProc(const char* name) {
    Egl& e = egl();
    return e.getProcAddress ? e.getProcAddress(name) : nullptr;
}

#elif defined(_WIN32)

namespace {

constexpr int kWglContextMajorVersionArb = 0x2091;
constexpr int kWglContextMinorVersionArb = 0x2092;
constexpr int kWglContextProfileMaskArb = 0x9126;
constexpr int kWglContextCoreProfileBitArb = 0x00000001;

using CreateContextAttribsFn = HGLRC(WINAPI*)(HDC, HGLRC, const int*);

}  // namespace

struct OffscreenGLContext::Impl {
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
};

OffscreenGLContext::OffscreenGLContext() : impl_(std::make_unique<Impl>()) {}

OffscreenGLContext::~OffscreenGLContext() {
    wglMakeCurrent(nullptr, nullptr);
    if (impl_->rc)
        wglDeleteContext(impl_->rc);
    if (impl_->dc && impl_->window)
        ReleaseDC(impl_->window, impl_->dc);
    if (impl_->window)
        DestroyWindow(impl_->window);
}

std::unique_ptr<OffscreenGLContext> OffscreenGLContext::create(std::string* why) {
    auto fail = [&](const char* msg) -> std::unique_ptr<OffscreenGLContext> {
        if (why)
            *why = msg;
        return nullptr;
    };
    static const wchar_t kClass[] = L"TbcadOffscreenGL";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        if (!RegisterClassW(&wc))
            return fail("RegisterClass failed");
        registered = true;
    }
    std::unique_ptr<OffscreenGLContext> ctx(new OffscreenGLContext());
    Impl& d = *ctx->impl_;
    d.window = CreateWindowW(kClass, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!d.window)
        return fail("CreateWindow failed");
    d.dc = GetDC(d.window);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int format = ChoosePixelFormat(d.dc, &pfd);
    if (!format || !SetPixelFormat(d.dc, format, &pfd))
        return fail("no pixel format for OpenGL");
    HGLRC legacy = wglCreateContext(d.dc);
    if (!legacy)
        return fail("wglCreateContext failed");
    if (!wglMakeCurrent(d.dc, legacy)) {
        wglDeleteContext(legacy);
        return fail("wglMakeCurrent failed");
    }
    auto createAttribs = reinterpret_cast<CreateContextAttribsFn>(
        reinterpret_cast<void*>(wglGetProcAddress("wglCreateContextAttribsARB")));
    if (!createAttribs) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(legacy);
        return fail("driver has no wglCreateContextAttribsARB (OpenGL 1.1 software renderer?)");
    }
    const int attribs[] = {
        kWglContextMajorVersionArb,   3, kWglContextMinorVersionArb, 3, kWglContextProfileMaskArb,
        kWglContextCoreProfileBitArb, 0};
    d.rc = createAttribs(d.dc, nullptr, attribs);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    if (!d.rc)
        return fail("could not create an OpenGL 3.3 core context");
    if (!wglMakeCurrent(d.dc, d.rc))
        return fail("wglMakeCurrent failed");
    return ctx;
}

void* OffscreenGLContext::loadProc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    // wglGetProcAddress returns small integers or null for GL 1.1 functions.
    if (p == nullptr || p == reinterpret_cast<void*>(1) || p == reinterpret_cast<void*>(2) ||
        p == reinterpret_cast<void*>(3) || p == reinterpret_cast<void*>(-1)) {
        static HMODULE lib = LoadLibraryW(L"opengl32.dll");
        p = reinterpret_cast<void*>(GetProcAddress(lib, name));
    }
    return p;
}

#else

struct OffscreenGLContext::Impl {};
OffscreenGLContext::OffscreenGLContext() = default;
OffscreenGLContext::~OffscreenGLContext() = default;
std::unique_ptr<OffscreenGLContext> OffscreenGLContext::create(std::string* why) {
    if (why)
        *why = "offscreen GL contexts are not implemented on this platform";
    return nullptr;
}
void* OffscreenGLContext::loadProc(const char*) {
    return nullptr;
}

#endif

}  // namespace tbcad::render::testing
