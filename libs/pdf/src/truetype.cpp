#include "truetype.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace tbcad::pdf::detail {
namespace {

using Bytes = std::vector<unsigned char>;

struct Reader {
    const Bytes& b;
    bool ok = true;
    std::uint32_t u8(std::size_t at) {
        if (at + 1 > b.size()) {
            ok = false;
            return 0;
        }
        return b[at];
    }
    std::uint32_t u16(std::size_t at) {
        if (at + 2 > b.size()) {
            ok = false;
            return 0;
        }
        return (std::uint32_t{b[at]} << 8) | b[at + 1];
    }
    std::int32_t s16(std::size_t at) { return static_cast<std::int16_t>(u16(at)); }
    std::uint32_t u32(std::size_t at) {
        if (at + 4 > b.size()) {
            ok = false;
            return 0;
        }
        return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) |
               (std::uint32_t{b[at + 2]} << 8) | b[at + 3];
    }
};

struct Table {
    std::size_t offset = 0, length = 0;
};

// The table directory of one face; empty when the file is not an sfnt.
std::optional<std::map<std::string, Table>> tables(const Bytes& file, int faceIndex) {
    Reader r{file};
    std::size_t base = 0;
    if (file.size() >= 12 && std::memcmp(file.data(), "ttcf", 4) == 0) {
        const std::uint32_t count = r.u32(8);
        if (faceIndex < 0 || static_cast<std::uint32_t>(faceIndex) >= count)
            return std::nullopt;
        base = r.u32(12 + 4 * static_cast<std::size_t>(faceIndex));
    } else if (faceIndex != 0) {
        return std::nullopt;
    }
    const std::uint32_t version = r.u32(base);
    if (version != 0x00010000 && version != 0x74727565)  // 1.0 or 'true'
        return std::nullopt;
    const std::uint32_t n = r.u16(base + 4);
    std::map<std::string, Table> out;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t at = base + 12 + 16 * static_cast<std::size_t>(i);
        if (at + 16 > file.size())
            return std::nullopt;
        std::string tag(reinterpret_cast<const char*>(file.data() + at), 4);
        Table t{r.u32(at + 8), r.u32(at + 12)};
        if (t.offset > file.size() || t.length > file.size() - t.offset)
            return std::nullopt;
        out[tag] = t;
    }
    if (!r.ok)
        return std::nullopt;
    for (const char* need : {"head", "hhea", "maxp", "hmtx", "loca", "glyf"})
        if (!out.count(need))
            return std::nullopt;
    return out;
}

// Glyph data ranges [start, end) in the glyf table, one per glyph.
std::optional<std::vector<std::pair<std::uint32_t, std::uint32_t>>> glyphRanges(
    const Bytes& file, const std::map<std::string, Table>& t, int numGlyphs, bool longLoca) {
    Reader r{file};
    const Table& loca = t.at("loca");
    const Table& glyf = t.at("glyf");
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out(static_cast<std::size_t>(numGlyphs));
    auto at = [&](int i) -> std::uint32_t {
        return longLoca ? r.u32(loca.offset + 4 * static_cast<std::size_t>(i))
                        : 2 * r.u16(loca.offset + 2 * static_cast<std::size_t>(i));
    };
    const std::size_t entries = static_cast<std::size_t>(numGlyphs) + 1;
    if (loca.length < entries * (longLoca ? 4 : 2))
        return std::nullopt;
    std::uint32_t prev = at(0);
    for (int i = 0; i < numGlyphs; ++i) {
        const std::uint32_t next = at(i + 1);
        if (next < prev || next > glyf.length)
            return std::nullopt;
        out[static_cast<std::size_t>(i)] = {prev, next};
        prev = next;
    }
    if (!r.ok)
        return std::nullopt;
    return out;
}

void put16(std::string& s, std::uint32_t v) {
    s += static_cast<char>((v >> 8) & 0xFF);
    s += static_cast<char>(v & 0xFF);
}
void put32(std::string& s, std::uint32_t v) {
    put16(s, v >> 16);
    put16(s, v & 0xFFFF);
}
std::uint32_t checksum(const std::string& data) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < data.size(); i += 4) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k)
            v = (v << 8) | (i + k < data.size() ? static_cast<unsigned char>(data[i + k]) : 0u);
        sum += v;
    }
    return sum;
}

}  // namespace

