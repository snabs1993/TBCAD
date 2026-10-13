// Atomic save. This file holds the only platform-specific code in libs/io (sync to disk and the
// final replace), kept small and behind two helper functions.

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <system_error>

#include "tbcad/io/tbcad.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tbcad::io {

namespace {

namespace fs = std::filesystem;

Error ioError(const std::string& what, const fs::path& p, const std::error_code& ec = {}) {
    std::string m = what + " " + p.string();
    if (ec)
        m += ": " + ec.message();
    return makeError(ErrorCode::Io, std::move(m));
}

// Writes the whole buffer to `path` (created or truncated) and forces it to stable storage.
Result<void> writeAndSync(const fs::path& path, std::span<const std::uint8_t> data) {
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return ioError("cannot create", path);
    std::size_t done = 0;
    bool ok = true;
    while (done < data.size() && ok) {
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 30));
        DWORD wrote = 0;
        ok = WriteFile(h, data.data() + done, chunk, &wrote, nullptr) && wrote == chunk;
        done += wrote;
    }
    ok = ok && FlushFileBuffers(h);
    ok = CloseHandle(h) && ok;
    if (!ok)
        return ioError("cannot write", path);
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return ioError("cannot create", path, std::error_code(errno, std::generic_category()));
    std::size_t done = 0;
    bool ok = true;
    while (done < data.size() && ok) {
        ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n <= 0) {
            ok = false;
        } else {
            done += static_cast<std::size_t>(n);
        }
    }
    ok = ok && ::fsync(fd) == 0;
    ok = (::close(fd) == 0) && ok;
    if (!ok)
        return ioError("cannot write", path);
#endif
    return {};
}

// Makes `tmp` the new content of `target`, atomically where the platform allows, and moves the
// previous file to `backup` when it is non-empty.
Result<void> replaceFile(const fs::path& tmp, const fs::path& target, const fs::path& backup) {
    std::error_code ec;
    bool exists = fs::exists(target, ec);
#ifdef _WIN32
    if (exists) {
        BOOL ok = ReplaceFileW(target.c_str(), tmp.c_str(),
                               backup.empty() ? nullptr : backup.c_str(), 0, nullptr, nullptr);
        if (!ok)
            return ioError("cannot replace", target);
        return {};
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return ioError("cannot rename to", target);
    return {};
#else
    if (exists && !backup.empty()) {
        fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec);
        if (ec)
            return ioError("cannot create backup", backup, ec);
    }
    fs::rename(tmp, target, ec);  // atomic on POSIX file systems
    if (ec)
        return ioError("cannot rename to", target, ec);
    return {};
#endif
}

}  // namespace

Result<void> writeTbcad(const Document& doc, const fs::path& path, const WriteOptions& options) {
    Result<std::vector<std::uint8_t>> bytes = writeTbcadToMemory(doc, options);
    if (!bytes)
        return bytes.error();
    fs::path tmp = path;
    tmp += ".tmp";
    if (Result<void> r = writeAndSync(tmp, bytes.value()); !r) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return r;
    }
    if (options.simulateCrashBeforeReplace)
        return makeError(ErrorCode::Io, "simulated crash before the final replace");
    fs::path backup;
    if (options.keepBackup) {
        backup = path;
        backup += ".bak";
    }
    Result<void> r = replaceFile(tmp, path, backup);
    if (!r) {
        std::error_code ec;
        fs::remove(tmp, ec);
    }
    return r;
}

}  // namespace tbcad::io
