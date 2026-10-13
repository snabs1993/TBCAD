#include "tbcad/layout/layout.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

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
          << l.content.value << '\n';
        for (const Viewport& v : l.viewports) {
            o << "V\t" << v.id << '\t' << num(v.paper.min.x) << '\t' << num(v.paper.min.y) << '\t'
              << num(v.paper.max.x) << '\t' << num(v.paper.max.y) << '\t' << num(v.center.x) << '\t'
              << num(v.center.y) << '\t' << num(v.scaleN) << '\t' << (v.on ? 1 : 0) << '\t';
            for (std::size_t i = 0; i < v.frozen.size(); ++i)
                o << (i ? "," : "") << v.frozen[i].value;
            o << '\n';
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

Result<Edit> openEdit(Transaction& tx, const std::string& layoutName, int vpId) {
    Edit e;
    e.all = current(tx, tx.document());
    e.layout = pick(e.all, layoutName);
    if (!e.layout)
        return err(ErrorCode::NotFound, "No layout named \"" + layoutName + "\"");
    if (vpId > 0) {
        for (Viewport& v : e.layout->viewports)
            if (v.id == vpId)
                e.vp = &v;
        if (!e.vp)
            return err(ErrorCode::NotFound,
                       "No viewport " + std::to_string(vpId) + " in layout \"" + layoutName + "\"");
    }
    return e;
}

}  // namespace

Result<int> addViewport(Transaction& tx, const std::string& layoutName, const BBox2& rect,
                        Vec2 center, double scaleN) {
    if (rect.empty() || !(rect.width() > 0) || !(rect.height() > 0))
        return err(ErrorCode::InvalidArgument, "A viewport needs a rectangle with an area");
    if (!(scaleN > 0))
        return err(ErrorCode::InvalidArgument, "The viewport scale must be positive");
    auto e = openEdit(tx, layoutName, 0);
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
    ed.vp->scaleN = scaleN;
    return store(tx, ed.all);
}

Result<void> setViewportCenter(Transaction& tx, const std::string& layoutName, int id, Vec2 c) {
    auto e = openEdit(tx, layoutName, id);
    if (!e)
        return e.error();
    Edit ed = std::move(e.value());
    ed.vp->center = c;
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

Result<void> addTitleBlock(Transaction& tx, const std::string& layoutName,
                           const TitleBlockFields& f) {
    auto l = findLayout(tx.document(), layoutName);
    if (!l)
        return err(ErrorCode::NotFound, "No layout named \"" + layoutName + "\"");
    const double w = l->widthMm, m = 10, bw = 120, bh = 40;
    const double x1 = w - m, x0 = x1 - bw, y0 = m, y1 = m + bh;
    auto line = [&](Vec2 a, Vec2 b) {
        Entity e;
        e.data = LineData{a, b};
        return tx.add(std::move(e), l->content);
    };
    auto text = [&](Vec2 p, double h, const std::string& s) {
        TextData t;
        t.position = p;
        t.height = h;
        t.utf8 = s;
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        return tx.add(std::move(e), l->content);
    };
    // Outer frame and four rows.
    const Vec2 c[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; ++i)
        if (auto r = line(c[i], c[(i + 1) % 4]); !r)
            return r.error();
    for (int row = 1; row < 4; ++row)
        if (auto r = line({x0, y0 + row * bh / 4}, {x1, y0 + row * bh / 4}); !r)
            return r.error();
    const std::pair<const char*, const std::string*> rows[4] = {{"Sheet: ", &f.sheet},
                                                                {"Scale: ", &f.scale},
                                                                {"Drawing: ", &f.drawing},
                                                                {"Project: ", &f.project}};
    for (int i = 0; i < 4; ++i)
        if (auto r = text({x0 + 3, y0 + i * bh / 4 + 3}, 3.5,
                          std::string(rows[i].first) + *rows[i].second);
            !r)
            return r.error();
    if (!f.date.empty())
        if (auto r = text({x0 + bw / 2, y0 + 3}, 3.5, "Date: " + f.date); !r)
            return r.error();
    return {};
}

}  // namespace tbcad::layout
