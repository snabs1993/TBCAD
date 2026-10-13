#include "tbcad/mcpapp/app_session.h"

#include <algorithm>

namespace tbcad::mcpapp {

AppSession::AppSession(ui::AppCore& core, mcp::OwnerQueue& queue, Hooks hooks)
    : core_(core), queue_(queue), hooks_(std::move(hooks)) {}

mcp::FileInfo AppSession::file() {
    mcp::FileInfo f;
    f.name = core_.displayName();
    f.hasFile = core_.hasFile();
    f.path = core_.file();
    f.modified = core_.modified();
    return f;
}

Result<void> AppSession::save(std::optional<std::filesystem::path> as) {
    if (as)
        return core_.saveAs(*as);
    return core_.save();
}

std::vector<std::string> AppSession::messagesSince(std::size_t mark) {
    const auto& h = core_.history();
    if (mark >= h.size())
        return {};
    return std::vector<std::string>(h.begin() + static_cast<std::ptrdiff_t>(mark), h.end());
}

std::optional<mcp::ViewInfo> AppSession::view() {
    const render::Camera2D& cam = core_.camera();
    mcp::ViewInfo v;
    v.center = cam.center;
    v.mmPerPixel = cam.pixelSizeMm();
    v.widthPx = cam.viewportW;
    v.heightPx = cam.viewportH;
    return v;
}

void AppSession::setView(const mcp::ViewInfo& v) {
    render::Camera2D cam = core_.camera();
    cam.center = v.center;
    if (v.mmPerPixel > 0)
        cam.pixelsPerMm = 1.0 / v.mmPerPixel;
    core_.setCamera(cam);
}

std::optional<mcp::Image> AppSession::captureView(int maxPx, const BBox2& area) {
    if (!hooks_.capture)
        return std::nullopt;
    return hooks_.capture(maxPx, area);
}

mcp::EditDecision AppSession::requestEdit(const mcp::ClientInfo& client) {
    return hooks_.requestEdit ? hooks_.requestEdit(client) : mcp::EditDecision::Deny;
}

void AppSession::logToolCall(const mcp::ToolCallLog& log) {
    if (hooks_.log)
        hooks_.log(log);
}

}  // namespace tbcad::mcpapp
