// PNG and JPEG encoders for rasterised plot sheets. PNG uses miniz for the zlib stream; JPEG is a
// small baseline encoder (YCbCr 4:4:4, the standard Huffman tables of ITU T.81 Annex K).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "miniz.h"
#include "tbcad/export/plot.h"

namespace tbcad::exportlib {

namespace {

void put32(std::string& s, std::uint32_t v) {
    for (int i = 3; i >= 0; --i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void pngChunk(std::string& out, const char type[5], const std::string& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::string body(type, 4);
    body += data;
    out += body;
    const mz_ulong crc =
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(body.data()), body.size());
    put32(out, static_cast<std::uint32_t>(crc));
}

// --- JPEG ----------------------------------------------------------------------------------

constexpr std::array<int, 64> kZigzag = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

constexpr std::array<int, 64> kLumQ = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<int, 64> kChrQ = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

constexpr std::array<std::uint8_t, 16> kDcLumBits = {0, 1, 5, 1, 1, 1, 1, 1,
                                                     1, 0, 0, 0, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 16> kDcChrBits = {0, 3, 1, 1, 1, 1, 1, 1,
                                                     1, 1, 1, 0, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 12> kDcVals = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr std::array<std::uint8_t, 16> kAcLumBits = {0, 2, 1, 3, 3, 2, 4, 3,
                                                     5, 5, 4, 4, 0, 0, 1, 125};
constexpr std::array<std::uint8_t, 16> kAcChrBits = {0, 2, 1, 2, 4, 4, 3, 4,
                                                     7, 5, 4, 4, 0, 1, 2, 119};
constexpr std::array<std::uint8_t, 162> kAcLumVals = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
    0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
    0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
constexpr std::array<std::uint8_t, 162> kAcChrVals = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
    0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
    0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

struct HuffCode {
    std::array<std::uint16_t, 256> code{};
    std::array<std::uint8_t, 256> len{};
};

HuffCode makeCodes(const std::uint8_t* bits, const std::uint8_t* vals) {
    HuffCode h;
    unsigned code = 0;
    int k = 0;
    for (int l = 1; l <= 16; ++l) {
        for (int i = 0; i < bits[l - 1]; ++i, ++k) {
            h.code[vals[k]] = static_cast<std::uint16_t>(code++);
            h.len[vals[k]] = static_cast<std::uint8_t>(l);
        }
        code <<= 1;
    }
    return h;
}

struct BitWriter {
    std::string& out;
    std::uint32_t acc = 0;
    int n = 0;
    void put(unsigned code, int len) {
        acc = (acc << len) | (code & ((1u << len) - 1));
        n += len;
        while (n >= 8) {
            const auto b = static_cast<std::uint8_t>((acc >> (n - 8)) & 0xFF);
            out.push_back(static_cast<char>(b));
            if (b == 0xFF)
                out.push_back('\0');  // byte stuffing
            n -= 8;
        }
        acc &= (1u << n) - 1;
    }
    void flush() {
        if (n > 0)
            put(0x7F, 8 - n);  // pad with 1 bits
    }
};

void seg16(std::string& s, unsigned v) {
    s.push_back(static_cast<char>(v >> 8));
    s.push_back(static_cast<char>(v & 0xFF));
}

std::array<int, 64> scaledQ(const std::array<int, 64>& base, int quality) {
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    std::array<int, 64> q{};
    for (int i = 0; i < 64; ++i)
        q[static_cast<std::size_t>(i)] =
            std::clamp((base[static_cast<std::size_t>(i)] * scale + 50) / 100, 1, 255);
    return q;
}

// Number of bits needed for |v|.
int bitLen(int v) {
    v = std::abs(v);
    int n = 0;
    while (v) {
        ++n;
        v >>= 1;
    }
    return n;
}

void dct8x8(const float in[64], float out[64]) {
    static const auto cosTable = [] {
        std::array<float, 64> t{};
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                t[static_cast<std::size_t>(u * 8 + x)] =
                    static_cast<float>((u == 0 ? std::sqrt(0.125) : 0.5) *
                                       std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16));
        return t;
    }();
    float tmp[64];
    for (int y = 0; y < 8; ++y)  // rows
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int x = 0; x < 8; ++x)
                s += in[y * 8 + x] * cosTable[static_cast<std::size_t>(u * 8 + x)];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; ++u)  // columns
        for (int v = 0; v < 8; ++v) {
            float s = 0;
            for (int y = 0; y < 8; ++y)
                s += tmp[y * 8 + u] * cosTable[static_cast<std::size_t>(v * 8 + y)];
            out[v * 8 + u] = s;
        }
}

}  // namespace

std::string pngBytes(const RasterImage& img) {
    const auto w = static_cast<std::size_t>(img.width);
    const auto h = static_cast<std::size_t>(img.height);
    // Filter type 2 (Up): rows of mostly white paper compress to almost nothing.
    std::string raw;
    raw.resize(h * (w * 3 + 1));
    for (std::size_t y = 0; y < h; ++y) {
        char* dst = raw.data() + y * (w * 3 + 1);
        const std::uint8_t* row = img.rgb.data() + y * w * 3;
        dst[0] = 2;
        for (std::size_t i = 0; i < w * 3; ++i) {
            const std::uint8_t above = y ? img.rgb[(y - 1) * w * 3 + i] : 0;
            dst[1 + i] = static_cast<char>(static_cast<std::uint8_t>(row[i] - above));
        }
    }
    mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::string z(cap, '\0');
    mz_compress2(reinterpret_cast<unsigned char*>(z.data()), &cap,
                 reinterpret_cast<const unsigned char*>(raw.data()),
                 static_cast<mz_ulong>(raw.size()), 6);
    z.resize(cap);

    std::string out("\x89PNG\r\n\x1a\n", 8);
    std::string ihdr;
    put32(ihdr, static_cast<std::uint32_t>(img.width));
    put32(ihdr, static_cast<std::uint32_t>(img.height));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // truecolour
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    pngChunk(out, "IHDR", ihdr);
    std::string phys;
    const auto ppm = static_cast<std::uint32_t>(std::llround(img.dpi / 0.0254));
    put32(phys, ppm);
    put32(phys, ppm);
    phys.push_back(1);  // metres
    pngChunk(out, "pHYs", phys);
    pngChunk(out, "IDAT", z);
    pngChunk(out, "IEND", "");
    return out;
}

std::string jpegBytes(const RasterImage& img, int quality) {
    const int w = img.width, h = img.height;
    const auto ql = scaledQ(kLumQ, quality);
    const auto qc = scaledQ(kChrQ, quality);
    const HuffCode dcL = makeCodes(kDcLumBits.data(), kDcVals.data());
    const HuffCode dcC = makeCodes(kDcChrBits.data(), kDcVals.data());
    const HuffCode acL = makeCodes(kAcLumBits.data(), kAcLumVals.data());
    const HuffCode acC = makeCodes(kAcChrBits.data(), kAcChrVals.data());

    std::string out;
    out += "\xFF\xD8";
    // JFIF APP0 with the density.
    out += "\xFF\xE0";
    seg16(out, 16);
    out.append("JFIF\0\1\1\1", 8);
    seg16(out, static_cast<unsigned>(std::lround(img.dpi)));
    seg16(out, static_cast<unsigned>(std::lround(img.dpi)));
    out.push_back(0);
    out.push_back(0);
    for (int t = 0; t < 2; ++t) {
        out += "\xFF\xDB";
        seg16(out, 67);
        out.push_back(static_cast<char>(t));
        const auto& q = t == 0 ? ql : qc;
        for (int i = 0; i < 64; ++i)
            out.push_back(static_cast<char>(
                q[static_cast<std::size_t>(kZigzag[static_cast<std::size_t>(i)])]));
    }
    out += "\xFF\xC0";
    seg16(out, 17);
    out.push_back(8);
    seg16(out, static_cast<unsigned>(h));
    seg16(out, static_cast<unsigned>(w));
    out.push_back(3);
    for (int c = 0; c < 3; ++c) {
        out.push_back(static_cast<char>(c + 1));
        out.push_back(static_cast<char>(0x11));  // no subsampling
        out.push_back(static_cast<char>(c == 0 ? 0 : 1));
    }
    auto dht = [&](int tcth, const std::uint8_t* bits, const std::uint8_t* vals, int n) {
        out += "\xFF\xC4";
        seg16(out, static_cast<unsigned>(19 + n));
        out.push_back(static_cast<char>(tcth));
        for (int i = 0; i < 16; ++i)
            out.push_back(static_cast<char>(bits[i]));
        for (int i = 0; i < n; ++i)
            out.push_back(static_cast<char>(vals[i]));
    };
    dht(0x00, kDcLumBits.data(), kDcVals.data(), 12);
    dht(0x10, kAcLumBits.data(), kAcLumVals.data(), 162);
    dht(0x01, kDcChrBits.data(), kDcVals.data(), 12);
    dht(0x11, kAcChrBits.data(), kAcChrVals.data(), 162);
    out += "\xFF\xDA";
    seg16(out, 12);
    out.push_back(3);
    for (int c = 0; c < 3; ++c) {
        out.push_back(static_cast<char>(c + 1));
        out.push_back(static_cast<char>(c == 0 ? 0x00 : 0x11));
    }
    out.push_back(0);
    out.push_back(63);
    out.push_back(0);

    BitWriter bw{out};
    int prevDc[3] = {0, 0, 0};
    const auto stride = static_cast<std::size_t>(w) * 3;
    for (int by = 0; by < h; by += 8)
        for (int bx = 0; bx < w; bx += 8) {
            float comp[3][64];
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const auto px = static_cast<std::size_t>(std::min(bx + x, w - 1));
                    const auto py = static_cast<std::size_t>(std::min(by + y, h - 1));
                    const std::uint8_t* p = img.rgb.data() + py * stride + px * 3;
                    const float r = p[0], g = p[1], b = p[2];
                    const int i = y * 8 + x;
                    comp[0][i] = 0.299f * r + 0.587f * g + 0.114f * b - 128.f;
                    comp[1][i] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                    comp[2][i] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            for (int c = 0; c < 3; ++c) {
                float freq[64];
                dct8x8(comp[c], freq);
                const auto& q = c == 0 ? ql : qc;
                const HuffCode& dc = c == 0 ? dcL : dcC;
                const HuffCode& ac = c == 0 ? acL : acC;
                int zz[64];
                for (int i = 0; i < 64; ++i) {
                    const int n = kZigzag[static_cast<std::size_t>(i)];
                    zz[i] = static_cast<int>(
                        std::lround(freq[n] / static_cast<float>(q[static_cast<std::size_t>(n)])));
                }
                const int diff = zz[0] - prevDc[c];
                prevDc[c] = zz[0];
                int nb = bitLen(diff);
                bw.put(dc.code[static_cast<std::size_t>(nb)], dc.len[static_cast<std::size_t>(nb)]);
                if (nb)
                    bw.put(static_cast<unsigned>(diff < 0 ? diff - 1 : diff), nb);
                int run = 0;
                for (int i = 1; i < 64; ++i) {
                    if (zz[i] == 0) {
                        ++run;
                        continue;
                    }
                    while (run > 15) {
                        bw.put(ac.code[0xF0], ac.len[0xF0]);
                        run -= 16;
                    }
                    nb = bitLen(zz[i]);
                    const auto sym = static_cast<std::size_t>((run << 4) | nb);
                    bw.put(ac.code[sym], ac.len[sym]);
                    bw.put(static_cast<unsigned>(zz[i] < 0 ? zz[i] - 1 : zz[i]), nb);
                    run = 0;
                }
                if (run > 0)
                    bw.put(ac.code[0], ac.len[0]);  // end of block
            }
        }
    bw.flush();
    out += "\xFF\xD9";
    return out;
}

namespace {
Result<void> saveBytes(const std::string& path, const std::string& bytes) {
    const std::u8string u8(path.begin(), path.end());
    std::ofstream f(std::filesystem::path(u8), std::ios::binary);
    if (!f)
        return Error{ErrorCode::Io, "Cannot write " + path};
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!f)
        return Error{ErrorCode::Io, "Cannot write " + path};
    return {};
}
}  // namespace

Result<void> writePng(const PlotSheet& sheet, const std::string& path, double dpi) {
    auto img = rasterize(sheet, dpi);
    if (!img)
        return img.error();
    return saveBytes(path, pngBytes(img.value()));
}

Result<void> writeJpeg(const PlotSheet& sheet, const std::string& path, double dpi, int quality) {
    auto img = rasterize(sheet, dpi);
    if (!img)
        return img.error();
    return saveBytes(path, jpegBytes(img.value(), quality));
}

}  // namespace tbcad::exportlib
