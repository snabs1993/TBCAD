#include "common.h"

#include <cstdio>

namespace tbcad::draw2::detail {

std::string promptWith(std::string_view text, const std::vector<std::string>& keywords) {
    std::string s(text);
    if (keywords.empty())
        return s;
    s += " [";
    for (std::size_t i = 0; i < keywords.size(); ++i) {
        if (i)
            s += '/';
        s += keywords[i];
    }
    s += ']';
    return s;
}

void previewEntity(cmd::CommandContext& ctx, Entity e, DisplayList& out) {
    e.common.layer = ctx.doc().header().currentLayer;
    geom::DisplayContext dc;
    const double scale = ctx.host().view().scale;
    if (scale > 0)
        dc.pixelSizeMm = scale;
    try {
        geom::buildDisplay(ctx.doc(), e, dc, out);
    } catch (...) {
    }
}

bool addEntity(cmd::CommandContext& ctx, EntityData data) {
    auto id = ctx.tx().add(Entity{{}, std::move(data)});
    if (!id) {
        ctx.message(id.error().message);
        return false;
    }
    return true;
}

double settingNumber(cmd::CommandContext& ctx, const char* key, double fallback) {
    if (auto s = ctx.host().setting(key)) {
        try {
            return std::stod(*s);
        } catch (...) {
        }
    }
    return fallback;
}

void saveSetting(cmd::CommandContext& ctx, const char* key, double value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", value);
    ctx.host().setSetting(key, buf);
}

double ringArea(const std::vector<Vec2>& ring) {
    double a = 0;
    for (std::size_t i = 0; i < ring.size(); ++i)
        a += cross(ring[i], ring[(i + 1) % ring.size()]);
    return a / 2;
}

}  // namespace tbcad::draw2::detail
