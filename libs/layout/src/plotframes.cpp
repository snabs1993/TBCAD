#include "tbcad/layout/plotframes.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
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

std::string encode(const PlotFrame& f) {
    std::ostringstream o;
    o << "F\t" << f.name;
    auto kv = [&](const char* k, const std::string& v) { o << '\t' << k << '=' << v; };
    kv("window", num(f.window.min.x) + "," + num(f.window.min.y) + "," + num(f.window.max.x) + "," +
                     num(f.window.max.y));
    kv("paper", f.paperName);
    kv("cw", num(f.customWidthMm));
    kv("ch", num(f.customHeightMm));
    kv("landscape", f.landscape ? "1" : "0");
    kv("fit", f.fitToPaper ? "1" : "0");
    kv("scale", num(f.scaleN));
    if (!f.pageSetup.empty())
        kv("setup", f.pageSetup);
    return o.str();
}

PlotFrame decode(const std::vector<std::string>& f) {
    PlotFrame p;
    p.name = f[1];
    for (std::size_t i = 2; i < f.size(); ++i) {
        const std::size_t eq = f[i].find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = f[i].substr(0, eq), v = f[i].substr(eq + 1);
        auto d = [&] { return std::strtod(v.c_str(), nullptr); };
        if (k == "window") {
            const auto n = split(v, ',');
            if (n.size() == 4) {
                p.window = {};
                p.window.expand(
                    {std::strtod(n[0].c_str(), nullptr), std::strtod(n[1].c_str(), nullptr)});
                p.window.expand(
                    {std::strtod(n[2].c_str(), nullptr), std::strtod(n[3].c_str(), nullptr)});
            }
        } else if (k == "paper")
            p.paperName = v;
        else if (k == "cw")
            p.customWidthMm = d();
        else if (k == "ch")
            p.customHeightMm = d();
        else if (k == "landscape")
            p.landscape = v == "1";
        else if (k == "fit")
            p.fitToPaper = v == "1";
        else if (k == "scale")
            p.scaleN = d();
        else if (k == "setup")
            p.pageSetup = v;
    }
    return p;
}

std::vector<PlotFrame> load(const Document& doc) {
    std::vector<PlotFrame> out;
    auto it = doc.header().custom.find(kPlotFrameKey);
    if (it == doc.header().custom.end())
        return out;
    for (const std::string& line : split(it->second, '\n')) {
        const auto f = split(line, '\t');
        if (f.size() >= 2 && f[0] == "F" && !f[1].empty())
            out.push_back(decode(f));
    }
    return out;
}

Result<void> save(Transaction& tx, const std::vector<PlotFrame>& frames) {
    std::ostringstream o;
    for (const PlotFrame& f : frames)
        o << encode(f) << '\n';
    const std::string text = o.str();
    return tx.modifyHeader([&](Header& h) {
        if (text.empty())
            h.custom.erase(kPlotFrameKey);
        else
            h.custom[kPlotFrameKey] = text;
    });
}

PlotFrame* pick(std::vector<PlotFrame>& frames, std::string_view name) {
    for (PlotFrame& f : frames)
        if (lower(f.name) == lower(name))
            return &f;
    return nullptr;
}

std::vector<PlotSet> loadSets(const Document& doc) {
    std::vector<PlotSet> out;
    auto it = doc.header().custom.find(kPlotSetKey);
    if (it == doc.header().custom.end())
        return out;
    for (const std::string& line : split(it->second, '\n')) {
        const auto f = split(line, '\t');
        if (f.size() >= 2 && f[0] == "P" && !f[1].empty())
            out.push_back({f[1], std::vector<std::string>(f.begin() + 2, f.end())});
    }
    return out;
}

Result<void> saveSets(Transaction& tx, const std::vector<PlotSet>& sets) {
    std::ostringstream o;
    for (const PlotSet& p : sets) {
        o << "P\t" << p.name;
        for (const std::string& f : p.frames)
            o << '\t' << f;
        o << '\n';
    }
    const std::string text = o.str();
    return tx.modifyHeader([&](Header& h) {
        if (text.empty())
            h.custom.erase(kPlotSetKey);
        else
            h.custom[kPlotSetKey] = text;
    });
}

