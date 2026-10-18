#include "tbcad/gdt/gdt.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace tbcad::gdt {

namespace {

constexpr double kCell = 5.0;  // height of the frame and width of the symbol cell
constexpr double kText = 2.5;  // text height
constexpr double kPad = 1.2;   // between a divider and the text
constexpr double kPi_ = 3.14159265358979323846;

struct Sym {
    Symbol symbol;
    const char* name;
};
constexpr Sym kSymbols[] = {
    {Symbol::Straightness, "Straightness"},     {Symbol::Flatness, "Flatness"},
    {Symbol::Circularity, "Circularity"},       {Symbol::Cylindricity, "Cylindricity"},
    {Symbol::ProfileLine, "ProfileLine"},       {Symbol::ProfileSurface, "ProfileSurface"},
    {Symbol::Angularity, "Angularity"},         {Symbol::Perpendicularity, "Perpendicularity"},
    {Symbol::Parallelism, "Parallelism"},       {Symbol::Position, "Position"},
    {Symbol::Concentricity, "Concentricity"},   {Symbol::Symmetry, "Symmetry"},
    {Symbol::CircularRunout, "CircularRunout"}, {Symbol::TotalRunout, "TotalRunout"},
};

struct Builder {
    std::vector<Entity> out;
    Vec2 origin{};  // current cell centre offset

    void line(Vec2 a, Vec2 b) {
        Entity e;
        e.data = LineData{a, b};
        out.push_back(std::move(e));
    }
    void circle(Vec2 c, double r) {
        Entity e;
        e.data = CircleData{c, r};
        out.push_back(std::move(e));
    }
    void arc(Vec2 c, double r, double a0, double a1) {
        Entity e;
        e.data = ArcData{c, r, a0, a1};
        out.push_back(std::move(e));
    }
    void poly(std::vector<Vec2> pts, bool closed) {
        PolylineData p;
        p.closed = closed;
        for (Vec2 v : pts)
            p.v.push_back(PolyVertex{.p = v});
        Entity e;
        e.data = std::move(p);
        out.push_back(std::move(e));
    }
    void text(Vec2 at, double height, const std::string& s, HAlign h) {
        TextData t;
        t.position = t.alignPoint = at;
        t.height = height;
        t.utf8 = s;
        t.h = h;
        t.v = VAlign::Middle;
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        out.push_back(std::move(e));
    }
};

// The symbol, centred on (cx, cy), about 3 mm across.
void drawSymbol(Builder& b, Symbol s, double cx, double cy) {
    auto P = [&](double x, double y) { return Vec2{cx + x, cy + y}; };
    auto arrow = [&](double x0, double y0, double x1, double y1) {
        b.line(P(x0, y0), P(x1, y1));
        const double a = std::atan2(y1 - y0, x1 - x0), L = 1.0, w = 0.4;
        const Vec2 tip = P(x1, y1);
        const Vec2 back = tip - Vec2{std::cos(a), std::sin(a)} * L;
        const Vec2 side{-std::sin(a) * w, std::cos(a) * w};
        b.poly({tip, back + side, back - side}, true);
    };
    switch (s) {
        case Symbol::Straightness:
            b.line(P(-1.5, 0), P(1.5, 0));
            break;
        case Symbol::Flatness:
            b.poly({P(-1.6, -0.7), P(0.8, -0.7), P(1.6, 0.7), P(-0.8, 0.7)}, true);
            break;
        case Symbol::Circularity:
            b.circle(P(0, 0), 1.3);
            break;
        case Symbol::Cylindricity:
            b.circle(P(0, 0), 1.0);
            b.line(P(-2.0, -1.1), P(-1.0, 1.1));
            b.line(P(1.0, -1.1), P(2.0, 1.1));
            break;
        case Symbol::ProfileLine:
            b.arc(P(0, -0.8), 1.6, 0, kPi_);
            break;
        case Symbol::ProfileSurface:
            b.arc(P(0, -0.8), 1.6, 0, kPi_);
            b.line(P(-1.6, -0.8), P(1.6, -0.8));
            break;
        case Symbol::Angularity:
            b.line(P(-1.6, -1.2), P(1.6, -1.2));
            b.line(P(-1.6, -1.2), P(1.0, 1.4));
            break;
        case Symbol::Perpendicularity:
            b.line(P(-1.6, -1.2), P(1.6, -1.2));
            b.line(P(0, -1.2), P(0, 1.4));
            break;
        case Symbol::Parallelism:
            b.line(P(-1.5, -1.3), P(-0.2, 1.3));
            b.line(P(0.2, -1.3), P(1.5, 1.3));
            break;
        case Symbol::Position:
            b.circle(P(0, 0), 1.4);
            b.line(P(-2.2, 0), P(2.2, 0));
            b.line(P(0, -2.2), P(0, 2.2));
            break;
        case Symbol::Concentricity:
            b.circle(P(0, 0), 1.5);
            b.circle(P(0, 0), 0.8);
            break;
        case Symbol::Symmetry:
            b.line(P(-1.6, 1.0), P(1.6, 1.0));
            b.line(P(-1.6, 0.0), P(1.6, 0.0));
            b.line(P(-1.6, -1.0), P(1.6, -1.0));
            break;
        case Symbol::CircularRunout:
            arrow(-1.3, -1.3, 1.3, 1.3);
            break;
        case Symbol::TotalRunout:
            arrow(-1.6, -1.2, 0.9, 1.3);
            arrow(-0.6, -1.5, 1.9, 1.0);
            break;
    }
}

// A material condition modifier: the letter in a small circle.
void drawModifier(Builder& b, char m, double x, double cy) {
    b.circle({x + 1.0, cy}, 1.0);
    b.text({x + 1.0, cy}, 1.4, std::string(1, m), HAlign::Center);
}

double textWidth(const std::string& s) {
    // About 0.62 of the text height per character; a glyph outside ASCII counts the same.
    double n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            n += 1;
    return n * kText * 0.62;
}

}  // namespace

