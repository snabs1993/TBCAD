#include "colors.h"

#include <array>
#include <cmath>

#include "tbcad/core/units.h"

namespace tbcad::dxf::detail {

namespace {

double dist2(Rgba a, Rgba b) {
    double dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

// Lowest ACI whose colour is nearest to `c`.
int nearestAci(Rgba c) {
    double best = 1e300;
    int bestAci = 7;
    for (int i = 1; i <= 255; ++i) {
        double d = dist2(paletteColor(static_cast<std::uint8_t>(i)), c);
        if (d < best) {
            best = d;
            bestAci = i;
        }
    }
    return bestAci;
}

std::uint32_t pack(Rgba c) {
    return (static_cast<std::uint32_t>(c.r) << 16) | (static_cast<std::uint32_t>(c.g) << 8) | c.b;
}

}  // namespace

AciChoice aciForIndex(int index) {
    if (index < 1 || index > 255)
        return {7, std::nullopt};
    return {index, std::nullopt};  // the palette is the ACI table
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
