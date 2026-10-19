#include "tbcad/layout/pagesetup.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>

#include "tbcad/layout/layout.h"

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
        const std::size_t j = s.find(sep, i);
        if (j == std::string::npos) {
            out.push_back(s.substr(i));
            return out;
        }
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
}

Error err(ErrorCode c, std::string m) {
    return Error{c, std::move(m)};
}

struct Store {
    std::vector<PageSetup> setups;
    std::string model;  // setup assigned to model space
};

std::string encodeSetup(const PageSetup& p) {
    const exportlib::PlotSettings& s = p.settings;
    std::ostringstream o;
    o << "S\t" << p.name;
    auto kv = [&](const char* k, const std::string& v) { o << '\t' << k << '=' << v; };
    kv("paper", s.paperName);
    kv("cw", num(s.customWidthMm));
    kv("ch", num(s.customHeightMm));
    kv("landscape", s.landscape ? "1" : "0");
    kv("area", s.area == exportlib::PlotArea::Extents   ? "extents"
               : s.area == exportlib::PlotArea::Display ? "display"
                                                        : "window");
    kv("window", num(s.window.min.x) + "," + num(s.window.min.y) + "," + num(s.window.max.x) + "," +
                     num(s.window.max.y));
    kv("fit", s.fitToPaper ? "1" : "0");
    kv("scale", num(s.scaleN));
    kv("center", s.centerOnPaper ? "1" : "0");
    kv("offset", num(s.offsetMm.x) + "," + num(s.offsetMm.y));
    kv("margin", num(s.marginMm));
    kv("lineweights", s.lineweights ? "1" : "0");
    if (s.lineweightScale != 1)
        kv("lwscale", num(s.lineweightScale));
    if (s.screening != 100)
        kv("screening", std::to_string(s.screening));
    kv("color", s.color == exportlib::ColorMode::Monochrome  ? "mono"
                : s.color == exportlib::ColorMode::Greyscale ? "grey"
                                                             : "color");
    kv("hairline", num(s.hairlineMm));
    kv("chord", num(s.chordTolMm));
    kv("style", p.styleName);
    return o.str();
}

PageSetup decodeSetup(const std::vector<std::string>& f) {
    PageSetup p;
    p.name = f[1];
    exportlib::PlotSettings& s = p.settings;
    for (std::size_t i = 2; i < f.size(); ++i) {
        const auto eq = f[i].find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        auto d = [&] { return std::strtod(v.c_str(), nullptr); };
        auto pair = [&](double& a, double& b) {
            const auto c = split(v, ',');
            if (c.size() == 2) {
                a = std::strtod(c[0].c_str(), nullptr);
                b = std::strtod(c[1].c_str(), nullptr);
            }
        };
        if (k == "paper")
            s.paperName = v;
        else if (k == "cw")
            s.customWidthMm = d();
        else if (k == "ch")
            s.customHeightMm = d();
        else if (k == "landscape")
            s.landscape = v == "1";
        else if (k == "area")
            s.area = v == "extents"   ? exportlib::PlotArea::Extents
                     : v == "display" ? exportlib::PlotArea::Display
                                      : exportlib::PlotArea::Window;
        else if (k == "window") {
            const auto c = split(v, ',');
            if (c.size() == 4) {
                s.window = {};
                s.window.expand(
                    {std::strtod(c[0].c_str(), nullptr), std::strtod(c[1].c_str(), nullptr)});
                s.window.expand(
                    {std::strtod(c[2].c_str(), nullptr), std::strtod(c[3].c_str(), nullptr)});
            }
        } else if (k == "fit")
            s.fitToPaper = v == "1";
        else if (k == "scale")
            s.scaleN = d();
        else if (k == "center")
            s.centerOnPaper = v == "1";
        else if (k == "offset")
            pair(s.offsetMm.x, s.offsetMm.y);
        else if (k == "margin")
            s.marginMm = d();
        else if (k == "lineweights")
            s.lineweights = v == "1";
        else if (k == "lwscale")
            s.lineweightScale = d() > 0 ? d() : 1;
        else if (k == "screening")
            s.screening = std::clamp(static_cast<int>(d()), 0, 100);
        else if (k == "color")
            s.color = v == "mono"   ? exportlib::ColorMode::Monochrome
                      : v == "grey" ? exportlib::ColorMode::Greyscale
                                    : exportlib::ColorMode::AsDisplayed;
        else if (k == "hairline")
            s.hairlineMm = d();
        else if (k == "chord")
            s.chordTolMm = d();
        else if (k == "style")
            p.styleName = v;
    }
    return p;
}

