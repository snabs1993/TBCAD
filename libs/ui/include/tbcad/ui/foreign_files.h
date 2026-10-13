// Plug-in points for file formats other than .tbcad. A backend (the clean-room DWG reader, or
// any other) registers itself in the application; AppCore only knows these two callbacks.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::ui {

struct ForeignOpenResult {
    std::unique_ptr<Document> document{};
    std::string format{};                 // "DWG R2018" ... shown in the log
    std::vector<std::string> messages{};  // warnings / summary lines for the command log
};

// Reads a file of a foreign format into a document. The file is never modified.
using ForeignReadFn = std::function<Result<ForeignOpenResult>(const std::filesystem::path&)>;

}  // namespace tbcad::ui
