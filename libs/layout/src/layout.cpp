#include "tbcad/layout/layout.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "tbcad/blocks/attributes.h"

namespace tbcad::layout {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t i = 0;
    for (;;) {
        std::size_t j = s.find(sep, i);
        if (j == std::string::npos) {
            out.push_back(s.substr(i));
            return out;
        }
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
}

double toD(const std::string& s) {
    return std::strtod(s.c_str(), nullptr);
}

std::string serialize(const std::vector<Layout>& all) {
    std::ostringstream o;
    for (const Layout& l : all) {
        o << "L\t" << l.uid << '\t' << l.name << '\t' << l.paperName << '\t'
          << (l.landscape ? 1 : 0) << '\t' << num(l.widthMm) << '\t' << num(l.heightMm) << '\t'
          << l.content.value << '\t' << l.pageSetup << '\n';
        for (const Viewport& v : l.viewports) {
            o << "V\t" << v.id << '\t' << num(v.paper.min.x) << '\t' << num(v.paper.min.y) << '\t'
              << num(v.paper.max.x) << '\t' << num(v.paper.max.y) << '\t' << num(v.center.x) << '\t'
              << num(v.center.y) << '\t' << num(v.scaleN) << '\t' << (v.on ? 1 : 0) << '\t';
            for (std::size_t i = 0; i < v.frozen.size(); ++i)
                o << (i ? "," : "") << v.frozen[i].value;
            o << '\t' << v.level.value << '\t' << (v.locked ? 1 : 0) << '\t';
            // The outline of a clipped viewport: x:y pairs joined by ';'.
            for (std::size_t i = 0; i < v.clip.size(); ++i)
                o << (i ? ";" : "") << num(v.clip[i].x) << ':' << num(v.clip[i].y);
            o << '\t' << (v.border ? 1 : 0) << '\t' << v.borderLayer.value << '\n';
        }
    }
    return o.str();
}

Result<void> store(Transaction& tx, const std::vector<Layout>& all) {
    std::string text = serialize(all);
    return tx.modifyHeader([&](Header& h) {
        if (text.empty())
            h.custom.erase(kHeaderKey);
        else
            h.custom[kHeaderKey] = text;
    });
}

// The transaction sees its own edits through the document.
std::vector<Layout> current(Transaction&, const Document& doc) {
    return layouts(doc);
}

Layout* pick(std::vector<Layout>& all, std::string_view name) {
    for (Layout& l : all)
        if (lower(l.name) == lower(name))
            return &l;
    return nullptr;
}

Error err(ErrorCode c, std::string m) {
    return Error{c, std::move(m)};
}

Result<void> resolvePaper(const std::string& paperName, bool landscape, double cw, double ch,
                          std::string& outName, double& w, double& h) {
    if (paperName == "Custom") {
        if (!(cw > 0) || !(ch > 0))
            return err(ErrorCode::InvalidArgument, "A custom paper needs a width and a height");
        outName = "Custom";
        w = cw;
        h = ch;
    } else if (const exportlib::PaperSize* p = exportlib::findPaperSize(paperName)) {
        outName = p->name;
        w = p->shortMm;
        h = p->longMm;
    } else {
        return err(ErrorCode::NotFound, "Unknown paper size: " + paperName);
    }
    if (landscape != (w > h))
        std::swap(w, h);
    return {};
}

}  // namespace

std::vector<Vec2> viewportOutline(const Viewport& vp) {
    if (vp.clip.size() >= 3)
        return vp.clip;
    return {vp.paper.min,
            {vp.paper.max.x, vp.paper.min.y},
            vp.paper.max,
            {vp.paper.min.x, vp.paper.max.y}};
}

bool viewportContains(const Viewport& vp, Vec2 p) {
    if (vp.clip.size() < 3)
        return vp.paper.contains(p);
    bool in = false;  // even-odd ray casting
    const auto& c = vp.clip;
    for (std::size_t i = 0, j = c.size() - 1; i < c.size(); j = i++)
        if ((c[i].y > p.y) != (c[j].y > p.y) &&
            p.x < (c[j].x - c[i].x) * (p.y - c[i].y) / (c[j].y - c[i].y) + c[i].x)
            in = !in;
    return in;
}

