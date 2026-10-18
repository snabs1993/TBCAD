#include "tbcad/platform/file_lock.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace tbcad::platform {

namespace fs = std::filesystem;

namespace {

fs::path lockPathOf(const fs::path& drawing) {
    fs::path p = drawing;
    p.replace_filename("." + drawing.filename().string() + ".lock");
    return p;
}

}  // namespace

struct FileLock::Impl {
    fs::path path;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
};

FileLock::FileLock(const fs::path& drawing) : impl_(std::make_unique<Impl>()) {
    impl_->path = lockPathOf(drawing);
#ifdef _WIN32
    // Shared with nobody and deleted when closed: a second open of the same name fails with a
    // sharing violation while the first holder lives.
    HANDLE h = CreateFileW(impl_->path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        impl_->handle = h;
        held_ = true;
    } else if (GetLastError() == ERROR_SHARING_VIOLATION) {
        elsewhere_ = true;
    }
#else
    const int fd = ::open(impl_->path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            impl_->fd = fd;
            held_ = true;
        } else {
            elsewhere_ = errno == EWOULDBLOCK;
            ::close(fd);
        }
    }
#endif
}

FileLock::~FileLock() {
#ifdef _WIN32
    if (impl_ && impl_->handle != INVALID_HANDLE_VALUE)
        CloseHandle(impl_->handle);
#else
    if (impl_ && impl_->fd >= 0) {
        // Remove the file while still holding the lock, so a new holder cannot be confused by it.
        ::unlink(impl_->path.c_str());
        ::close(impl_->fd);
    }
#endif
}

}  // namespace tbcad::platform
