#include "tbcad/levels/level_ops.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace tbcad::levels {

namespace {

Error err(ErrorCode code, std::string msg) {
    return makeError(code, std::move(msg));
}

std::string quoted(const std::string& s) {
    return "\"" + s + "\"";
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

Result<void> edit(Transaction& tx, LevelId id, const std::function<void(Level&)>& fn) {
    auto r = tx.modifyLevel(id, fn);
    if (!r)
        return err(r.error().code, r.error().message);
    return {};
}

}  // namespace

Result<LevelId> resolveLevel(const Document& doc, std::string_view ref) {
    ref = trimView(ref);
    if (ref.empty())
        return err(ErrorCode::InvalidArgument, "Level name is empty.");
    if (ref.front() == '#') {
        std::uint32_t v = 0;
        auto r = std::from_chars(ref.data() + 1, ref.data() + ref.size(), v);
        if (r.ec == std::errc() && r.ptr == ref.data() + ref.size() && doc.level(LevelId{v}))
            return LevelId{v};
        return err(ErrorCode::NotFound, "No level " + std::string(ref) + ".");
    }
    if (const Level* l = doc.levelByName(ref))
        return l->id;
    return err(ErrorCode::NotFound, "No level named " + quoted(std::string(ref)) + ".");
}

std::vector<LevelId> levelOrder(const Document& doc) {
    std::vector<LevelId> out;
    for (const auto& [id, l] : doc.levels())
        out.push_back(id);
    std::stable_sort(out.begin(), out.end(), [&](LevelId a, LevelId b) {
        return doc.level(a)->elevation < doc.level(b)->elevation;
    });
    return out;
}

std::size_t entityCount(const Document& doc, LevelId id) {
    std::size_t n = 0;
    doc.forEachEntity([&](const Entity& e) { n += e.common.level == id ? 1 : 0; });
    return n;
}

Result<LevelId> createLevel(const Document&, Transaction& tx, std::string name, double elevation) {
    Level row;
    row.name = std::move(name);
    row.elevation = elevation;
    auto r = tx.addLevel(std::move(row));
    if (!r)
        return err(r.error().code, r.error().message);
    return r.value();
}

Result<void> renameLevel(const Document&, Transaction& tx, LevelId id, std::string newName) {
    return edit(tx, id, [&](Level& l) { l.name = std::move(newName); });
}

Result<void> setElevation(const Document&, Transaction& tx, LevelId id, double elevation) {
    return edit(tx, id, [&](Level& l) { l.elevation = elevation; });
}

Result<void> setVisible(const Document& doc, Transaction& tx, LevelId id, bool visible) {
    if (!visible && doc.header().currentLevel == id)
        return err(ErrorCode::Conflict, "The current level cannot be hidden.");
    return edit(tx, id, [&](Level& l) { l.visible = visible; });
}

Result<void> setLocked(const Document&, Transaction& tx, LevelId id, bool locked) {
    return edit(tx, id, [&](Level& l) { l.locked = locked; });
}

Result<void> setCurrentLevel(const Document& doc, Transaction& tx, LevelId id) {
    if (!doc.level(id))
        return err(ErrorCode::NotFound, "No such level.");
    if (doc.header().currentLevel == id)
        return {};
    if (!doc.level(id)->visible) {
        if (auto r = edit(tx, id, [](Level& l) { l.visible = true; }); !r)
            return r;
    }
    auto r = tx.modifyHeader([&](Header& h) { h.currentLevel = id; });
    if (!r)
        return err(r.error().code, r.error().message);
    return {};
}

Result<void> setLevelView(const Document& doc, Transaction& tx, LevelView view) {
    if (doc.header().levelView == view)
        return {};
    auto r = tx.modifyHeader([&](Header& h) { h.levelView = view; });
    if (!r)
        return err(r.error().code, r.error().message);
    return {};
}

Result<std::size_t> moveToLevel(const Document& doc, Transaction& tx,
                                const std::vector<EntityId>& ids, LevelId level) {
    if (!doc.level(level))
        return err(ErrorCode::NotFound, "No such level.");
    std::size_t n = 0;
    for (EntityId id : ids) {
        const Entity* e = doc.find(id);
        if (!e || doc.ownerBlock(id) || e->common.level == level)
            continue;
        auto r = tx.modify(id, [&](Entity& x) { x.common.level = level; });
        if (!r)
            return err(r.error().code, r.error().message);
        ++n;
    }
    return n;
}

Result<void> deleteLevel(const Document& doc, Transaction& tx, LevelId id, LevelId moveTo) {
    const Level* row = doc.level(id);
    if (!row)
        return err(ErrorCode::NotFound, "No such level.");
    if (id == kLevelDefault)
        return err(ErrorCode::Conflict, "The first level cannot be deleted.");
    if (id == moveTo || !doc.level(moveTo))
        return err(ErrorCode::InvalidArgument, "Choose another level to move the objects to.");
    std::vector<EntityId> ids;
    doc.forEachEntity([&](const Entity& e) {
        if (e.common.level == id)
            ids.push_back(e.common.id);
    });
    for (EntityId e : ids) {
        auto r = tx.modify(e, [&](Entity& x) { x.common.level = moveTo; });
        if (!r)
            return err(r.error().code, r.error().message);
    }
    if (doc.header().currentLevel == id) {
        auto r = tx.modifyHeader([&](Header& h) { h.currentLevel = moveTo; });
        if (!r)
            return err(r.error().code, r.error().message);
    }
    auto r = tx.eraseLevel(id);
    if (!r)
        return err(r.error().code, r.error().message);
    return {};
}

std::string describe(const Document& doc, LevelId id) {
    const Level* l = doc.level(id);
    if (!l)
        return "?";
    char buf[64];
    std::snprintf(buf, sizeof buf, " (elevation %g mm)", l->elevation);
    return quoted(l->name) + buf;
}

Result<LevelView> parseLevelView(std::string_view text) {
    std::string t;
    for (char c : trimView(text))
        t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (t == "dim" || t == "d" || t == "dimothers")
        return LevelView::DimOthers;
    if (t == "hide" || t == "h" || t == "hideothers")
        return LevelView::HideOthers;
    if (t == "all" || t == "a" || t == "showall")
        return LevelView::ShowAll;
    return err(ErrorCode::InvalidArgument, "Level view is Dim, Hide or All.");
}

const char* levelViewName(LevelView view) {
    switch (view) {
        case LevelView::DimOthers:
            return "Dim";
        case LevelView::HideOthers:
            return "Hide";
        case LevelView::ShowAll:
            return "All";
    }
    return "Dim";
}

}  // namespace tbcad::levels
