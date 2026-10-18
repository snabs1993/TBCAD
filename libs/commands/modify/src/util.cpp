#include "util.h"

#include <cstdio>
#include <cstdlib>

namespace tbcad::modifycmd {

double getSetting(const cmd::IHost& host, std::string_view key, double fallback) {
    auto v = host.setting(key);
    if (!v)
        return fallback;
    char* end = nullptr;
    double d = std::strtod(v->c_str(), &end);
    return end == v->c_str() ? fallback : d;
}

void putSetting(cmd::IHost& host, std::string_view key, double value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.12g", value);
    host.setSetting(key, buf);
}

bool getFlag(const cmd::IHost& host, std::string_view key, bool fallback) {
    auto v = host.setting(key);
    if (!v)
        return fallback;
    return *v == "1" || *v == "true";
}

void putFlag(cmd::IHost& host, std::string_view key, bool value) {
    host.setSetting(key, value ? "1" : "0");
}

namespace {
bool layerAllows(const Document& doc, const Entity& e, bool needUnlocked) {
    if (!e.common.visible)
        return false;
    // Layer, sub-layer inheritance, "active only" and levels all apply.
    const EffectiveLayer l = doc.effective(e.common);
    if (!l.live())
        return false;
    return !(needUnlocked && l.locked);
}
}  // namespace

bool isEditable(const Document& doc, const Entity& e) {
    return modify::isCurveEntity(e) && layerAllows(doc, e, true);
}

bool isModifiable(const Document& doc, const Entity& e) {
    return layerAllows(doc, e, true);
}

bool isUsableEdge(const Document& doc, const Entity& e) {
    return modify::isCurveEntity(e) && layerAllows(doc, e, false);
}

bool isUsableBoundary(const Document& doc, const Entity& e) {
    return (modify::isCurveEntity(e) || kind(e) == EntityKind::Ray ||
            kind(e) == EntityKind::Insert) &&
           layerAllows(doc, e, false);
}

bool isTrimTarget(const Document& doc, const Entity& e) {
    return (modify::isCurveEntity(e) || kind(e) == EntityKind::Ray) && layerAllows(doc, e, true);
}

Result<void> replaceEntity(Transaction& tx, EntityId id, const Entity& value) {
    return tx.modify(id, [&](Entity& e) {
        EntityId keep = e.common.id;
        e = value;
        e.common.id = keep;
    });
}

Result<std::vector<EntityId>> applyEdit(Transaction& tx, EntityId id, modify::EntityEdit edit) {
    std::vector<EntityId> ids;
    if (edit.eraseSource) {
        if (auto r = tx.erase(id); !r)
            return r.error();
        return ids;
    }
    if (edit.modified) {
        if (auto r = replaceEntity(tx, id, *edit.modified); !r)
            return r.error();
        ids.push_back(id);
    }
    for (Entity& e : edit.added) {
        auto r = tx.add(std::move(e));
        if (!r)
            return r.error();
        ids.push_back(r.value());
    }
    return ids;
}

void previewEntity(const Document& doc, const cmd::IHost& host, const Entity& e, DisplayList& out) {
    geom::DisplayContext dc;
    double px = host.view().scale;
    dc.pixelSizeMm = px > 0 ? px : 0.1;
    geom::buildDisplay(doc, e, dc, out);
}

}  // namespace tbcad::modifycmd
