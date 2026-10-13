#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tbcad/cmd/registry.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::plugin {

struct PluginInfo {
    std::filesystem::path file{};
    std::string name{};  // from tbcad_plugin_info, else the file name
    std::string version{};
    std::string author{};
    std::vector<std::string> commands{};
    std::string error{};  // non-empty = not loaded
};

// Loads plugins (see tbcad_plugin.h) and registers their commands. Keep it alive as long as the
// registry: the plugins' code stays loaded until it is destroyed.
class PluginHost {
public:
    PluginHost();
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Loads every shared library in `folder` (not recursive). A plugin that fails to load or
    // initialise is reported in plugins() and skipped; the program carries on.
    void loadFolder(const std::filesystem::path& folder, cmd::Registry& registry);
    void loadFile(const std::filesystem::path& file, cmd::Registry& registry);

    const std::vector<PluginInfo>& plugins() const;
    // Messages plugins logged while loading (the window prints them once it can).
    std::vector<std::string> takeLog();

    // Adds the PLUGINS command, which lists what was loaded and where plugins go.
    void registerListCommand(cmd::Registry& registry, std::filesystem::path pluginFolder) const;

    // Adds the menu entries the plugins asked for.
    void registerUi(ui::UiRegistry& ui) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tbcad::plugin