Store load(const Document& doc) {
    Store st;
    auto it = doc.header().custom.find(kPageSetupKey);
    if (it == doc.header().custom.end())
        return st;
    for (const std::string& line : split(it->second, '\n')) {
        const auto f = split(line, '\t');
        if (f.size() >= 2 && f[0] == "S" && !f[1].empty())
            st.setups.push_back(decodeSetup(f));
        else if (f.size() >= 2 && f[0] == "M")
            st.model = f[1];
    }
    return st;
}

Result<void> save(Transaction& tx, const Store& st) {
    std::ostringstream o;
    for (const PageSetup& p : st.setups)
        o << encodeSetup(p) << '\n';
    if (!st.model.empty())
        o << "M\t" << st.model << '\n';
    const std::string text = o.str();
    return tx.modifyHeader([&](Header& h) {
        if (text.empty())
            h.custom.erase(kPageSetupKey);
        else
            h.custom[kPageSetupKey] = text;
    });
}

PageSetup* pick(Store& st, std::string_view name) {
    for (PageSetup& p : st.setups)
        if (lower(p.name) == lower(name))
            return &p;
    return nullptr;
}

// Rewrites the layout table with the page setup names changed by `fn`.
Result<void> updateLayouts(Transaction& tx,
                           const std::function<std::string(const std::string&)>& fn) {
    // The layout table is private to layout.cpp; go through its public API: only layouts whose
    // assignment changes are touched.
    for (const Layout& l : layouts(tx.document())) {
        const std::string now = fn(l.pageSetup);
        if (now != l.pageSetup)
            if (auto r = setLayoutPageSetupName(tx, l.name, now); !r)
                return r;
    }
    return {};
}

}  // namespace

std::vector<PageSetup> pageSetups(const Document& doc) {
    return load(doc).setups;
}

std::optional<PageSetup> findPageSetup(const Document& doc, std::string_view name) {
    for (PageSetup& p : load(doc).setups)
        if (lower(p.name) == lower(name))
            return std::move(p);
    return std::nullopt;
}

Result<void> validatePageSetupName(const Document& doc, std::string_view name,
                                   std::string_view ignore) {
    if (name.empty() || name.size() > 64)
        return err(ErrorCode::InvalidArgument, "A page setup name has 1 to 64 characters");
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            return err(ErrorCode::InvalidArgument,
                       "A page setup name cannot contain control characters");
    if (name.front() == ' ' || name.back() == ' ')
        return err(ErrorCode::InvalidArgument,
                   "A page setup name cannot start or end with a space");
    for (const PageSetup& p : load(doc).setups)
        if (lower(p.name) == lower(name) && lower(p.name) != lower(ignore))
            return err(ErrorCode::Conflict, "A page setup named \"" + p.name + "\" already exists");
    return {};
}

Result<void> savePageSetup(Transaction& tx, const PageSetup& setup) {
    Store st = load(tx.document());
    if (PageSetup* old = pick(st, setup.name)) {
        PageSetup p = setup;
        p.name = old->name;  // keep the spelling the layouts refer to
        *old = std::move(p);
    } else {
        if (auto ok = validatePageSetupName(tx.document(), setup.name); !ok)
            return ok;
        PageSetup p = setup;
        p.settings.styleTable.reset();
        st.setups.push_back(std::move(p));
    }
    for (PageSetup& p : st.setups)
        p.settings.styleTable.reset();
    return save(tx, st);
}