const Viewport* Layout::findViewport(int vid) const {
    for (const Viewport& v : viewports)
        if (v.id == vid)
            return &v;
    return nullptr;
}

std::vector<Layout> layouts(const Document& doc) {
    std::vector<Layout> out;
    auto it = doc.header().custom.find(kHeaderKey);
    if (it == doc.header().custom.end())
        return out;
    for (const std::string& line : split(it->second, '\n')) {
        auto f = split(line, '\t');
        if (f.size() >= 8 && f[0] == "L") {
            Layout l;
            l.uid = std::atoi(f[1].c_str());
            l.name = f[2];
            l.paperName = f[3];
            l.landscape = f[4] == "1";
            l.widthMm = toD(f[5]);
            l.heightMm = toD(f[6]);
            l.content =
                BlockId{static_cast<std::uint32_t>(std::strtoul(f[7].c_str(), nullptr, 10))};
            if (f.size() > 8)
                l.pageSetup = f[8];
            out.push_back(std::move(l));
        } else if (f.size() >= 10 && f[0] == "V" && !out.empty()) {
            Viewport v;
            v.id = std::atoi(f[1].c_str());
            v.paper.min = {toD(f[2]), toD(f[3])};
            v.paper.max = {toD(f[4]), toD(f[5])};
            v.center = {toD(f[6]), toD(f[7])};
            v.scaleN = toD(f[8]);
            v.on = f[9] == "1";
            if (f.size() > 10 && !f[10].empty())
                for (const std::string& id : split(f[10], ','))
                    v.frozen.push_back(
                        LayerId{static_cast<std::uint32_t>(std::strtoul(id.c_str(), nullptr, 10))});
            if (f.size() > 11)
                v.level =
                    LevelId{static_cast<std::uint32_t>(std::strtoul(f[11].c_str(), nullptr, 10))};
            v.locked = f.size() > 12 && f[12] == "1";
            if (f.size() > 13 && !f[13].empty()) {
                for (const std::string& pt : split(f[13], ';')) {
                    const std::size_t c = pt.find(':');
                    if (c != std::string::npos)
                        v.clip.push_back({toD(pt.substr(0, c)), toD(pt.substr(c + 1))});
                }
                if (v.clip.size() < 3)
                    v.clip.clear();
            }
            v.border = !(f.size() > 14 && f[14] == "0");
            if (f.size() > 15)
                v.borderLayer =
                    LayerId{static_cast<std::uint32_t>(std::strtoul(f[15].c_str(), nullptr, 10))};
            out.back().viewports.push_back(std::move(v));
        }
    }
    return out;
}

std::optional<Layout> findLayout(const Document& doc, std::string_view name) {
    for (Layout& l : layouts(doc))
        if (lower(l.name) == lower(name))
            return std::move(l);
    return std::nullopt;
}

Result<void> validateLayoutName(const Document& doc, std::string_view name,
                                std::string_view ignore) {
    if (name.empty() || name.size() > 64)
        return err(ErrorCode::InvalidArgument, "A layout name has 1 to 64 characters");
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            return err(ErrorCode::InvalidArgument,
                       "A layout name cannot contain control characters");
    if (name.front() == ' ' || name.back() == ' ')
        return err(ErrorCode::InvalidArgument, "A layout name cannot start or end with a space");
    for (const Layout& l : layouts(doc))
        if (lower(l.name) == lower(name) && lower(l.name) != lower(ignore))
            return err(ErrorCode::Conflict, "A layout named \"" + l.name + "\" already exists");
    return {};
}

