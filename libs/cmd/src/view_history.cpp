#include <algorithm>
#include <cstdio>

#include "tbcad/cmd/host.h"

namespace tbcad::cmd {

namespace {
constexpr const char* kViewHistoryKey = "tmp.view.history";
}  // namespace

void pushViewHistory(IHost& host, const ViewState& v) {
    std::string h = host.setting(kViewHistoryKey).value_or("");
    char line[128];
    std::snprintf(line, sizeof line, "%.12g %.12g %.12g\n", v.center.x, v.center.y, v.scale);
    // The same view twice in a row is one step back, not two.
    if (!h.empty()) {
        const std::size_t prev = h.size() < 2 ? std::string::npos : h.rfind('\n', h.size() - 2);
        if (h.compare(prev == std::string::npos ? 0 : prev + 1, std::string::npos, line) == 0)
            return;
    }
    h += line;
    // Keep the newest entries only.
    std::size_t lines = static_cast<std::size_t>(std::count(h.begin(), h.end(), '\n'));
    while (lines-- > kViewHistoryMax)
        h.erase(0, h.find('\n') + 1);
    host.setSetting(kViewHistoryKey, h);
}

std::optional<ViewState> popViewHistory(IHost& host, const ViewState& like) {
    std::string h = host.setting(kViewHistoryKey).value_or("");
    if (h.empty())
        return std::nullopt;
    const std::size_t end = h.size() - 1;  // the final '\n'
    const std::size_t start = end == 0 ? std::string::npos : h.rfind('\n', end - 1);
    const std::string last = h.substr(start == std::string::npos ? 0 : start + 1);
    h.erase(start == std::string::npos ? 0 : start + 1);
    host.setSetting(kViewHistoryKey, h);
    ViewState v = like;
    if (std::sscanf(last.c_str(), "%lf %lf %lf", &v.center.x, &v.center.y, &v.scale) != 3)
        return std::nullopt;
    return v;
}

std::size_t viewHistorySize(const IHost& host) {
    const std::string h = host.setting(kViewHistoryKey).value_or("");
    return static_cast<std::size_t>(std::count(h.begin(), h.end(), '\n'));
}

}  // namespace tbcad::cmd
