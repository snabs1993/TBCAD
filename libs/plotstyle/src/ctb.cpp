#include <miniz.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#include "tbcad/plotstyle/plotstyle.h"

namespace tbcad::plotstyle {

namespace {

constexpr char kHeader[] = "PIAFILEVERSION_2.0,CTBVER1,compress\r\npmzlibcodec";
constexpr char kStbHeader[] = "PIAFILEVERSION_2.0,STBVER1,compress\r\npmzlibcodec";

bool startsWith(std::span<const std::uint8_t> bytes, const char* header, std::size_t len) {
    return bytes.size() >= len + 12 && std::memcmp(bytes.data(), header, len) == 0;
}

std::uint32_t u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 |
           std::uint32_t(p[3]) << 24;
}

std::string trim(std::string s) {
    const char* ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos)
        return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::string unquote(std::string v) {
    v = trim(std::move(v));
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    return v;
}

}  // namespace

// The file is a short ASCII header, three little-endian words (checksum, raw size, stored size) and
// a zlib stream holding a brace-structured text:
//
//   description="..."  lineweight_table{ 0=0.0 ... }  plot_style{ 0{ name=... color=... } ... }
//
// Entry k of plot_style is palette colour k+1. `color` is -1 for "use object colour", otherwise a
// 32-bit value with the flag byte on top and R, G, B below.
Result<std::string> ctbText(std::span<const std::uint8_t> bytes) {
    const std::size_t hl = sizeof kHeader - 1;
    static_assert(sizeof kHeader == sizeof kStbHeader);
    if (!startsWith(bytes, kHeader, hl) && !startsWith(bytes, kStbHeader, hl))
        return Error{ErrorCode::Parse, "Not a compressed .ctb or .stb plot style table"};
    const std::uint32_t rawSize = u32(bytes.data() + hl + 4);
    if (rawSize == 0 || rawSize > 64u * 1024 * 1024)
        return Error{ErrorCode::Parse, "The .ctb file has an implausible size"};
    std::string text(rawSize, '\0');
    mz_ulong outLen = rawSize;
    const std::uint8_t* z = bytes.data() + hl + 12;
    if (mz_uncompress(reinterpret_cast<unsigned char*>(text.data()), &outLen, z,
                      static_cast<mz_ulong>(bytes.size() - hl - 12)) != MZ_OK)
        return Error{ErrorCode::Parse, "The .ctb data cannot be decompressed"};
    text.resize(outLen);
    return text;
}

Result<PlotStyleTable> importCtb(std::span<const std::uint8_t> bytes) {
    auto decoded = ctbText(bytes);
    if (!decoded)
        return decoded.error();
    const bool stb = startsWith(bytes, kStbHeader, sizeof kStbHeader - 1);
    std::string text = std::move(decoded.value());

    PlotStyleTable t;
    std::vector<double> weights;  // lineweight_table, mm
    enum class Section { None, Weights, Styles } section = Section::None;
    int depth = 0, styleIndex = -1;
    PlotStyleEntry cur;
    std::vector<PlotStyleEntry> styles;
    // Lineweight index per style: AutoCAD writes custom_lineweight_table after plot_style, so
    // the widths are looked up once the whole file is read.
    int curWeight = -1;
    std::vector<int> styleWeights;
    bool sawStyles = false;

    std::istringstream in(text);
    std::string raw;
    bool policyZero = false;
    while (std::getline(in, raw)) {
        std::string line = trim(raw);
        if (line.empty())
            continue;
        if (line == "}") {
            if (section == Section::Styles && depth == 2 && styleIndex >= 0) {
                // color_policy 0 keeps the object's own colour: the stored colour (white in
                // AutoCAD's tables) is then only a placeholder.
                if (policyZero)
                    cur.color.reset();
                styles.push_back(cur);
                styleWeights.push_back(curWeight);
                styleIndex = -1;
                policyZero = false;
            }
            --depth;
            if (depth == 0)
                section = Section::None;
            continue;
        }
        if (line.back() == '{') {
            std::string key = trim(line.substr(0, line.size() - 1));
            ++depth;
            if (depth == 1) {
                section = key == "lineweight_table" || key == "custom_lineweight_table"
                              ? Section::Weights
                          : key == "plot_style" ? Section::Styles
                                                : Section::None;
                if (section == Section::Styles)
                    sawStyles = true;
            } else if (depth == 2 && section == Section::Styles) {
                styleIndex = std::atoi(key.c_str());
                cur = PlotStyleEntry{};
                curWeight = -1;
            }
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq)), val = unquote(line.substr(eq + 1));
        if (depth == 0) {
            if (key == "description")
                t.description = val;
        } else if (section == Section::Weights && depth == 1) {
            std::size_t idx = static_cast<std::size_t>(std::atoi(key.c_str()));
            if (weights.size() <= idx)
                weights.resize(idx + 1, 0.0);
            weights[idx] = std::atof(val.c_str());
        } else if (section == Section::Styles && depth == 2 && styleIndex >= 0) {
            if (key == "name") {
                cur.name = val;
            } else if (key == "color") {
                const long long v = std::atoll(val.c_str());
                if (v != -1) {
                    const auto u = static_cast<std::uint32_t>(v);
                    cur.color =
                        Rgba{static_cast<std::uint8_t>(u >> 16), static_cast<std::uint8_t>(u >> 8),
                             static_cast<std::uint8_t>(u), 255};
                }
            } else if (key == "color_policy") {
                policyZero = std::atoi(val.c_str()) == 0;
            } else if (key == "screen") {
                cur.screen = std::clamp(std::atoi(val.c_str()), 0, 100);
            } else if (key == "lineweight") {
                curWeight = std::atoi(val.c_str());
            }
        }
    }
    if (!sawStyles)
        return Error{ErrorCode::Unsupported, "This plot style table has no styles"};
    // A pen's lineweight counts from 1 into the table: 0 (and 255) keep the object's lineweight,
    // n is entry n - 1. Checked against an AutoCAD plot of the owner's _EP-100.ctb, whose pens
    // 5 / 12 / 13 / 18 printed 0.13 / 0.30 / 0.33 / 0.55 mm (entries 4 / 11 / 12 / 17).
    for (std::size_t i = 0; i < styles.size(); ++i) {
        const int n = styleWeights[i];
        if (n >= 1 && static_cast<std::size_t>(n) <= weights.size())
            styles[i].lineweightMm = weights[static_cast<std::size_t>(n) - 1];
    }
    if (stb) {
        t.named = true;
        t.styles = std::move(styles);
        return t;
    }
    for (std::size_t i = 0; i < styles.size() && i < 255; ++i)
        t.entries[i + 1] = styles[i];
    return t;
}

}  // namespace tbcad::plotstyle
