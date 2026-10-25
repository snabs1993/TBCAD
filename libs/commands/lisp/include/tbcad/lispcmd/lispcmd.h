#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "tbcad/cmd/registry.h"

namespace tbcad::lispcmd {

// Called by the generated tbcad::registerAllCommands (see module.cmake).
// Commands: LISP, APPLOAD, and one per (defun c:NAME ...) a loaded program defines.
void registerCommands(cmd::Registry& registry);

// Runs the startup suite: acad.lsp in `settingsFolder`, then every file listed in
// startup-suite.txt there (one path per line; APPLOAD's Startup option adds to it). Problems go to
// `print`. Does nothing when the LISP command is not registered or the files are missing.
void loadStartup(cmd::Registry& registry, const std::filesystem::path& settingsFolder,
                 const std::function<void(const std::string&)>& print);

}  // namespace tbcad::lispcmd
