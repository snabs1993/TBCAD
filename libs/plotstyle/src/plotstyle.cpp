#include "tbcad/plotstyle/plotstyle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace tbcad::plotstyle {

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string hex(Rgba c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::optional<Rgba> parseHex(const std::string& s) {
    if (s.size() != 7 || s[0] != '#')
        return std::nullopt;
    for (std::size_t i = 1; i < 7; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(s[i])))
            return std::nullopt;
    auto byte = [&](std::size_t at) {
        return static_cast<std::uint8_t>(std::strtoul(s.substr(at, 2).c_str(), nullptr, 16));
    };
    return Rgba{byte(1), byte(3), byte(5), 255};
}

Rgba blendToWhite(Rgba c, int screen) {
    const double k = std::clamp(screen, 0, 100) / 100.0;
    auto mix = [k](std::uint8_t v) {
        return static_cast<std::uint8_t>(std::lround(v * k + 255.0 * (1.0 - k)));
    };
    return {mix(c.r), mix(c.g), mix(c.b), c.a};
}

}  // namespace

int paletteIndexOf(Rgba color) {
    static const std::array<Rgba, 256> palette = [] {
        std::array<Rgba, 256> p{};
        for (int i = 0; i < 256; ++i)
            p[static_cast<std::size_t>(i)] = paletteColor(static_cast<std::uint8_t>(i));
        return p;
    }();
    color.a = 255;
    for (int i = 1; i < 256; ++i) {
        Rgba p = palette[static_cast<std::size_t>(i)];
        p.a = 255;
        if (p == color)
            return i;
    }
    return 0;
}

