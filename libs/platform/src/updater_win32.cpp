#include <windows.h>
#include <winhttp.h>
// clang-format off: shellapi.h needs windows.h first
#include <shellapi.h>
// clang-format on

#include <fstream>

#include "tbcad/platform/updater.h"

namespace tbcad::platform {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x = nullptr) : h(x) {}
    ~Handle() {
        if (h)
            WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

// Runs a GET and hands every chunk of the answer to `sink` (false stops).
template <class Sink>
bool get(const std::string& url, Sink&& sink, std::string& error, long long* total = nullptr) {
    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "Bad update address.";
        return false;
    }
    Handle session(WinHttpOpen(L"TBCAD-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        error = "Cannot start the network connection.";
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 20000, 30000);
    Handle conn(WinHttpConnect(session.h, std::wstring(host, uc.dwHostNameLength).c_str(),
                               uc.nPort, 0));
    if (!conn.h) {
        error = "Cannot reach the update server.";
        return false;
    }
    Handle req(WinHttpOpenRequest(conn.h, L"GET", std::wstring(path, uc.dwUrlPathLength).c_str(),
                                  nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  WINHTTP_FLAG_SECURE));
    if (!req.h) {
        error = "Cannot reach the update server.";
        return false;
    }
    const wchar_t* hdr = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
    if (!WinHttpSendRequest(req.h, hdr, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        error = "No answer from the update server (is the internet connection on?).";
        return false;
    }
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = "The update server answered " + std::to_string(status) + ".";
        return false;
    }
    if (total) {
        wchar_t len[32];
        DWORD n = sizeof len;
        *total = WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                     len, &n, WINHTTP_NO_HEADER_INDEX)
                     ? _wtoi64(len)
                     : 0;
    }
    std::string chunk;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            error = "The connection was lost.";
            return false;
        }
        if (avail == 0)
            return true;
        chunk.resize(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, chunk.data(), avail, &got)) {
            error = "The connection was lost.";
            return false;
        }
        if (!sink(chunk.data(), got))
            return false;
    }
}

}  // namespace

bool httpsGet(const std::string& url, std::string& body, std::string& error) {
    body.clear();
    return get(
        url,
        [&](const char* p, DWORD n) {
            body.append(p, n);
            return body.size() < (8u << 20);
        },
        error);
}

bool httpsDownload(const std::string& url, const std::filesystem::path& file,
                   std::atomic<float>* progress, const std::atomic<bool>* cancel,
                   std::string& error) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Cannot write " + file.string();
        return false;
    }
    long long total = 0, done = 0;
    // The size is known only after the headers; the sink reads it through `total`.
    const bool ok = get(
        url,
        [&](const char* p, DWORD n) {
            out.write(p, n);
            done += n;
            if (progress && total > 0)
                *progress = static_cast<float>(done) / static_cast<float>(total);
            return out.good() && !(cancel && cancel->load());
        },
        error, &total);
    out.close();
    if (ok && out.fail()) {
        error = "Cannot write " + file.string();
        return false;
    }
    return ok;
}

std::filesystem::path selfExecutable() {
    wchar_t buf[4096];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
    return n > 0 && n < 4096 ? std::filesystem::path(std::wstring(buf, n)) : std::filesystem::path();
}

bool launchDetached(const std::filesystem::path& exe, const std::vector<std::string>& args) {
    // ShellExecute (not CreateProcess) so the installer's own "run as administrator" request is
    // honoured for an install in Program Files.
    std::wstring params;
    for (const auto& a : args)
        params += (params.empty() ? L"" : L" ") + widen(a);
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(),
                                                           params.c_str(), nullptr, SW_SHOWNORMAL));
    return r > 32;
}

}  // namespace tbcad::platform