Result<void> createLayout(Transaction& tx, const std::string& name, const std::string& paperName,
                          bool landscape) {
    const Document& doc = tx.document();
    if (auto ok = validateLayoutName(doc, name); !ok)
        return ok;
    Layout l;
    l.name = name;
    if (auto ok = resolvePaper(paperName, landscape, 0, 0, l.paperName, l.widthMm, l.heightMm); !ok)
        return ok;
    l.landscape = landscape;
    std::vector<Layout> all = current(tx, doc);
    for (const Layout& o : all)
        l.uid = std::max(l.uid, o.uid);
    ++l.uid;
    BlockDef def;
    def.name = "*LAYOUT-" + std::to_string(l.uid);
    def.anonymous = true;
    auto block = tx.addBlock(def);
    if (!block)
        return block.error();
    l.content = block.value();
    all.push_back(std::move(l));
    return store(tx, all);
}

Result<void> deleteLayout(Transaction& tx, const std::string& name) {
    std::vector<Layout> all = current(tx, tx.document());
    Layout* l = pick(all, name);
    if (!l)
        return err(ErrorCode::NotFound, "No layout named \"" + name + "\"");
    if (auto r = tx.eraseBlock(l->content); !r)
        return r;
    all.erase(all.begin() + (l - all.data()));
    return store(tx, all);
}

Result<void> copyLayout(Transaction& tx, const std::string& name, const std::string& newName) {
    const Document& doc = tx.document();
    if (auto ok = validateLayoutName(doc, newName); !ok)
        return ok;
    std::vector<Layout> all = current(tx, doc);
    const Layout* src = pick(all, name);
    if (!src)
        return err(ErrorCode::NotFound, "No layout named \"" + name + "\"");
    Layout l = *src;
    l.name = newName;
    l.uid = 0;
    for (const Layout& o : all)
        l.uid = std::max(l.uid, o.uid);
    ++l.uid;
    BlockDef def;
    def.name = "*LAYOUT-" + std::to_string(l.uid);
    def.anonymous = true;
    auto block = tx.addBlock(def);
    if (!block)
        return block.error();
    std::vector<EntityId> sheetObjects;
    if (const BlockDef* from = doc.block(src->content))
        sheetObjects = from->entities;
    for (EntityId id : sheetObjects) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        Entity copy = *e;
        copy.common.id = {};
        if (auto r = tx.add(std::move(copy), block.value()); !r)
            return r.error();
    }
    l.content = block.value();
    all.push_back(std::move(l));
    return store(tx, all);
}

Result<void> renameLayout(Transaction& tx, const std::string& name, const std::string& newName) {
    if (auto ok = validateLayoutName(tx.document(), newName, name); !ok)
        return ok;
    std::vector<Layout> all = current(tx, tx.document());
    Layout* l = pick(all, name);
    if (!l)
        return err(ErrorCode::NotFound, "No layout named \"" + name + "\"");
    l->name = newName;
    return store(tx, all);
}

