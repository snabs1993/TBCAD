#include "tiff.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "miniz.h"

namespace tbcad::underlay {

namespace {

constexpr long long kMaxPixels = 100'000'000;

Error bad(const std::string& why) {
    return Error{ErrorCode::Parse, "Cannot read the TIFF picture: " + why};
}

struct Bytes {
    const std::uint8_t* p;
    std::size_t n;
    bool big;
    bool has(std::size_t at, std::size_t len) const { return at <= n && len <= n - at; }
    std::uint16_t u16(std::size_t at) const {
        return big ? static_cast<std::uint16_t>((p[at] << 8) | p[at + 1])
                   : static_cast<std::uint16_t>(p[at] | (p[at + 1] << 8));
    }
    std::uint32_t u32(std::size_t at) const {
        return big ? (std::uint32_t(p[at]) << 24) | (std::uint32_t(p[at + 1]) << 16) |
                         (std::uint32_t(p[at + 2]) << 8) | p[at + 3]
                   : (std::uint32_t(p[at + 3]) << 24) | (std::uint32_t(p[at + 2]) << 16) |
                         (std::uint32_t(p[at + 1]) << 8) | p[at];
    }
};

struct Tag {
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::size_t at = 0;  // where the values start
};

// ---- compression --------------------------------------------------------------------------------

bool packBits(const std::uint8_t* s, std::size_t n, std::vector<std::uint8_t>& out,
              std::size_t want) {
    std::size_t i = 0;
    while (i < n && out.size() < want) {
        const int c = static_cast<std::int8_t>(s[i++]);
        if (c >= 0) {
            const std::size_t len = static_cast<std::size_t>(c) + 1;
            if (i + len > n)
                return false;
            out.insert(out.end(), s + i, s + i + len);
            i += len;
        } else if (c != -128) {
            if (i >= n)
                return false;
            out.insert(out.end(), static_cast<std::size_t>(1 - c), s[i++]);
        }
    }
    return true;
}

bool lzw(const std::uint8_t* s, std::size_t n, std::vector<std::uint8_t>& out, std::size_t want) {
    struct Entry {
        std::int32_t prev;
        std::uint8_t ch;
        std::uint16_t len;
    };
    std::vector<Entry> table(4096);
    for (int i = 0; i < 256; ++i)
        table[static_cast<std::size_t>(i)] = {-1, static_cast<std::uint8_t>(i), 1};
    int next = 258, width = 9;
    std::uint32_t acc = 0;
    int bits = 0;
    std::size_t pos = 0;
    int prev = -1;
    std::vector<std::uint8_t> tmp;
    auto emit = [&](int code) {
        const std::size_t len = table[static_cast<std::size_t>(code)].len;
        tmp.resize(len);
        int c = code;
        for (std::size_t k = len; k > 0; --k) {
            tmp[k - 1] = table[static_cast<std::size_t>(c)].ch;
            c = table[static_cast<std::size_t>(c)].prev;
        }
        out.insert(out.end(), tmp.begin(), tmp.end());
        return tmp[0];
    };
    while (out.size() < want) {
        while (bits < width && pos < n) {
            acc = (acc << 8) | s[pos++];
            bits += 8;
        }
        if (bits < width)
            break;
        const int code = static_cast<int>((acc >> (bits - width)) & ((1u << width) - 1));
        bits -= width;
        if (code == 257)
            break;
        if (code == 256) {
            next = 258;
            width = 9;
            prev = -1;
            continue;
        }
        if (prev < 0) {
            if (code > 255)
                return false;
            emit(code);
            prev = code;
            continue;
        }
        std::uint8_t first;
        if (code < next) {
            first = emit(code);
        } else if (code == next) {
            // The string of prev plus its own first byte.
            int c = prev;
            while (table[static_cast<std::size_t>(c)].prev >= 0)
                c = table[static_cast<std::size_t>(c)].prev;
            first = table[static_cast<std::size_t>(c)].ch;
            if (next >= 4096)
                return false;
            table[static_cast<std::size_t>(next)] = {
                prev, first,
                static_cast<std::uint16_t>(table[static_cast<std::size_t>(prev)].len + 1)};
            emit(next);
            ++next;
            prev = code;
            if (next + 1 >= (1 << width) && width < 12)
                ++width;
            continue;
        } else {
            return false;
        }
        if (next < 4096) {
            table[static_cast<std::size_t>(next)] = {
                prev, first,
                static_cast<std::uint16_t>(table[static_cast<std::size_t>(prev)].len + 1)};
            ++next;
        }
        prev = code;
        if (next + 1 >= (1 << width) && width < 12)
            ++width;
    }
    return true;
}

// ---- CCITT Group 4 ------------------------------------------------------------------------------

struct RunCode {
    const char* bits;
    int run;
};

constexpr RunCode kWhite[] = {
    {"00110101", 0},     {"000111", 1},       {"0111", 2},         {"1000", 3},
    {"1011", 4},         {"1100", 5},         {"1110", 6},         {"1111", 7},
    {"10011", 8},        {"10100", 9},        {"00111", 10},       {"01000", 11},
    {"001000", 12},      {"000011", 13},      {"110100", 14},      {"110101", 15},
    {"101010", 16},      {"101011", 17},      {"0100111", 18},     {"0001100", 19},
    {"0001000", 20},     {"0010111", 21},     {"0000011", 22},     {"0000100", 23},
    {"0101000", 24},     {"0101011", 25},     {"0010011", 26},     {"0100100", 27},
    {"0011000", 28},     {"00000010", 29},    {"00000011", 30},    {"00011010", 31},
    {"00011011", 32},    {"00010010", 33},    {"00010011", 34},    {"00010100", 35},
    {"00010101", 36},    {"00010110", 37},    {"00010111", 38},    {"00101000", 39},
    {"00101001", 40},    {"00101010", 41},    {"00101011", 42},    {"00101100", 43},
    {"00101101", 44},    {"00000100", 45},    {"00000101", 46},    {"00001010", 47},
    {"00001011", 48},    {"01010010", 49},    {"01010011", 50},    {"01010100", 51},
    {"01010101", 52},    {"00100100", 53},    {"00100101", 54},    {"01011000", 55},
    {"01011001", 56},    {"01011010", 57},    {"01011011", 58},    {"01001010", 59},
    {"01001011", 60},    {"00110010", 61},    {"00110011", 62},    {"00110100", 63},
    {"11011", 64},       {"10010", 128},      {"010111", 192},     {"0110111", 256},
    {"00110110", 320},   {"00110111", 384},   {"01100100", 448},   {"01100101", 512},
    {"01101000", 576},   {"01100111", 640},   {"011001100", 704},  {"011001101", 768},
    {"011010010", 832},  {"011010011", 896},  {"011010100", 960},  {"011010101", 1024},
    {"011010110", 1088}, {"011010111", 1152}, {"011011000", 1216}, {"011011001", 1280},
    {"011011010", 1344}, {"011011011", 1408}, {"010011000", 1472}, {"010011001", 1536},
    {"010011010", 1600}, {"011000", 1664},    {"010011011", 1728}};

constexpr RunCode kBlack[] = {{"0000110111", 0},
                              {"010", 1},
                              {"11", 2},
                              {"10", 3},
                              {"011", 4},
                              {"0011", 5},
                              {"0010", 6},
                              {"00011", 7},
                              {"000101", 8},
                              {"000100", 9},
                              {"0000100", 10},
                              {"0000101", 11},
                              {"0000111", 12},
                              {"00000100", 13},
                              {"00000111", 14},
                              {"000011000", 15},
                              {"0000010111", 16},
                              {"0000011000", 17},
                              {"0000001000", 18},
                              {"00001100111", 19},
                              {"00001101000", 20},
                              {"00001101100", 21},
                              {"00000110111", 22},
                              {"00000101000", 23},
                              {"00000010111", 24},
                              {"00000011000", 25},
                              {"000011001010", 26},
                              {"000011001011", 27},
                              {"000011001100", 28},
                              {"000011001101", 29},
                              {"000001101000", 30},
                              {"000001101001", 31},
                              {"000001101010", 32},
                              {"000001101011", 33},
                              {"000011010010", 34},
                              {"000011010011", 35},
                              {"000011010100", 36},
                              {"000011010101", 37},
                              {"000011010110", 38},
                              {"000011010111", 39},
                              {"000001101100", 40},
                              {"000001101101", 41},
                              {"000011011010", 42},
                              {"000011011011", 43},
                              {"000001010100", 44},
                              {"000001010101", 45},
                              {"000001010110", 46},
                              {"000001010111", 47},
                              {"000001100100", 48},
                              {"000001100101", 49},
                              {"000001010010", 50},
                              {"000001010011", 51},
                              {"000000100100", 52},
                              {"000000110111", 53},
                              {"000000111000", 54},
                              {"000000100111", 55},
                              {"000000101000", 56},
                              {"000001011000", 57},
                              {"000001011001", 58},
                              {"000000101011", 59},
                              {"000000101100", 60},
                              {"000001011010", 61},
                              {"000001100110", 62},
                              {"000001100111", 63},
                              {"0000001111", 64},
                              {"000011001000", 128},
                              {"000011001001", 192},
                              {"000001011011", 256},
                              {"000000110011", 320},
                              {"000000110100", 384},
                              {"000000110101", 448},
                              {"0000001101100", 512},
                              {"0000001101101", 576},
                              {"0000001001010", 640},
                              {"0000001001011", 704},
                              {"0000001001100", 768},
                              {"0000001001101", 832},
                              {"0000001110010", 896},
                              {"0000001110011", 960},
                              {"0000001110100", 1024},
                              {"0000001110101", 1088},
                              {"0000001110110", 1152},
                              {"0000001110111", 1216},
                              {"0000001010010", 1280},
                              {"0000001010011", 1344},
                              {"0000001010100", 1408},
                              {"0000001010101", 1472},
                              {"0000001011010", 1536},
                              {"0000001011011", 1600},
                              {"0000001100100", 1664},
                              {"0000001100101", 1728}};

constexpr RunCode kExtended[] = {
    {"00000001000", 1792},  {"00000001100", 1856},  {"00000001101", 1920},  {"000000010010", 1984},
    {"000000010011", 2048}, {"000000010100", 2112}, {"000000010101", 2176}, {"000000010110", 2240},
    {"000000010111", 2304}, {"000000011100", 2368}, {"000000011101", 2432}, {"000000011110", 2496},
    {"000000011111", 2560}};

// code length (1..13) and bits -> run, for one colour.
using RunMap = std::unordered_map<std::uint32_t, int>;
std::uint32_t key(int len, std::uint32_t bits) {
    return (std::uint32_t(len) << 16) | bits;
}

template <std::size_t N>
void addCodes(RunMap& m, const RunCode (&codes)[N]) {
    for (const RunCode& c : codes) {
        std::uint32_t v = 0;
        int len = 0;
        for (const char* s = c.bits; *s; ++s, ++len)
            v = (v << 1) | (*s == '1');
        m[key(len, v)] = c.run;
    }
}

const RunMap& runMap(bool black) {
    static const RunMap white = [] {
        RunMap m;
        addCodes(m, kWhite);
        addCodes(m, kExtended);
        return m;
    }();
    static const RunMap blk = [] {
        RunMap m;
        addCodes(m, kBlack);
        addCodes(m, kExtended);
        return m;
    }();
    return black ? blk : white;
}

struct BitReader {
    const std::uint8_t* p;
    std::size_t n;
    std::size_t pos = 0;  // in bits
    bool reverse = false;
    bool atEnd() const { return pos >= n * 8; }
    int bit() {
        if (atEnd())
            return -1;
        std::uint8_t b = p[pos >> 3];
        const int sh = reverse ? static_cast<int>(pos & 7) : 7 - static_cast<int>(pos & 7);
        ++pos;
        return (b >> sh) & 1;
    }
    int peek(int count) {  // zero-padded past the end
        int v = 0;
        const std::size_t save = pos;
        for (int i = 0; i < count; ++i) {
            const int b = bit();
            v = (v << 1) | (b < 0 ? 0 : b);
        }
        pos = save;
        return v;
    }
};

// One run length of a colour (make-up codes added to a terminating code); -1 on error.
int readRun(BitReader& br, bool black) {
    const RunMap& m = runMap(black);
    int total = 0;
    for (;;) {
        std::uint32_t v = 0;
        int found = -1;
        for (int len = 1; len <= 13; ++len) {
            const int b = br.bit();
            if (b < 0)
                return -1;
            v = (v << 1) | static_cast<std::uint32_t>(b);
            auto it = m.find(key(len, v));
            if (it != m.end()) {
                found = it->second;
                break;
            }
        }
        if (found < 0)
            return -1;
        total += found;
        if (found < 64)
            return total;
    }
}

// Decodes `rows` Group 4 rows of `width` pixels into bytes (255 white, 0 black), one per pixel.
bool group4(const std::uint8_t* s, std::size_t n, bool reverse, int width, int rows,
            std::vector<std::uint8_t>& out) {
    BitReader br{s, n, 0, reverse};
    std::vector<int> ref{width, width}, cur;
    for (int row = 0; row < rows; ++row) {
        cur.clear();
        int a0 = -1;
        bool black = false;
        while (a0 < width) {
            // b1: first changing element of the reference line right of a0 and of the opposite
            // colour (even index = a change to black); b2 follows it.
            std::size_t i = black ? 1 : 0;
            while (i < ref.size() && ref[i] <= a0 && ref[i] < width)
                i += 2;
            const int b1 = i < ref.size() ? ref[i] : width;
            const int b2 = i + 1 < ref.size() ? ref[i + 1] : width;
            // Mode code.
            int a1 = -1;
            if (br.peek(1) == 1) {
                br.bit();
                a1 = b1;
            } else if (br.peek(3) == 3) {
                br.pos += 3;
                a1 = b1 + 1;
            } else if (br.peek(3) == 2) {
                br.pos += 3;
                a1 = b1 - 1;
            } else if (br.peek(3) == 1) {  // horizontal
                br.pos += 3;
                const int r1 = readRun(br, black);
                const int r2 = readRun(br, !black);
                if (r1 < 0 || r2 < 0)
                    return false;
                const int start = std::max(a0, 0);
                const int x1 = std::min(start + r1, width), x2 = std::min(x1 + r2, width);
                cur.push_back(x1);
                cur.push_back(x2);
                a0 = x2;
                if (a0 >= width)
                    break;
                continue;
            } else if (br.peek(4) == 1) {  // pass
                br.pos += 4;
                a0 = b2;
                continue;
            } else if (br.peek(6) == 3) {
                br.pos += 6;
                a1 = b1 + 2;
            } else if (br.peek(6) == 2) {
                br.pos += 6;
                a1 = b1 - 2;
            } else if (br.peek(7) == 3) {
                br.pos += 7;
                a1 = b1 + 3;
            } else if (br.peek(7) == 2) {
                br.pos += 7;
                a1 = b1 - 3;
            } else {
                return false;  // end of block, extension or damage
            }
            a1 = std::clamp(a1, std::max(a0, 0), width);
            cur.push_back(a1);
            a0 = a1;
            black = !black;
            if (a0 >= width)
                break;
        }
        // Row pixels.
        const std::size_t base = out.size();
        out.resize(base + static_cast<std::size_t>(width), 255);
        bool paint = false;
        int x = 0;
        for (int c : cur) {
            c = std::clamp(c, x, width);
            if (paint)
                std::fill(out.begin() + static_cast<std::ptrdiff_t>(base) + x,
                          out.begin() + static_cast<std::ptrdiff_t>(base) + c, std::uint8_t{0});
            x = c;
            paint = !paint;
        }
        if (paint)
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(base) + x, out.end(),
                      std::uint8_t{0});
        cur.push_back(width);
        cur.push_back(width);
        ref = cur;
    }
    return true;
}

}  // namespace

