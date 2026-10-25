#include <windows.h>

#include <cstring>
#include <string>

#include "tbcad/platform/clipboard_picture.h"

namespace tbcad::platform {

std::optional<ClipboardPicture> readClipboardPicture() {
    if (!IsClipboardFormatAvailable(CF_DIB) || !OpenClipboard(nullptr))
        return std::nullopt;
    std::optional<ClipboardPicture> pic;
    if (HANDLE h = GetClipboardData(CF_DIB)) {
        const SIZE_T size = GlobalSize(h);
        if (const void* p = GlobalLock(h)) {
            pic = parseDib(static_cast<const std::uint8_t*>(p), size);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return pic;
}

bool writeClipboard(const std::string& utf8Text, const ClipboardPicture* pic) {
    auto toGlobal = [](const void* data, SIZE_T size) -> HGLOBAL {
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!g)
            return nullptr;
        void* p = GlobalLock(g);
        if (!p) {
            GlobalFree(g);
            return nullptr;
        }
        std::memcpy(p, data, size);
        GlobalUnlock(g);
        return g;
    };
    std::vector<std::uint8_t> dib;
    if (pic)
        dib = buildDib(*pic);
    std::wstring wide;
    if (!utf8Text.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8Text.data(),
                                          static_cast<int>(utf8Text.size()), nullptr, 0);
        wide.resize(static_cast<std::size_t>(n > 0 ? n : 0));
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, utf8Text.data(), static_cast<int>(utf8Text.size()),
                                wide.data(), n);
    }
    if (dib.empty() && wide.empty())
        return false;
    if (!OpenClipboard(nullptr))
        return false;
    EmptyClipboard();
    bool ok = false;
    if (!dib.empty()) {
        if (HGLOBAL g = toGlobal(dib.data(), dib.size())) {
            if (SetClipboardData(CF_DIB, g))
                ok = true;
            else
                GlobalFree(g);
        }
    }
    if (!wide.empty()) {
        if (HGLOBAL g = toGlobal(wide.c_str(), (wide.size() + 1) * sizeof(wchar_t))) {
            if (SetClipboardData(CF_UNICODETEXT, g))
                ok = true;
            else
                GlobalFree(g);
        }
    }
    CloseClipboard();
    return ok;
}

}  // namespace tbcad::platform