PlotSet* pickSet(std::vector<PlotSet>& sets, std::string_view name) {
    for (PlotSet& p : sets)
        if (lower(p.name) == lower(name))
            return &p;
    return nullptr;
}

// Applies `fn` to every frame name of every set (empty result: the frame is taken out).
Result<void> updateSetFrames(Transaction& tx,
                             const std::function<std::string(const std::string&)>& fn) {
    std::vector<PlotSet> sets = loadSets(tx.document());
    bool changed = false;
    for (PlotSet& p : sets) {
        std::vector<std::string> now;
        for (const std::string& f : p.frames)
            if (std::string n = fn(f); !n.empty())
                now.push_back(std::move(n));
        changed = changed || now != p.frames;
        p.frames = std::move(now);
    }
    return changed ? saveSets(tx, sets) : Result<void>{};
}

Result<void> validateName(const char* what, std::string_view name) {
    if (name.empty() || name.size() > 64)
        return err(ErrorCode::InvalidArgument,
                   std::string("A ") + what + " name has 1 to 64 characters");
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == ',')
            return err(
                ErrorCode::InvalidArgument,
                std::string("A ") + what + " name cannot contain commas or control characters");
    if (name.front() == ' ' || name.back() == ' ')
        return err(ErrorCode::InvalidArgument,
                   std::string("A ") + what + " name cannot start or end with a space");
    return {};
}

}  // namespace

std::vector<PlotFrame> plotFrames(const Document& doc) {
    return load(doc);
}

std::optional<PlotFrame> findPlotFrame(const Document& doc, std::string_view name) {
    for (PlotFrame& f : load(doc))
        if (lower(f.name) == lower(name))
            return std::move(f);
    return std::nullopt;
}

Result<void> validatePlotFrameName(const Document& doc, std::string_view name,
                                   std::string_view ignore) {
    if (auto ok = validateName("plot frame", name); !ok)
        return ok;
    for (const PlotFrame& f : load(doc))
        if (lower(f.name) == lower(name) && lower(f.name) != lower(ignore))
            return err(ErrorCode::Conflict, "A plot frame named \"" + f.name + "\" already exists");
    return {};
}

Result<void> savePlotFrame(Transaction& tx, const PlotFrame& frame) {
    if (frame.window.empty())
        return err(ErrorCode::InvalidArgument, "The plot frame has no area");
    if (!frame.fitToPaper && !(frame.scaleN > 0))
        return err(ErrorCode::InvalidArgument, "The scale must be positive");
    std::vector<PlotFrame> frames = load(tx.document());
    if (PlotFrame* old = pick(frames, frame.name)) {
        PlotFrame f = frame;
        f.name = old->name;
        *old = std::move(f);
    } else {
        if (auto ok = validatePlotFrameName(tx.document(), frame.name); !ok)
            return ok;
        frames.push_back(frame);
    }
    return save(tx, frames);
}

Result<void> deletePlotFrame(Transaction& tx, const std::string& name) {
    std::vector<PlotFrame> frames = load(tx.document());
    PlotFrame* f = pick(frames, name);
    if (!f)
        return err(ErrorCode::NotFound, "No plot frame named \"" + name + "\"");
    const std::string real = f->name;
    frames.erase(frames.begin() + (f - frames.data()));
    if (auto r = save(tx, frames); !r)
        return r;
    return updateSetFrames(
        tx, [&](const std::string& n) { return lower(n) == lower(real) ? std::string() : n; });
}

Result<void> renamePlotFrame(Transaction& tx, const std::string& name, const std::string& newName) {
    if (auto ok = validatePlotFrameName(tx.document(), newName, name); !ok)
        return ok;
    std::vector<PlotFrame> frames = load(tx.document());
    PlotFrame* f = pick(frames, name);
    if (!f)
        return err(ErrorCode::NotFound, "No plot frame named \"" + name + "\"");
    const std::string real = f->name;
    f->name = newName;
    if (auto r = save(tx, frames); !r)
        return r;
    return updateSetFrames(
        tx, [&](const std::string& n) { return lower(n) == lower(real) ? newName : n; });
}

std::string nextPlotFrameName(const Document& doc) {
    const std::vector<PlotFrame> frames = load(doc);
    for (int n = 1;; ++n) {
        const std::string name = "Sheet " + std::to_string(n);
        bool taken = false;
        for (const PlotFrame& f : frames)
            taken = taken || lower(f.name) == lower(name);
        if (!taken)
            return name;
    }
}

