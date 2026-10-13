#include "dynlib.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace tbcad::plugin {

#if defined(_WIN32)

DynLib::~DynLib() {
    if (handle_)
        FreeLibrary(static_cast<HMODULE>(handle_));
}

std::string DynLib::open(const std::filesystem::path& file) {
    handle_ = LoadLibraryW(file.c_str());
    if (handle_)
        return {};
    return "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
}

void* DynLib::symbol(const char* name) const {
    return handle_ ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name))
                   : nullptr;
}

const char* libraryExtension() {
    return ".dll";
}

#else

DynLib::~DynLib() {
    if (handle_)
        dlclose(handle_);
}

std::string DynLib::open(const std::filesystem::path& file) {
    handle_ = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_)
        return {};
    const char* err = dlerror();
    return err ? err : "dlopen failed";
}

void* DynLib::symbol(const char* name) const {
    return handle_ ? dlsym(handle_, name) : nullptr;
}

const char* libraryExtension() {
#if defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

#endif

DynLib& DynLib::operator=(DynLib&& o) noexcept {
    if (this != &o) {
        this->~DynLib();
        handle_ = o.handle_;
        o.handle_ = nullptr;
    }
    return *this;
}

}  // namespace tbcad::plugin
