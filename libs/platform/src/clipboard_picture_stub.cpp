#include "tbcad/platform/clipboard_picture.h"

namespace tbcad::platform {

std::optional<ClipboardPicture> readClipboardPicture() {
    return std::nullopt;
}

bool writeClipboard(const std::string&, const ClipboardPicture*) {
    return false;
}

}  // namespace tbcad::platform
