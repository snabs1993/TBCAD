#include "tbcad/platform/external_link.h"

#include <algorithm>
#include <array>
#include <cctype>

#include "tbcad/platform/paths.h"

#if defined(_WIN32)
#include <shellapi.h>
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace tbcad::platform {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Files that run when opened.
bool runsCode(std::string path) {
    static constexpr std::array<std::string_view, 33> kBlocked = {
        "exe", "com", "bat", "cmd", "scr",  "msi",    "msp", "pif", "ps1", "psm1", "vbs",
        "vbe", "js",  "jse", "wsf", "wsh",  "hta",    "reg", "dll", "cpl", "lnk",  "url",
        "jar", "sh",  "py",  "app", "appx", "gadget", "inf", "scf", "msc", "chm",  "apk"};
    while (!path.empty() && (path.back() == ' ' || path.back() == '.'))
        path.pop_back();  // "a.exe " and "a.exe." open as a.exe on Windows
    const auto dot = path.find_last_of('.');
    const auto slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return false;
    const std::string ext = lower(path.substr(dot + 1));
    return std::find(kBlocked.begin(), kBlocked.end(), ext) != kBlocked.end();
}

}  // namespace

LinkCheck checkLink(std::string_view text) {
    LinkCheck r;
    std::string t(text);
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front())))
        t.erase(t.begin());
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
        t.pop_back();
    if (t.empty()) {
        r.reason = "The link is empty.";
        return r;
    }
    for (unsigned char c : t)
        if (c < 0x20 || c == 0x7f) {
            r.reason = "The link contains control characters.";
            return r;
        }
    // A scheme is two or more letters before the colon; "C:" is a drive.
    std::size_t colon = t.find(':');
    bool scheme = colon != std::string::npos && colon >= 2;
    for (std::size_t i = 0; scheme && i < colon; ++i)
        scheme = std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '+' || t[i] == '-' ||
                 t[i] == '.';
    if (scheme && !std::isalpha(static_cast<unsigned char>(t[0])))
        scheme = false;
    std::string path = t;
    if (scheme) {
        const std::string s = lower(t.substr(0, colon));
        if (s == "http" || s == "https") {
            if (t.size() <= colon + 3 || t.compare(colon, 3, "://") != 0) {
                r.reason = "Not a web address.";
                return r;
            }
            r.kind = LinkKind::Web;
            r.target = t;
            return r;
        }
        if (s == "mailto") {
            r.kind = LinkKind::Mail;
            r.target = t;
            return r;
        }
        if (s != "file") {
            r.reason = "Links of type \"" + s + ":\" are not opened.";
            return r;
        }
        path = t.substr(colon + 1);
        std::size_t slashes = 0;
        while (slashes < path.size() && path[slashes] == '/')
            ++slashes;
        if (slashes >= 3)
            path.erase(0, slashes - 1);  // file:///home/x -> /home/x; file:///C:/x -> /C:/x
        if (path.size() >= 3 && path[0] == '/' &&
            std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':')
            path.erase(path.begin());
    }
    if (path.rfind("\\\\", 0) == 0 || path.rfind("//", 0) == 0) {
        r.reason = "Network shares are not opened from a drawing.";
        return r;
    }
    if (runsCode(path)) {
        r.reason = "Programs and scripts are not opened from a drawing.";
        return r;
    }
    r.kind = (path.back() == '/' || path.back() == '\\') ? LinkKind::Folder : LinkKind::File;
    r.target = path;
    return r;
}

bool openExternal(const LinkCheck& link) {
    if (link.kind == LinkKind::Refused || link.target.empty())
        return false;
#if defined(_WIN32)
    const std::wstring wide = pathFromUtf8(link.target).wstring();
    const HINSTANCE h =
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(h) > 32;
#else
    return false;
#endif
}

}  // namespace tbcad::platform
