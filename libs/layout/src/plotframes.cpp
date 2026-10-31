#include "tbcad/layout/plotframes.h"

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
    if (name.empty() || name.size() > 64)
        return err(ErrorCode::InvalidArgument, "A plot frame name has 1 to 64 characters");
    for (char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == ',')
            return err(ErrorCode::InvalidArgument,
                       "A plot frame name cannot contain commas or control characters");
    if (name.front() == ' ' || name.back() == ' ')
        return err(ErrorCode::InvalidArgument,
                   "A plot frame name cannot start or end with a space");
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
    frames.erase(frames.begin() + (f - frames.data()));
    return save(tx, frames);
}

Result<void> renamePlotFrame(Transaction& tx, const std::string& name, const std::string& newName) {
    if (auto ok = validatePlotFrameName(tx.document(), newName, name); !ok)
        return ok;
    std::vector<PlotFrame> frames = load(tx.document());
    PlotFrame* f = pick(frames, name);
    if (!f)
        return err(ErrorCode::NotFound, "No plot frame named \"" + name + "\"");
    f->name = newName;
    return save(tx, frames);
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

}  // namespace tbcad::layout