std::optional<TrueTypeInfo> readTrueType(const Bytes& file, int faceIndex) {
    auto t = tables(file, faceIndex);
    if (!t)
        return std::nullopt;
    Reader r{file};
    TrueTypeInfo info;
    const std::size_t head = t->at("head").offset, hhea = t->at("hhea").offset;
    info.unitsPerEm = static_cast<int>(r.u16(head + 18));
    info.xMin = r.s16(head + 36);
    info.yMin = r.s16(head + 38);
    info.xMax = r.s16(head + 40);
    info.yMax = r.s16(head + 42);
    info.ascent = r.s16(hhea + 4);
    info.descent = r.s16(hhea + 6);
    const std::uint32_t numMetrics = r.u16(hhea + 34);
    info.numGlyphs = static_cast<int>(r.u16(t->at("maxp").offset + 4));
    info.capHeight = info.ascent;
    if (auto os2 = t->find("OS/2");
        os2 != t->end() && os2->second.length >= 90 && r.u16(os2->second.offset) >= 2)
        info.capHeight = r.s16(os2->second.offset + 88);
    if (auto post = t->find("post"); post != t->end() && post->second.length >= 16) {
        info.italicAngle = static_cast<std::int32_t>(r.u32(post->second.offset + 4)) / 65536.0;
        info.fixedPitch = r.u32(post->second.offset + 12) != 0;
    }
    if (info.unitsPerEm < 16 || info.numGlyphs <= 0 || numMetrics == 0 ||
        numMetrics > static_cast<std::uint32_t>(info.numGlyphs))
        return std::nullopt;
    const Table& hmtx = t->at("hmtx");
    if (hmtx.length < 4 * static_cast<std::size_t>(numMetrics))
        return std::nullopt;
    info.advances.resize(static_cast<std::size_t>(info.numGlyphs));
    std::uint16_t last = 0;
    for (int g = 0; g < info.numGlyphs; ++g) {
        if (static_cast<std::uint32_t>(g) < numMetrics)
            last = static_cast<std::uint16_t>(r.u16(hmtx.offset + 4 * static_cast<std::size_t>(g)));
        info.advances[static_cast<std::size_t>(g)] = last;
    }
    if (auto name = t->find("name"); name != t->end() && name->second.length >= 6) {
        // PostScript name (ID 6): Windows Unicode (UTF-16BE, ASCII only by spec) or Mac Roman.
        const std::size_t base = name->second.offset, end = base + name->second.length;
        const std::uint32_t count = r.u16(base + 2);
        const std::size_t strings = base + r.u16(base + 4);
        for (std::uint32_t i = 0; i < count && info.postscriptName.empty(); ++i) {
            const std::size_t rec = base + 6 + 12 * static_cast<std::size_t>(i);
            if (rec + 12 > end)
                break;
            const std::uint32_t platform = r.u16(rec), id = r.u16(rec + 6);
            const std::size_t len = r.u16(rec + 8), at = strings + r.u16(rec + 10);
            if (id != 6 || at + len > end || (platform != 3 && platform != 1))
                continue;
            const std::size_t step = platform == 3 ? 2 : 1;
            for (std::size_t k = step - 1; k < len; k += step) {
                const unsigned char c = file[at + k];
                if (c > 32 && c < 127 && c != '[' && c != ']' && c != '(' && c != ')' && c != '{' &&
                    c != '}' && c != '<' && c != '>' && c != '/' && c != '%')
                    info.postscriptName += static_cast<char>(c);
            }
        }
    }
    const bool longLoca = r.s16(head + 50) != 0;
    if (!r.ok || !glyphRanges(file, *t, info.numGlyphs, longLoca))
        return std::nullopt;
    return info;
}