BBox2 frameBoxForPaper(const std::string& paperName, bool landscape, double marginMm, double scaleN,
                       Vec2 corner, double customWidthMm, double customHeightMm) {
    double w = customWidthMm, h = customHeightMm;
    if (paperName != "Custom") {
        const exportlib::PaperSize* p = exportlib::findPaperSize(paperName);
        if (!p)
            return {};
        w = p->shortMm;
        h = p->longMm;
    }
    if (landscape != (w > h))
        std::swap(w, h);
    const double m = std::max(0.0, marginMm);
    w -= 2 * m;
    h -= 2 * m;
    if (!(w > 0) || !(h > 0) || !(scaleN > 0))
        return {};
    BBox2 b;
    b.expand(corner);
    b.expand({corner.x + w * scaleN, corner.y + h * scaleN});
    return b;
}

exportlib::PlotSettings frameSettings(const Document& doc, const exportlib::PlotSettings& base,
                                      const PlotFrame& frame, const StyleResolver& resolver) {
    exportlib::PlotSettings s = base;
    if (!frame.pageSetup.empty())
        if (auto setup = findPageSetup(doc, frame.pageSetup))
            s = settingsFor(base, *setup, resolver);
    s.area = exportlib::PlotArea::Window;
    s.window = frame.window;
    s.paperName = frame.paperName;
    s.customWidthMm = frame.customWidthMm;
    s.customHeightMm = frame.customHeightMm;
    s.landscape = frame.landscape;
    s.fitToPaper = frame.fitToPaper;
    s.scaleN = frame.scaleN;
    s.centerOnPaper = true;
    s.annoScale = frame.fitToPaper ? 0 : frame.scaleN;
    return s;
}

std::string describeFrame(const PlotFrame& frame) {
    std::string t = frame.paperName + (frame.landscape ? " landscape " : " portrait ");
    if (frame.fitToPaper) {
        t += "Fit";
    } else {
        char b[32];
        std::snprintf(b, sizeof b, "1:%.4g", frame.scaleN);
        t += b;
    }
    return t;
}

std::vector<PlotSet> plotSets(const Document& doc) {
    return loadSets(doc);
}

std::optional<PlotSet> findPlotSet(const Document& doc, std::string_view name) {
    for (PlotSet& p : loadSets(doc))
        if (lower(p.name) == lower(name))
            return std::move(p);
    return std::nullopt;
}

Result<void> validatePlotSetName(const Document& doc, std::string_view name,
                                 std::string_view ignore) {
    if (auto ok = validateName("plot set", name); !ok)
        return ok;
    for (const PlotSet& p : loadSets(doc))
        if (lower(p.name) == lower(name) && lower(p.name) != lower(ignore))
            return err(ErrorCode::Conflict, "A plot set named \"" + p.name + "\" already exists");
    return {};
}

Result<void> savePlotSet(Transaction& tx, const PlotSet& set) {
    PlotSet clean{set.name, {}};
    for (const std::string& n : set.frames) {
        auto f = findPlotFrame(tx.document(), n);
        if (!f)
            return err(ErrorCode::NotFound, "No plot frame named \"" + n + "\"");
        bool have = false;
        for (const std::string& c : clean.frames)
            have = have || c == f->name;
        if (!have)
            clean.frames.push_back(f->name);
    }
    std::vector<PlotSet> sets = loadSets(tx.document());
    if (PlotSet* old = pickSet(sets, set.name)) {
        clean.name = old->name;
        *old = std::move(clean);
    } else {
        if (auto ok = validatePlotSetName(tx.document(), set.name); !ok)
            return ok;
        sets.push_back(std::move(clean));
    }
    return saveSets(tx, sets);
}

Result<void> deletePlotSet(Transaction& tx, const std::string& name) {
    std::vector<PlotSet> sets = loadSets(tx.document());
    PlotSet* p = pickSet(sets, name);
    if (!p)
        return err(ErrorCode::NotFound, "No plot set named \"" + name + "\"");
    sets.erase(sets.begin() + (p - sets.data()));
    return saveSets(tx, sets);
}