const char* symbolName(Symbol s) {
    for (const Sym& e : kSymbols)
        if (e.symbol == s)
            return e.name;
    return "";
}

std::optional<Symbol> symbolByName(std::string_view name) {
    for (const Sym& e : kSymbols)
        if (name == e.name)
            return e.symbol;
    return std::nullopt;
}

std::vector<Entity> frameEntities(const Spec& spec) {
    Builder b;
    double x = 0;  // left edge of the next cell
    const double top = kCell / 2, bottom = -kCell / 2;
    auto divider = [&](double at) { b.line({at, bottom}, {at, top}); };

    // The symbol cell.
    drawSymbol(b, spec.symbol, x + kCell / 2, 0);
    x += kCell;
    divider(x);

    // The tolerance: optional diameter sign, the value, the material modifier.
    std::string value = (spec.diameterZone ? "\xE2\x8C\x80" : "") + spec.value;  // U+2300
    double w = kPad + textWidth(value) + kPad;
    b.text({x + kPad, 0}, kText, value, HAlign::Left);
    if (spec.material) {
        drawModifier(b, spec.material, x + w - kPad + 0.2, 0);
        w += 2.2;
    }
    x += w;

    // The datums.
    for (const std::string& d : spec.datums) {
        if (d.empty())
            continue;
        divider(x);
        const double cw = std::max(kCell, 2 * kPad + textWidth(d));
        b.text({x + cw / 2, 0}, kText, d, HAlign::Center);
        x += cw;
    }

    // The outline.
    b.poly({{0, bottom}, {x, bottom}, {x, top}, {0, top}}, true);
    return std::move(b.out);
}

// ---- the table
// -----------------------------------------------------------------------------------

namespace {
constexpr const char* kTableKey = "tbcad.tolerances";

std::string clean(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r' || c == ',')
            c = ' ';
    return s;
}
}  // namespace

FrameTable readFrames(const Header& header) {
    FrameTable table;
    auto it = header.custom.find(kTableKey);
    if (it == header.custom.end())
        return table;
    std::istringstream in(it->second);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::size_t start = 0;
        for (;;) {
            const auto tab = line.find('\t', start);
            f.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
            if (tab == std::string::npos)
                break;
            start = tab + 1;
        }
        if (f.size() != 6 || f[0].empty())
            continue;
        const auto sym = symbolByName(f[1]);
        if (!sym)
            continue;
        Spec s;
        s.symbol = *sym;
        s.value = f[2];
        s.diameterZone = f[3] == "1";
        s.material = f[4].empty() ? 0 : f[4][0];
        std::size_t p = 0;
        while (p <= f[5].size() && !f[5].empty()) {
            const auto comma = f[5].find(',', p);
            s.datums.push_back(f[5].substr(p, comma == std::string::npos ? comma : comma - p));
            if (comma == std::string::npos)
                break;
            p = comma + 1;
        }
        table[f[0]] = std::move(s);
    }
    return table;
}

void writeFrames(Header& header, const FrameTable& table) {
    std::string text;
    for (const auto& [name, s] : table) {
        text += clean(name) + '\t' + symbolName(s.symbol) + '\t' + clean(s.value) + '\t' +
                (s.diameterZone ? "1" : "0") + '\t' +
                (s.material ? std::string(1, s.material) : "") + '\t';
        for (std::size_t i = 0; i < s.datums.size(); ++i)
            text += (i ? "," : "") + clean(s.datums[i]);
        text += '\n';
    }
    if (text.empty())
        header.custom.erase(kTableKey);
    else
        header.custom[kTableKey] = std::move(text);
}

Result<void> addFrame(Transaction& tx, const std::string& blockName, const Spec& spec) {
    return tx.modifyHeader([&](Header& h) {
        FrameTable t = readFrames(h);
        t[blockName] = spec;
        writeFrames(h, t);
    });
}

