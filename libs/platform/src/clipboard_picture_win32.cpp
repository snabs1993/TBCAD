#include <windows.h>

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

}  // namespace tbcad::platform