Result<void> moveLayout(Transaction& tx, const std::string& name, std::size_t position) {
    std::vector<Layout> all = current(tx, tx.document());
    std::size_t from = all.size();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (lower(all[i].name) == lower(name))
            from = i;
    if (from == all.size())
        return err(ErrorCode::NotFound, "No layout named \"" + name + "\"");
    const std::size_t to = std::min(position, all.size() - 1);
    if (to == from)
        return {};
    Layout moved = std::move(all[from]);
    all.erase(all.begin() + static_cast<std::ptrdiff_t>(from));
    all.insert(all.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    return store(tx, all);
}

Result<void> setLayoutPageSetupName(Transaction& tx, const std::string& name,
                                    const std::string& pageSetup) {
    std::vector<Layout> all = current(tx, tx.document());
    Layout* l = pick(all, name);
    if (!l)
        return err(ErrorCode::NotFound, "No layout named \"" + name + "\"");
    l->pageSetup = pageSetup;
    return store(tx, all);
}

Result<void> setLayoutPaper(Transaction& tx, const std::string& name, const std::string& paperName,
                            bool landscape, double cw, double ch) {
    std::vector<Layout> all = current(tx, tx.document());
    Layout* l = pick(all, name);
    if (!l)
        return err(ErrorCode::NotFound, "No layout named \"" + name + "\"");
    std::string pn;
    double w = 0, h = 0;
    if (auto ok = resolvePaper(paperName, landscape, cw, ch, pn, w, h); !ok)
        return ok;
    l->paperName = pn;
    l->landscape = landscape;
    l->widthMm = w;
    l->heightMm = h;
    return store(tx, all);
}

namespace {

struct Edit {
    std::vector<Layout> all;
    Layout* layout = nullptr;
    Viewport* vp = nullptr;
};

// wantVp: also find viewport `vpId` (an id below 1 never exists); false opens the layout only.
Result<Edit> openEdit(Transaction& tx, const std::string& layoutName, int vpId,
                      bool wantVp = true) {
    Edit e;
    e.all = current(tx, tx.document());
    e.layout = pick(e.all, layoutName);
    if (!e.layout)
        return err(ErrorCode::NotFound, "No layout named \"" + layoutName + "\"");
    if (wantVp) {
        for (Viewport& v : e.layout->viewports)
            if (vpId > 0 && v.id == vpId)
                e.vp = &v;
        if (!e.vp)
            return err(ErrorCode::NotFound,
                       "No viewport " + std::to_string(vpId) + " in layout \"" + layoutName + "\"");
    }
    return e;
}

}  // namespace

std::vector<LayerId> layersFrozenInNewViewports(const Document& doc) {
    std::vector<LayerId> out;
    const auto it = doc.header().custom.find(kNewViewportFrozenKey);
    if (it == doc.header().custom.end())
        return out;
    for (const std::string& part : split(it->second, ',')) {
        const LayerId id{static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 10))};
        if (id.value != 0 && doc.layer(id))
            out.push_back(id);
    }
    return out;
}

Result<void> setLayerFrozenInNewViewports(const Document& doc, Transaction& tx, LayerId layer,
                                          bool frozen) {
    if (!doc.layer(layer))
        return err(ErrorCode::NotFound, "No such layer");
    return tx.modifyHeader([&](Header& h) {
        std::vector<std::uint32_t> now;
        if (auto it = h.custom.find(kNewViewportFrozenKey); it != h.custom.end())
            for (const std::string& part : split(it->second, ','))
                now.push_back(static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 10)));
        const auto at = std::find(now.begin(), now.end(), layer.value);
        if (frozen && at == now.end())
            now.push_back(layer.value);
        else if (!frozen && at != now.end())
            now.erase(at);
        std::string text;
        for (std::uint32_t id : now)
            text += (text.empty() ? "" : ",") + std::to_string(id);
        if (text.empty())
            h.custom.erase(kNewViewportFrozenKey);
        else
            h.custom[kNewViewportFrozenKey] = text;
    });
}

Result<int> addViewport(Transaction& tx, const std::string& layoutName, const BBox2& rect,
                        Vec2 center, double scaleN) {
    if (rect.empty() || !(rect.width() > 0) || !(rect.height() > 0))
        return err(ErrorCode::InvalidArgument, "A viewport needs a rectangle with an area");
    if (!(scaleN > 0))
        return err(ErrorCode::InvalidArgument, "The viewport scale must be positive");
    auto e = openEdit(tx, layoutName, 0, false);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    Viewport v;
    for (const Viewport& o : ed.layout->viewports)
        v.id = std::max(v.id, o.id);
    ++v.id;
    v.paper = rect;
    v.center = center;
    v.scaleN = scaleN;
    v.frozen = layersFrozenInNewViewports(tx.document());
    int id = v.id;
    ed.layout->viewports.push_back(std::move(v));
    if (auto r = store(tx, ed.all); !r)
        return r.error();
    return id;
}

Result<void> removeViewport(Transaction& tx, const std::string& layoutName, int id) {
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    auto& vps = ed.layout->viewports;
    vps.erase(vps.begin() + (ed.vp - vps.data()));
    return store(tx, ed.all);
}