// ---- DXF text
// ------------------------------------------------------------------------------------

namespace {

struct Code {
    char letter;
    Symbol symbol;
};
// The characters of AutoCAD's gdt font.
constexpr Code kCodes[] = {
    {'a', Symbol::Angularity},     {'b', Symbol::Perpendicularity}, {'c', Symbol::Flatness},
    {'d', Symbol::ProfileSurface}, {'e', Symbol::Circularity},      {'f', Symbol::Parallelism},
    {'g', Symbol::Cylindricity},   {'h', Symbol::CircularRunout},   {'i', Symbol::Symmetry},
    {'j', Symbol::Position},       {'k', Symbol::ProfileLine},      {'r', Symbol::Concentricity},
    {'t', Symbol::TotalRunout},    {'u', Symbol::Straightness},
};

// A cell as plain text plus the gdt font codes in it, in order.
struct Cell {
    std::string plain{};
    std::string codes{};
};

Cell parseCell(std::string_view s) {
    Cell c;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '{' || s[i] == '}') {
            ++i;
            continue;
        }
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'F') {
            const auto semi = s.find(';', i);
            if (semi != std::string_view::npos && semi + 1 < s.size()) {
                c.codes += s[semi + 1];
                i = semi + 2;
                continue;
            }
        }
        if (s[i] == '%' && i + 2 < s.size() && s[i + 1] == '%' &&
            (s[i + 2] == 'c' || s[i + 2] == 'C')) {
            c.codes += 'n';
            i += 3;
            continue;
        }
        if (static_cast<unsigned char>(s[i]) == 0xE2 && i + 2 < s.size() &&
            static_cast<unsigned char>(s[i + 1]) == 0x8C &&
            static_cast<unsigned char>(s[i + 2]) == 0x80) {
            c.codes += 'n';  // U+2300
            i += 3;
            continue;
        }
        if (static_cast<unsigned char>(s[i]) == 0xC3 && i + 1 < s.size() &&
            static_cast<unsigned char>(s[i + 1]) == 0x98) {
            c.codes += 'n';  // U+00D8
            i += 2;
            continue;
        }
        c.plain += s[i++];
    }
    return c;
}

std::string trimmed(std::string s) {
    const auto a = s.find_first_not_of(" \t");
    if (a == std::string::npos)
        return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

}  // namespace

std::string dxfText(const Spec& spec) {
    char sym = 'j';
    for (const Code& c : kCodes)
        if (c.symbol == spec.symbol)
            sym = c.letter;
    std::string out = std::string("{\\Fgdt;") + sym + "}%%v";
    if (spec.diameterZone)
        out += "{\\Fgdt;n}";
    out += spec.value;
    if (spec.material)
        out += spec.material == 'M' ? "{\\Fgdt;m}" : "{\\Fgdt;l}";
    out += "%%v%%v";  // no second tolerance
    for (int i = 0; i < 3; ++i)
        out += (i < static_cast<int>(spec.datums.size()) ? spec.datums[i] : std::string()) + "%%v";
    out += "%%v";  // no height, no datum identifier
    return out;
}

std::optional<Spec> parseDxfText(std::string_view text) {
    // Only the first line of a multi-line frame.
    for (std::string_view brk : {std::string_view("\n"), std::string_view("^J")}) {
        const auto at = text.find(brk);
        if (at != std::string_view::npos)
            text = text.substr(0, at);
    }
    std::vector<std::string_view> cells;
    for (std::size_t start = 0;;) {
        const auto at = text.find("%%v", start);
        cells.push_back(text.substr(start, at == std::string_view::npos ? at : at - start));
        if (at == std::string_view::npos)
            break;
        start = at + 3;
    }
    if (cells.size() < 2)
        return std::nullopt;
    const Cell symbolCell = parseCell(cells[0]);
    std::optional<Symbol> symbol;
    for (char ch : symbolCell.codes)
        for (const Code& c : kCodes)
            if (c.letter == ch)
                symbol = c.symbol;
    if (!symbol)
        return std::nullopt;
    Spec spec;
    spec.symbol = *symbol;
    const Cell tol = parseCell(cells[1]);
    spec.diameterZone = tol.codes.find('n') != std::string::npos;
    if (tol.codes.find('m') != std::string::npos)
        spec.material = 'M';
    else if (tol.codes.find('l') != std::string::npos)
        spec.material = 'L';
    spec.value = trimmed(tol.plain);
    if (spec.value.empty())
        return std::nullopt;
    for (std::size_t i = 3; i <= 5 && i < cells.size(); ++i) {
        const std::string d = trimmed(parseCell(cells[i]).plain);
        if (d.empty())
            break;
        spec.datums.push_back(d);
    }
    return spec;
}

}  // namespace tbcad::gdt