std::string subsetTrueType(const Bytes& file, int faceIndex, const std::set<std::uint16_t>& keep) {
    auto t = tables(file, faceIndex);
    if (!t)
        return {};
    Reader r{file};
    const std::size_t head = t->at("head").offset;
    const bool longLoca = r.s16(head + 50) != 0;
    const int numGlyphs = static_cast<int>(r.u16(t->at("maxp").offset + 4));
    auto ranges = glyphRanges(file, *t, numGlyphs, longLoca);
    if (!ranges || !r.ok)
        return {};
    const std::size_t glyf = t->at("glyf").offset;

    // The kept glyphs and, through composite glyphs, the glyphs they are built from.
    std::vector<char> used(static_cast<std::size_t>(numGlyphs), 0);
    std::vector<std::uint16_t> todo(keep.begin(), keep.end());
    todo.push_back(0);  // .notdef
    while (!todo.empty()) {
        const std::uint16_t g = todo.back();
        todo.pop_back();
        if (g >= numGlyphs || used[g])
            continue;
        used[g] = 1;
        const auto [s, e] = (*ranges)[g];
        if (e - s < 10 || r.s16(glyf + s) >= 0)
            continue;
        std::size_t at = glyf + s + 10;
        for (int guard = 0; guard < 1024 && at + 4 <= glyf + e; ++guard) {
            const std::uint32_t flags = r.u16(at);
            todo.push_back(static_cast<std::uint16_t>(r.u16(at + 2)));
            at += 4 + ((flags & 0x0001) ? 4 : 2);
            if (flags & 0x0008)
                at += 2;
            else if (flags & 0x0040)
                at += 4;
            else if (flags & 0x0080)
                at += 8;
            if (!(flags & 0x0020))
                break;
        }
    }

    std::string newGlyf, newLoca;
    for (int g = 0; g < numGlyphs; ++g) {
        put32(newLoca, static_cast<std::uint32_t>(newGlyf.size()));
        if (!used[static_cast<std::size_t>(g)])
            continue;
        const auto [s, e] = (*ranges)[static_cast<std::size_t>(g)];
        newGlyf.append(reinterpret_cast<const char*>(file.data() + glyf + s), e - s);
        while (newGlyf.size() % 4)
            newGlyf += '\0';
    }
    put32(newLoca, static_cast<std::uint32_t>(newGlyf.size()));

    auto copy = [&](const std::string& tag) {
        const Table& tb = t->at(tag);
        return std::string(reinterpret_cast<const char*>(file.data() + tb.offset), tb.length);
    };
    std::map<std::string, std::string> out;
    for (const char* tag : {"hhea", "maxp", "hmtx", "cvt ", "fpgm", "prep"})
        if (t->count(tag))
            out[tag] = copy(tag);
    std::string newHead = copy("head");
    if (newHead.size() < 54)
        return {};
    std::memset(newHead.data() + 8, 0, 4);  // checkSumAdjustment, set below
    newHead[50] = 0;
    newHead[51] = 1;  // long loca offsets
    out["head"] = newHead;
    out["loca"] = newLoca;
    out["glyf"] = newGlyf;

    // The font file: offset table, directory (tags in order), then the 4-byte aligned tables.
    const std::uint32_t n = static_cast<std::uint32_t>(out.size());
    std::uint32_t pow2 = 1, log2 = 0;
    while (pow2 * 2 <= n) {
        pow2 *= 2;
        ++log2;
    }
    std::string font;
    put32(font, 0x00010000);
    put16(font, n);
    put16(font, pow2 * 16);
    put16(font, log2);
    put16(font, n * 16 - pow2 * 16);
    std::size_t offset = 12 + 16 * static_cast<std::size_t>(n);
    std::string body;
    std::size_t headAt = 0;
    for (const auto& [tag, data] : out) {
        font += tag;
        put32(font, checksum(data));
        put32(font, static_cast<std::uint32_t>(offset + body.size()));
        put32(font, static_cast<std::uint32_t>(data.size()));
        if (tag == "head")
            headAt = offset + body.size();
        body += data;
        while (body.size() % 4)
            body += '\0';
    }
    font += body;
    const std::uint32_t adjust = 0xB1B0AFBAu - checksum(font);
    for (int k = 0; k < 4; ++k)
        font[headAt + 8 + static_cast<std::size_t>(k)] =
            static_cast<char>((adjust >> (24 - 8 * k)) & 0xFF);
    return font;
}

}  // namespace tbcad::pdf::detail