Result<void> deletePageSetup(Transaction& tx, const std::string& name) {
    Store st = load(tx.document());
    PageSetup* p = pick(st, name);
    if (!p)
        return err(ErrorCode::NotFound, "No page setup named \"" + name + "\"");
    const std::string real = p->name;
    st.setups.erase(st.setups.begin() + (p - st.setups.data()));
    if (lower(st.model) == lower(real))
        st.model.clear();
    if (auto r = save(tx, st); !r)
        return r;
    return updateLayouts(
        tx, [&](const std::string& n) { return lower(n) == lower(real) ? std::string() : n; });
}

Result<void> renamePageSetup(Transaction& tx, const std::string& name, const std::string& newName) {
    if (auto ok = validatePageSetupName(tx.document(), newName, name); !ok)
        return ok;
    Store st = load(tx.document());
    PageSetup* p = pick(st, name);
    if (!p)
        return err(ErrorCode::NotFound, "No page setup named \"" + name + "\"");
    const std::string real = p->name;
    p->name = newName;
    if (lower(st.model) == lower(real))
        st.model = newName;
    if (auto r = save(tx, st); !r)
        return r;
    return updateLayouts(
        tx, [&](const std::string& n) { return lower(n) == lower(real) ? newName : n; });
}

Result<void> assignPageSetup(Transaction& tx, const std::string& layoutName,
                             const std::string& setup) {
    auto found = findLayout(tx.document(), layoutName);
    if (!found)
        return err(ErrorCode::NotFound, "No layout named \"" + layoutName + "\"");
    if (setup.empty())
        return setLayoutPageSetupName(tx, found->name, "");
    auto p = findPageSetup(tx.document(), setup);
    if (!p)
        return err(ErrorCode::NotFound, "No page setup named \"" + setup + "\"");
    if (auto r = setLayoutPaper(tx, found->name, p->settings.paperName, p->settings.landscape,
                                p->settings.customWidthMm, p->settings.customHeightMm);
        !r)
        return r;
    return setLayoutPageSetupName(tx, found->name, p->name);
}

Result<void> assignModelPageSetup(Transaction& tx, const std::string& setup) {
    Store st = load(tx.document());
    if (setup.empty()) {
        st.model.clear();
    } else {
        PageSetup* p = pick(st, setup);
        if (!p)
            return err(ErrorCode::NotFound, "No page setup named \"" + setup + "\"");
        st.model = p->name;
    }
    return save(tx, st);
}

std::string modelPageSetup(const Document& doc) {
    const Store st = load(doc);
    for (const PageSetup& p : st.setups)
        if (lower(p.name) == lower(st.model))
            return p.name;
    return {};
}

std::optional<PageSetup> pageSetupOfLayout(const Document& doc, const std::string& layoutName) {
    auto l = findLayout(doc, layoutName);
    if (!l || l->pageSetup.empty())
        return std::nullopt;
    return findPageSetup(doc, l->pageSetup);
}

std::optional<PageSetup> pageSetupOfModel(const Document& doc) {
    const std::string name = modelPageSetup(doc);
    if (name.empty())
        return std::nullopt;
    return findPageSetup(doc, name);
}

exportlib::PlotSettings settingsFor(const exportlib::PlotSettings& base, const PageSetup& setup,
                                    const StyleResolver& resolver) {
    exportlib::PlotSettings s = setup.settings;
    if (s.area == exportlib::PlotArea::Display)
        s.window = base.window;
    if (resolver) {
        const std::string n = lower(setup.styleName);
        s.styleTable = (n.empty() || n == "none") ? nullptr : resolver(setup.styleName);
    } else {
        s.styleTable = base.styleTable;
    }
    return s;
}

PageSetup makePageSetup(std::string name, const exportlib::PlotSettings& settings,
                        std::string styleName) {
    PageSetup p;
    p.name = std::move(name);
    p.settings = settings;
    p.settings.styleTable.reset();
    p.styleName = std::move(styleName);
    return p;
}

}  // namespace tbcad::layout