int nearestPaletteIndex(Rgba color) {
    if (int exact = paletteIndexOf(color))
        return exact;
    int best = 0;
    long bestD = -1;
    for (int i = 1; i < 256; ++i) {
        const Rgba p = paletteColor(static_cast<std::uint8_t>(i));
        const long dr = static_cast<long>(p.r) - color.r, dg = static_cast<long>(p.g) - color.g,
                   db = static_cast<long>(p.b) - color.b;
        const long d = dr * dr + dg * dg + db * db;
        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

Rgba applyEntry(const PlotStyleEntry& e, Rgba base) {
    Rgba c = e.color.value_or(base);
    if (e.grayscale) {
        const double lum = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
        const auto g = static_cast<std::uint8_t>(std::lround(lum));
        c = {g, g, g, c.a};
    }
    if (e.screen < 100)
        c = blendToWhite(c, e.screen);
    return c;
}

Rgba mapColor(const PlotStyleTable& table, Rgba objectColor) {
    const int aci = paletteIndexOf(objectColor);
    if (aci == 0)
        return objectColor;
    return applyEntry(table.forIndex(aci), objectColor);
}

double mapLineweight(const PlotStyleTable& table, Rgba objectColor, double objectMm) {
    const int aci = paletteIndexOf(objectColor);
    if (aci == 0)
        return objectMm;
    return table.forIndex(aci).lineweightMm.value_or(objectMm);
}

std::vector<std::string> builtinTableNames() {
    return {"monochrome.ctb",   "grayscale.ctb",    "screening-25.ctb",
            "screening-50.ctb", "screening-75.ctb", "acad.ctb"};
}

std::optional<PlotStyleTable> builtinTable(const std::string& name) {
    const std::string n = lower(name);
    PlotStyleTable t;
    t.name = n;
    if (n == "monochrome.ctb") {
        t.description = "Everything is plotted in black";
        for (int i = 1; i < 256; ++i)
            t.entries[static_cast<std::size_t>(i)].color = Rgba{0, 0, 0, 255};
    } else if (n == "grayscale.ctb") {
        t.description = "Colours are plotted as greys of the same lightness";
        for (int i = 1; i < 256; ++i)
            t.entries[static_cast<std::size_t>(i)].grayscale = true;
    } else if (n == "screening-25.ctb" || n == "screening-50.ctb" || n == "screening-75.ctb") {
        const int pct = n == "screening-25.ctb" ? 25 : n == "screening-50.ctb" ? 50 : 75;
        t.description = "Colours are plotted at " + std::to_string(pct) + "% strength";
        for (int i = 1; i < 256; ++i)
            t.entries[static_cast<std::size_t>(i)].screen = pct;
    } else if (n == "acad.ctb") {
        t.description = "Colours and lineweights as drawn";
    } else {
        return std::nullopt;
    }
    return t;
}

std::string toText(const PlotStyleTable& table) {
    std::ostringstream o;
    o << "TBCAD-PLOT-STYLE 1\n";
    o << "name " << table.name << '\n';
    o << "description " << table.description << '\n';
    const PlotStyleEntry plain;
    for (int i = 1; i < 256; ++i) {
        const PlotStyleEntry& e = table.entries[static_cast<std::size_t>(i)];
        if (e == plain)
            continue;
        o << i << " color=" << (e.color ? hex(*e.color) : "object") << " lineweight=";
        if (e.lineweightMm) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.4g", *e.lineweightMm);
            o << buf;
        } else {
            o << "object";
        }
        o << " screen=" << e.screen << " gray=" << (e.grayscale ? 1 : 0);
        if (!e.name.empty())
            o << " name=" << e.name;
        o << '\n';
    }
    return o.str();
}

Result<PlotStyleTable> fromText(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("TBCAD-PLOT-STYLE", 0) != 0)
        return Error{ErrorCode::Parse, "Not a TBCAD plot style table"};
    PlotStyleTable t;
    int lineNo = 1;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        if (line.rfind("name ", 0) == 0) {
            t.name = line.substr(5);
        } else if (line.rfind("description ", 0) == 0) {
            t.description = line.substr(12);
        } else if (std::isdigit(static_cast<unsigned char>(line[0]))) {
            std::istringstream ls(line);
            int idx = 0;
            ls >> idx;
            if (idx < 1 || idx > 255)
                return Error{ErrorCode::Parse,
                             "Line " + std::to_string(lineNo) + ": colour index must be 1 to 255"};
            PlotStyleEntry e;
            std::string kv;
            while (ls >> kv) {
                auto eq = kv.find('=');
                if (eq == std::string::npos)
                    return Error{ErrorCode::Parse, "Line " + std::to_string(lineNo) + ": " + kv};
                const std::string key = kv.substr(0, eq), val = kv.substr(eq + 1);
                if (key == "color") {
                    if (val != "object") {
                        auto c = parseHex(val);
                        if (!c)
                            return Error{ErrorCode::Parse,
                                         "Line " + std::to_string(lineNo) + ": bad colour " + val};
                        e.color = *c;
                    }
                } else if (key == "lineweight") {
                    if (val != "object") {
                        char* end = nullptr;
                        double v = std::strtod(val.c_str(), &end);
                        if (end == val.c_str() || *end || v < 0 || v > 20)
                            return Error{ErrorCode::Parse, "Line " + std::to_string(lineNo) +
                                                               ": bad lineweight " + val};
                        e.lineweightMm = v;
                    }
                } else if (key == "screen") {
                    e.screen = std::clamp(std::atoi(val.c_str()), 0, 100);
                } else if (key == "gray") {
                    e.grayscale = val == "1";
                } else if (key == "name") {
                    e.name = val;
                }
            }
            // Tables imported from a .ctb by an earlier version carry the file's white
            // placeholder colour (for "use object colour") as a real white pen, which prints the
            // object white. A CTB-named entry (Color_N) with a white pen is that placeholder.
            if (e.color && *e.color == Rgba{255, 255, 255, 255} &&
                e.name.find("Color_") != std::string::npos)
                e.color.reset();
            t.entries[static_cast<std::size_t>(idx)] = e;
        }
    }
    return t;
}

Result<PlotStyleTable> loadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return Error{ErrorCode::Io, "Cannot read " + path.string()};
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string data = buf.str();
    const std::string ext = lower(path.extension().string());
    Result<PlotStyleTable> r =
        (ext == ".ctb" || ext == ".stb")
            ? importCtb({reinterpret_cast<const std::uint8_t*>(data.data()), data.size()})
            : fromText(data);
    if (r.ok() && r.value().name.empty())
        r.value().name = path.filename().string();
    return r;
}

Result<void> saveFile(const PlotStyleTable& table, const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return Error{ErrorCode::Io, "Cannot write " + path.string()};
    out << toText(table);
    return out ? Result<void>{}
               : Result<void>{Error{ErrorCode::Io, "Write failed: " + path.string()}};
}

}  // namespace tbcad::plotstyle