Result<void> setViewportScale(Transaction& tx, const std::string& layoutName, int id,
                              double scaleN) {
    if (!(scaleN > 0))
        return err(ErrorCode::InvalidArgument, "The viewport scale must be positive");
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    if (ed.vp->locked)
        return err(ErrorCode::Conflict, "Viewport is locked. Use VPLOCK Off first.");
    ed.vp->scaleN = scaleN;
    return store(tx, ed.all);
}

Result<void> setViewportCenter(Transaction& tx, const std::string& layoutName, int id, Vec2 c) {
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    if (ed.vp->locked)
        return err(ErrorCode::Conflict, "Viewport is locked. Use VPLOCK Off first.");
    ed.vp->center = c;
    return store(tx, ed.all);
}

Result<void> setViewportBorderLayer(Transaction& tx, const std::string& layoutName, int id,
                                    LayerId layer) {
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    ed.vp->borderLayer = layer;
    return store(tx, ed.all);
}

Result<void> setViewportLocked(Transaction& tx, const std::string& layoutName, int id,
                               bool locked) {
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    ed.vp->locked = locked;
    return store(tx, ed.all);
}

Result<void> setViewportRect(Transaction& tx, const std::string& layoutName, int id,
                             const BBox2& rect) {
    if (rect.empty() || !(rect.width() > 0) || !(rect.height() > 0))
        return makeError(ErrorCode::InvalidArgument, "The viewport needs a size.");
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    // A clipped viewport keeps its outline: it moves and stretches with the rectangle.
    if (!ed.vp->clip.empty()) {
        const BBox2 old = ed.vp->paper;
        const double sx = old.width() > 0 ? rect.width() / old.width() : 1;
        const double sy = old.height() > 0 ? rect.height() / old.height() : 1;
        for (Vec2& p : ed.vp->clip)
            p = {rect.min.x + (p.x - old.min.x) * sx, rect.min.y + (p.y - old.min.y) * sy};
    }
    ed.vp->paper = rect;
    return store(tx, ed.all);
}

Result<void> setViewportClip(Transaction& tx, const std::string& layoutName, int id,
                             const std::vector<Vec2>& outline) {
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    if (ed.vp->locked)
        return err(ErrorCode::Conflict, "Viewport is locked. Use VPLOCK Off first.");
    std::vector<Vec2> ring = outline;
    if (ring.size() > 1 && ring.front() == ring.back())
        ring.pop_back();
    if (ring.size() < 3) {
        ed.vp->clip.clear();
        return store(tx, ed.all);
    }
    BBox2 box;
    for (const Vec2& p : ring)
        box.expand(p);
    if (!(box.width() > 0) || !(box.height() > 0))
        return err(ErrorCode::InvalidArgument, "The outline has no area");
    ed.vp->paper = box;
    ed.vp->clip = std::move(ring);
    return store(tx, ed.all);
}

Result<void> setViewportOn(Transaction& tx, const std::string& layoutName, int id, bool on) {
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    ed.vp->on = on;
    return store(tx, ed.all);
}

Result<void> setViewportLayerFrozen(Transaction& tx, const std::string& layoutName, int id,
                                    LayerId layer, bool frozen) {
    if (!tx.document().layer(layer))
        return err(ErrorCode::NotFound, "No such layer");
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    auto& list = ed.vp->frozen;
    auto it = std::find(list.begin(), list.end(), layer);
    if (frozen && it == list.end())
        list.push_back(layer);
    else if (!frozen && it != list.end())
        list.erase(it);
    return store(tx, ed.all);
}

Result<void> setViewportLevel(Transaction& tx, const std::string& layoutName, int id,
                              LevelId level) {
    if (level.value != 0 && !tx.document().level(level))
        return err(ErrorCode::NotFound, "No such level");
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    ed.vp->level = level;
    return store(tx, ed.all);
}

bool frozenInViewport(const Document& doc, const Viewport& vp, LayerId layer) {
    for (int depth = 0; layer.value != 0 && depth < 64; ++depth) {
        if (std::find(vp.frozen.begin(), vp.frozen.end(), layer) != vp.frozen.end())
            return true;
        const Layer* l = doc.layer(layer);
        if (!l)
            return false;
        layer = l->parent;
    }
    return false;
}

