#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tbcad::platform {

struct FileFilter {
    std::string name;     // "TBCAD drawings"
    std::string pattern;  // "*.tbcad" (several: "*.dxf;*.dwg")
};

// Native file dialogs. Only Windows has them (IFileDialog); elsewhere nativeDialogsAvailable() is
// false and the application shows its own ImGui file browser instead.
bool nativeDialogsAvailable();

// `startFolder` is where the dialog opens; empty or missing means the user's Documents folder.
// `ownerWindow` is the HWND on Windows (may be null). Returns nullopt when the user cancels.
std::optional<std::filesystem::path> showOpenDialog(void* ownerWindow,
                                                    const std::vector<FileFilter>& filters,
                                                    const std::string& title,
                                                    const std::filesystem::path& startFolder = {});
// `defaultExtension` without the dot ("tbcad"). `suggestedName` may be empty.
std::optional<std::filesystem::path> showSaveDialog(void* ownerWindow,
                                                    const std::vector<FileFilter>& filters,
                                                    const std::string& title,
                                                    const std::string& defaultExtension,
                                                    const std::string& suggestedName,
                                                    const std::filesystem::path& startFolder = {});

}  // namespace tbcad::platform