Result<void> renamePlotSet(Transaction& tx, const std::string& name, const std::string& newName) {
    if (auto ok = validatePlotSetName(tx.document(), newName, name); !ok)
        return ok;
    std::vector<PlotSet> sets = loadSets(tx.document());
    PlotSet* p = pickSet(sets, name);
    if (!p)
        return err(ErrorCode::NotFound, "No plot set named \"" + name + "\"");
    p->name = newName;
    return saveSets(tx, sets);
}

Result<void> copyPlotSet(Transaction& tx, const std::string& name, const std::string& newName) {
    auto p = findPlotSet(tx.document(), name);
    if (!p)
        return err(ErrorCode::NotFound, "No plot set named \"" + name + "\"");
    if (auto ok = validatePlotSetName(tx.document(), newName); !ok)
        return ok;
    p->name = newName;
    return savePlotSet(tx, *p);
}

Result<void> addToPlotSet(Transaction& tx, const std::string& name,
                          const std::vector<std::string>& frames) {
    auto p = findPlotSet(tx.document(), name);
    if (!p)
        return err(ErrorCode::NotFound, "No plot set named \"" + name + "\"");
    p->frames.insert(p->frames.end(), frames.begin(), frames.end());
    return savePlotSet(tx, *p);
}

Result<void> removeFromPlotSet(Transaction& tx, const std::string& name,
                               const std::vector<std::string>& frames) {
    auto p = findPlotSet(tx.document(), name);
    if (!p)
        return err(ErrorCode::NotFound, "No plot set named \"" + name + "\"");
    std::vector<std::string> keep;
    for (const std::string& f : p->frames) {
        bool drop = false;
        for (const std::string& r : frames)
            drop = drop || lower(r) == lower(f);
        if (!drop)
            keep.push_back(f);
    }
    p->frames = std::move(keep);
    return savePlotSet(tx, *p);
}

std::string nextPlotSetName(const Document& doc) {
    for (int n = 1;; ++n) {
        const std::string name = "Set " + std::to_string(n);
        if (!findPlotSet(doc, name))
            return name;
    }
}

Result<std::vector<std::string>> resolveFrameList(const Document& doc, const std::string& text) {
    const std::vector<PlotFrame> frames = load(doc);
    std::vector<std::string> out;
    auto trim = [](std::string t) {
        const auto a = t.find_first_not_of(" \t");
        if (a == std::string::npos)
            return std::string();
        return t.substr(a, t.find_last_not_of(" \t") - a + 1);
    };
    auto isNumber = [](const std::string& t) {
        return !t.empty() && t.find_first_not_of("0123456789") == std::string::npos;
    };
    auto byNumber = [&](const std::string& t) -> const PlotFrame* {
        const long n = std::strtol(t.c_str(), nullptr, 10);
        return n >= 1 && n <= static_cast<long>(frames.size())
                   ? &frames[static_cast<std::size_t>(n - 1)]
                   : nullptr;
    };
    for (const std::string& raw : split(text, ',')) {
        const std::string item = trim(raw);
        if (item.empty())
            continue;
        // A frame whose name this is wins over a number or a range.
        if (auto f = findPlotFrame(doc, item)) {
            out.push_back(f->name);
            continue;
        }
        if (isNumber(item)) {
            const PlotFrame* f = byNumber(item);
            if (!f)
                return err(ErrorCode::NotFound, "There is no plot frame number " + item);
            out.push_back(f->name);
            continue;
        }
        const auto dash = item.find('-');
        if (dash != std::string::npos) {
            const std::string a = trim(item.substr(0, dash)), b = trim(item.substr(dash + 1));
            if (isNumber(a) && isNumber(b)) {
                const PlotFrame* fa = byNumber(a);
                const PlotFrame* fb = byNumber(b);
                if (!fa || !fb)
                    return err(ErrorCode::NotFound,
                               "There is no plot frame number " + (fa ? b : a));
                std::size_t i = static_cast<std::size_t>(fa - frames.data());
                const std::size_t j = static_cast<std::size_t>(fb - frames.data());
                for (;; i = i < j ? i + 1 : i - 1) {
                    out.push_back(frames[i].name);
                    if (i == j)
                        break;
                }
                continue;
            }
        }
        return err(ErrorCode::NotFound, "No plot frame named \"" + item + "\"");
    }
    return out;
}

}  // namespace tbcad::layout
