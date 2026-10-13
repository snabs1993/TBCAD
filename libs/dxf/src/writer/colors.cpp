#include "colors.h"

#include <array>
#include <cmath>

#include "tbcad/core/units.h"

namespace tbcad::dxf::detail {

// Defined by the reader (aci.cpp).
Color mapAci(int aci);

namespace {

struct Rgb3 {
    double r, g, b;
};

Rgb3 hsv(double hueDeg, double s, double v) {
    double c = v * s;
    double hp = hueDeg / 60.0;
    double x = c * (1 - std::abs(std::fmod(hp, 2.0) - 1));
    double r = 0, g = 0, b = 0;
    if (hp < 1)
        r = c, g = x;
    else if (hp < 2)
        r = x, g = c;
    else if (hp < 3)
        g = c, b = x;
    else if (hp < 4)
        g = x, b = c;
    else if (hp < 5)
        r = x, b = c;
    else
        r = c, b = x;
    double m = v - c;
    return {(r + m) * 255, (g + m) * 255, (b + m) * 255};
}

// The public ACI layout (the same rule the reader uses to rebuild the table).
Rgb3 aciRgb(int i) {
    static constexpr double kValue[10] = {1.0, 1.0, 0.8, 0.8, 0.6, 0.6, 0.5, 0.5, 0.3, 0.3};
    static constexpr double kGray[6] = {0x33, 0x50, 0x69, 0x82, 0xBE, 0xFF};
    static constexpr Rgb3 kBasic[8] = {{0, 0, 0},     {255, 0, 0}, {255, 255, 0}, {0, 255, 0},
                                       {0, 255, 255}, {0, 0, 255}, {255, 0, 255}, {255, 255, 255}};
    if (i >= 1 && i <= 7)
        return kBasic[i];
    if (i == 8)
        return {128, 128, 128};
    if (i == 9)
        return {192, 192, 192};
    if (i >= 250)
        return {kGray[i - 250], kGray[i - 250], kGray[i - 250]};
    int n = i - 10;
    return hsv((n / 10) * 15.0, (n % 2 == 0) ? 1.0 : 0.5, kValue[n % 10]);
}

double dist2(Rgb3 a, Rgba b) {
    double dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

int nearestAci(Rgba c) {
    double best = 1e300;
    int bestAci = 7;
    for (int i = 1; i <= 255; ++i) {
        double d = dist2(aciRgb(i), c);
        if (d < best) {
            best = d;
            bestAci = i;
        }
    }
    return bestAci;
}

struct Tables {
    std::array<int, 256> preimage{};  // palette index -> ACI, 0 when no ACI maps to it
    Tables() {
        std::array<double, 256> best{};
        best.fill(1e300);
        for (int a = 1; a <= 255; ++a) {
            int idx = mapAci(a).indexValue();
            double d = dist2(aciRgb(a), paletteColor(static_cast<std::uint8_t>(idx)));
            if (a <= 7)
                d = -1;  // the basic colours always win
            if (d < best[static_cast<std::size_t>(idx)]) {
                best[static_cast<std::size_t>(idx)] = d;
                preimage[static_cast<std::size_t>(idx)] = a;
            }
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

std::uint32_t pack(Rgba c) {
    return (static_cast<std::uint32_t>(c.r) << 16) | (static_cast<std::uint32_t>(c.g) << 8) | c.b;
}

}  // namespace

AciChoice aciForIndex(int index) {
    if (index < 1 || index > 255)
        return {7, std::nullopt};
    int a = tables().preimage[static_cast<std::size_t>(index)];
    if (a != 0)
        return {a, std::nullopt};
    Rgba p = paletteColor(static_cast<std::uint8_t>(index));
    return {nearestAci(p), pack(p)};
}

AciChoice aciForRgba(Rgba c) {
    return {nearestAci(c), pack(c)};
}

AciChoice aciFor(Color c) {
    if (c.kind == ColorKind::Index)
        return aciForIndex(c.indexValue());
    if (c.kind == ColorKind::Rgb)
        return aciForRgba(Rgba{c.r, c.g, c.b, 255});
    return {7, std::nullopt};
}

int validLineweight(int h) {
    static constexpr int kValid[] = {0,  5,  9,  13, 15, 18,  20,  25,  30,  35,  40,  50,
                                     53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};
    int best = kValid[0];
    for (int v : kValid) {
        if (std::abs(v - h) < std::abs(best - h))
            best = v;
    }
    return best;
}

std::string encodeText(std::string_view s, bool utf8Mode) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    auto hex4 = [&](unsigned cp) {
        static const char* d = "0123456789ABCDEF";
        out += "\\U+";
        out.push_back(d[(cp >> 12) & 15]);
        out.push_back(d[(cp >> 8) & 15]);
        out.push_back(d[(cp >> 4) & 15]);
        out.push_back(d[cp & 15]);
    };
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(c < 0x20 && c != '\t' ? ' ' : static_cast<char>(c));
            ++i;
            continue;
        }
        // Decode one UTF-8 sequence; invalid bytes become U+FFFD.
        unsigned cp = 0xFFFD;
        std::size_t len = 1;
        if (c >= 0xC2 && c <= 0xDF && i + 1 < s.size() && (s[i + 1] & 0xC0) == 0x80) {
            cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF && i + 2 < s.size() && (s[i + 1] & 0xC0) == 0x80 &&
                   (s[i + 2] & 0xC0) == 0x80) {
            cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            len = 3;
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))
                cp = 0xFFFD;
        } else if (c >= 0xF0 && c <= 0xF4 && i + 3 < s.size() && (s[i + 1] & 0xC0) == 0x80 &&
                   (s[i + 2] & 0xC0) == 0x80 && (s[i + 3] & 0xC0) == 0x80) {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) |
                 (s[i + 3] & 0x3Fu);
            len = 4;
            if (cp < 0x10000 || cp > 0x10FFFF)
                cp = 0xFFFD;
        }
        if (utf8Mode) {
            if (cp == 0xFFFD) {
                out += "\xEF\xBF\xBD";
            } else {
                out.append(s.substr(i, len));
            }
        } else if (cp >= 0x10000) {
            unsigned v = cp - 0x10000;
            hex4(0xD800 + (v >> 10));
            hex4(0xDC00 + (v & 0x3FF));
        } else {
            hex4(cp);
        }
        i += len;
    }
    return out;
}

std::string formatNumber(double v) {
    std::string s = formatDouble(v);
    if (s.find_first_of(".eEn") == std::string::npos)
        s += ".0";
    return s;
}

}  // namespace tbcad::dxf::detail
