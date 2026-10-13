#pragma once

#include <filesystem>
#include <string>

namespace tbcad::plugin {

// A loaded shared library. Unloaded by the destructor.
class DynLib {
public:
    DynLib() = default;
    ~DynLib();
    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;
    DynLib(DynLib&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
    DynLib& operator=(DynLib&& o) noexcept;

    // Returns an empty string on success, else the system's error text.
    std::string open(const std::filesystem::path& file);
    void* symbol(const char* name) const;  // nullptr when missing

private:
    void* handle_ = nullptr;
};

// ".dll", ".so" or ".dylib".
const char* libraryExtension();

}  // namespace tbcad::plugin