bool looksLikeTiff(const std::uint8_t* d, std::size_t n) {
    return n >= 8 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 42 && d[3] == 0) ||
                      (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 42));
}

Result<std::shared_ptr<const ImageData>> decodeTiff(const std::uint8_t* data, std::size_t size) {
    if (!looksLikeTiff(data, size))
        return bad("not a TIFF file");
    Bytes b{data, size, data[0] == 'M'};
    const std::size_t ifd = b.u32(4);
    if (!b.has(ifd, 2))
        return bad("damaged directory");
    const std::size_t count = b.u16(ifd);
    if (!b.has(ifd + 2, count * 12))
        return bad("damaged directory");
    std::map<int, Tag> tags;
    static const std::size_t typeSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t e = ifd + 2 + i * 12;
        Tag t;
        t.type = b.u16(e + 2);
        t.count = b.u32(e + 4);
        const std::size_t sz = t.type < 13 ? typeSize[t.type] : 0;
        const std::size_t total = sz * t.count;
        t.at = total <= 4 ? e + 8 : b.u32(e + 8);
        if (sz == 0 || !b.has(t.at, total))
            continue;
        tags[b.u16(e)] = t;
    }
    auto value = [&](int tag, std::size_t k, long long def) -> long long {
        auto it = tags.find(tag);
        if (it == tags.end() || k >= it->second.count)
            return def;
        const Tag& t = it->second;
        switch (t.type) {
            case 3:
                return b.u16(t.at + k * 2);
            case 4:
                return b.u32(t.at + k * 4);
            case 1:
                return data[t.at + k];
            default:
                return def;
        }
    };
    const long long w = value(256, 0, 0), h = value(257, 0, 0);
    if (w <= 0 || h <= 0)
        return bad("no size");
    if (w * h > kMaxPixels)
        return bad("the picture is too large");
    const int bps = static_cast<int>(value(258, 0, 1));
    const int comp = static_cast<int>(value(259, 0, 1));
    const int photo = static_cast<int>(value(262, 0, 1));
    const int spp = static_cast<int>(value(277, 0, 1));
    const int planar = static_cast<int>(value(284, 0, 1));
    const int predictor = static_cast<int>(value(317, 0, 1));
    const bool reverseFill = value(266, 0, 1) == 2;
    if (tags.count(322) || tags.count(324))
        return bad("tiled pictures are not supported");
    if (planar != 1 && spp > 1)
        return bad("planar pictures are not supported");
    if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 16)
        return bad("unsupported bits per sample");
    if (spp < 1 || spp > 5)
        return bad("unsupported samples per pixel");
    if (!tags.count(273) || !tags.count(279))
        return bad("no strips");
    const long long rps = std::min<long long>(value(278, 0, h), h);
    if (rps <= 0)
        return bad("bad rows per strip");
    const std::size_t rowBytes = (static_cast<std::size_t>(w) * spp * bps + 7) / 8;
    const std::size_t strips = tags[273].count;

    std::vector<std::uint8_t> raw;  // the whole image, rows of rowBytes
    raw.reserve(rowBytes * static_cast<std::size_t>(h));
    std::vector<std::uint8_t> g4rows;  // Group 4: one byte per pixel
    for (std::size_t s = 0; s < strips && static_cast<long long>(s) * rps < h; ++s) {
        const std::size_t off = static_cast<std::size_t>(value(273, s, 0));
        const std::size_t len = static_cast<std::size_t>(value(279, s, 0));
        if (!b.has(off, len))
            return bad("a strip lies outside the file");
        const long long rows = std::min<long long>(rps, h - static_cast<long long>(s) * rps);
        const std::size_t want = rowBytes * static_cast<std::size_t>(rows);
        std::vector<std::uint8_t> strip;
        if (comp == 1) {
            strip.assign(data + off, data + off + std::min(len, want));
        } else if (comp == 32773) {
            if (!packBits(data + off, len, strip, want))
                return bad("damaged PackBits data");
        } else if (comp == 5) {
            if (!lzw(data + off, len, strip, want))
                return bad("damaged LZW data");
        } else if (comp == 8 || comp == 32946) {
            strip.resize(want);
            mz_ulong outLen = static_cast<mz_ulong>(want);
            const int rc =
                mz_uncompress(strip.data(), &outLen, data + off, static_cast<mz_ulong>(len));
            if (rc != MZ_OK && rc != MZ_BUF_ERROR)
                return bad("damaged Deflate data");
            strip.resize(outLen);
        } else if (comp == 4) {
            if (bps != 1 || spp != 1)
                return bad("Group 4 needs a 1-bit picture");
            std::vector<std::uint8_t> px;
            if (!group4(data + off, len, reverseFill, static_cast<int>(w), static_cast<int>(rows),
                        px))
                return bad("damaged Group 4 data");
            g4rows.insert(g4rows.end(), px.begin(), px.end());
            continue;
        } else {
            return bad("unsupported compression " + std::to_string(comp));
        }
        strip.resize(want, 0);
        if (predictor == 2 && bps == 8)
            for (long long r = 0; r < rows; ++r) {
                std::uint8_t* row = strip.data() + static_cast<std::size_t>(r) * rowBytes;
                for (std::size_t i = static_cast<std::size_t>(spp); i < rowBytes; ++i)
                    row[i] =
                        static_cast<std::uint8_t>(row[i] + row[i - static_cast<std::size_t>(spp)]);
            }
        raw.insert(raw.end(), strip.begin(), strip.end());
    }

    auto img = std::make_shared<ImageData>();
    img->width = static_cast<int>(w);
    img->height = static_cast<int>(h);
    img->rgba.assign(static_cast<std::size_t>(w * h) * 4, 255);
    auto put = [&](std::size_t px, int r, int g, int bl, int a = 255) {
        std::uint8_t* o = &img->rgba[px * 4];
        o[0] = static_cast<std::uint8_t>(r);
        o[1] = static_cast<std::uint8_t>(g);
        o[2] = static_cast<std::uint8_t>(bl);
        o[3] = static_cast<std::uint8_t>(a);
    };
    if (comp == 4) {
        if (g4rows.size() < static_cast<std::size_t>(w * h))
            g4rows.resize(static_cast<std::size_t>(w * h), 255);  // damaged tail stays white
        // A white run codes 0 bits; whether those are white pixels is up to the photometric
        // interpretation (as libtiff reads it).
        const bool invert = photo == 1;
        for (std::size_t i = 0; i < static_cast<std::size_t>(w * h); ++i) {
            const int g = invert ? 255 - g4rows[i] : g4rows[i];
            put(i, g, g, g);
        }
        return std::shared_ptr<const ImageData>(std::move(img));
    }
    raw.resize(rowBytes * static_cast<std::size_t>(h), 0);
    // Sample k of pixel x on row y, scaled to 0..255 (palette index when `index`).
    auto sample = [&](long long y, long long x, int k, bool index) -> int {
        const std::uint8_t* row = raw.data() + static_cast<std::size_t>(y) * rowBytes;
        const std::size_t n = static_cast<std::size_t>(x) * spp + k;
        int v;
        if (bps == 8) {
            v = row[n];
        } else if (bps == 16) {
            v = b.big ? row[n * 2] : row[n * 2 + 1];  // the high byte
        } else {
            const std::size_t bit = n * bps;
            v = (row[bit >> 3] >> (8 - bps - (bit & 7))) & ((1 << bps) - 1);
            if (!index)
                v = v * 255 / ((1 << bps) - 1);
        }
        return v;
    };
    std::vector<int> palette;
    if (photo == 3) {
        auto it = tags.find(320);
        const std::size_t entries = std::size_t(1) << std::min(bps, 8);
        if (it == tags.end() || it->second.count < entries * 3)
            return bad("no colour map");
        palette.resize(entries * 3);
        for (std::size_t i = 0; i < entries * 3; ++i)
            palette[i] = b.u16(it->second.at + i * 2) >> 8;
    }
    for (long long y = 0; y < h; ++y)
        for (long long x = 0; x < w; ++x) {
            const std::size_t px = static_cast<std::size_t>(y * w + x);
            if (photo == 3) {
                const std::size_t i = static_cast<std::size_t>(sample(y, x, 0, true));
                if (i * 3 + 2 < palette.size())
                    put(px, palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2]);
            } else if (photo == 2 && spp >= 3) {
                put(px, sample(y, x, 0, false), sample(y, x, 1, false), sample(y, x, 2, false),
                    spp >= 4 ? sample(y, x, 3, false) : 255);
            } else if (photo == 5 && spp >= 4) {
                const int k = 255 - sample(y, x, 3, false);
                put(px, (255 - sample(y, x, 0, false)) * k / 255,
                    (255 - sample(y, x, 1, false)) * k / 255,
                    (255 - sample(y, x, 2, false)) * k / 255);
            } else if (photo == 0 || photo == 1) {
                int g = sample(y, x, 0, false);
                if (photo == 0)
                    g = 255 - g;
                put(px, g, g, g, spp >= 2 ? sample(y, x, 1, false) : 255);
            } else {
                return bad("unsupported colour model");
            }
        }
    return std::shared_ptr<const ImageData>(std::move(img));
}

}  // namespace tbcad::underlay
