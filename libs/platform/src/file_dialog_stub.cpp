#include "tbcad/platform/file_dialog.h"

namespace tbcad::platform {

bool nativeDialogsAvailable() {
    return false;
}

std::optional<std::filesystem::path> showOpenDialog(void*, const std::vector<FileFilter>&,
                                                    const std::string&) {
    return std::nullopt;
}

std::optional<std::filesystem::path> showSaveDialog(void*, const std::vector<FileFilter>&,
                                                    const std::string&, const std::string&,
                                                    const std::string&) {
    return std::nullopt;
}

}  // namespace tbcad::platform