namespace {

constexpr const char* kTitleBlockName = "TITLEBLOCK";
constexpr double kBlockW = 120, kBlockH = 40, kRowH = 10, kTextH = 3.5;

// The title block definition: a frame with four rows (project, drawing, scale, and sheet with
// date) whose values are attributes, so they can be edited with ATTEDIT and read back by tag.
Result<BlockId> titleBlockDefinition(Transaction& tx) {
    if (const BlockDef* have = tx.document().blockByName(kTitleBlockName))
        return have->id;
    auto block = tx.addBlock(BlockDef{.name = kTitleBlockName});
    if (!block)
        return block.error();
    const BlockId id = block.value();
    auto add = [&](EntityData d) -> Result<void> {
        Entity e;
        e.data = std::move(d);
        if (auto r = tx.add(std::move(e), id); !r)
            return r.error();
        return {};
    };
    const Vec2 c[4] = {{0, 0}, {kBlockW, 0}, {kBlockW, kBlockH}, {0, kBlockH}};
    for (int i = 0; i < 4; ++i)
        if (auto r = add(LineData{c[i], c[(i + 1) % 4]}); !r)
            return r.error();
    for (int row = 1; row < 4; ++row)
        if (auto r = add(LineData{{0, row * kRowH}, {kBlockW, row * kRowH}}); !r)
            return r.error();
    if (auto r = add(LineData{{kBlockW / 2, 0}, {kBlockW / 2, kRowH}}); !r)
        return r.error();
    auto text = [&](Vec2 p, const std::string& label, const char* tag = nullptr) -> Result<void> {
        TextData t;
        t.position = p;
        t.alignPoint = p;
        t.height = kTextH;
        t.utf8 = label;
        if (tag) {
            AttDefInfo def;
            def.tag = tag;
            def.prompt = label;
            t.attdef = std::move(def);
        }
        return add(Indirect<TextData>(std::move(t)));
    };
    struct Cell {
        double x, row;
        const char* label;
        const char* tag;
    };
    // Row 0 is the bottom one.
    const Cell cells[] = {{0, 0, "Sheet:", "SHEET"},
                          {kBlockW / 2, 0, "Date:", "DATE"},
                          {0, 1, "Scale:", "SCALE"},
                          {0, 2, "Drawing:", "DRAWING"},
                          {0, 3, "Project:", "PROJECT"}};
    for (const Cell& cell : cells) {
        const double y = cell.row * kRowH + 3;
        if (auto r = text({cell.x + 3, y}, cell.label); !r)
            return r.error();
        if (auto r = text({cell.x + 24, y}, cell.tag, cell.tag); !r)
            return r.error();
    }
    return id;
}

}  // namespace

Result<void> addTitleBlock(Transaction& tx, const std::string& layoutName,
                           const TitleBlockFields& f) {
    auto l = findLayout(tx.document(), layoutName);
    if (!l)
        return err(ErrorCode::NotFound, "No layout named \"" + layoutName + "\"");
    auto def = titleBlockDefinition(tx);
    if (!def)
        return def.error();

    InsertData ins;
    ins.block = def.value();
    ins.position = {l->widthMm - 10 - kBlockW, 10};
    ins.attribs = blocks::defaultAttribs(tx.document(), def.value());
    const std::pair<const char*, const std::string*> values[] = {{"PROJECT", &f.project},
                                                                 {"DRAWING", &f.drawing},
                                                                 {"SCALE", &f.scale},
                                                                 {"DATE", &f.date},
                                                                 {"SHEET", &f.sheet}};
    for (auto& a : ins.attribs)
        for (const auto& [tag, value] : values)
            if (a.tag == tag)
                a.value = *value;
    Entity e;
    e.data = Indirect<InsertData>(std::move(ins));
    if (auto r = tx.add(std::move(e), l->content); !r)
        return r.error();
    return {};
}

}  // namespace tbcad::layout
