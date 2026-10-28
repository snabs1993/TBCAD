#include "tbcad/platform/updater.h"

namespace tbcad::platform {

bool httpsGet(const std::string&, std::string&, std::string& error) {
    error = "Update check is only available on Windows.";
    return false;
}
bool httpsDownload(const std::string&, const std::filesystem::path&, std::atomic<float>*,
                   const std::atomic<bool>*, std::string& error) {
    error = "Update download is only available on Windows.";
    return false;
}
std::filesystem::path selfExecutable() {
    return {};
}
bool launchDetached(const std::filesystem::path&, const std::vector<std::string>&) {
    return false;
}

}  // namespace tbcad::platform
