// Reader for compiled AutoCAD shape fonts (.shx): "shapes 1.0 / 1.1" and "unifont 1.0". The format
// is a small byte code (vectors, arcs, sub-shapes) that is run here once per glyph and turned into
// the strokes of a StrokeFont. Big fonts (two-byte code pages) are not read.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "stroke_font.h"

namespace tbcad::font {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Reader {
    const std::uint8_t* d;
    std::size_t n;
    bool ok(std::size_t pos, std::size_t len) const { return pos <= n && len <= n - pos; }
    std::uint16_t u16(std::size_t pos) const {
        return static_cast<std::uint16_t>(d[pos] | d[pos + 1] << 8);
    }
    std::uint32_t u32(std::size_t pos) const {
        return static_cast<std::uint32_t>(u16(pos)) | static_cast<std::uint32_t>(u16(pos + 2))
                                                          << 16;
    }
};

struct Def {
    std::size_t off = 0, len = 0;
};

// Direction vectors of the 16 vector codes, in 1/2 units.
constexpr double kDir[16][2] = {{1, 0},  {1, .5},  {1, 1},  {.5, 1},   {0, 1},   {-.5, 1},
                                {-1, 1}, {-1, .5}, {-1, 0}, {-1, -.5}, {-1, -1}, {-.5, -1},
                                {0, -1}, {.5, -1}, {1, -1}, {1, -.5}};

struct Machine {
    const Reader& rd;
    const std::map<std::uint32_t, Def>& defs;
    bool unicode;
    // state
    double x = 0, y = 0;
    double mul = 1, div = 1;
    bool pen = true;
    std::vector<FPoint> stackPts{};
    StrokePath cur{};
    StrokeGlyph out{};
    std::size_t steps = 0;

    double sc() const { return mul / div; }
    void flush() {
        if (cur.points.size() >= 2)
            out.paths.push_back(std::move(cur));
        cur = StrokePath{};
    }
    void lineTo(double nx, double ny) {
        if (pen) {
            if (cur.points.empty())
                cur.points.push_back({x, y});
            cur.points.push_back({nx, ny});
        } else {
            flush();
        }
        x = nx;
        y = ny;
    }
    void arc(double cx, double cy, double r, double a0, double a1) {
        const double span = a1 - a0;
        const int segs =
            std::clamp(static_cast<int>(std::ceil(std::abs(span) / (kPi / 16))), 1, 64);
        for (int i = 1; i <= segs; ++i) {
            const double a = a0 + span * i / segs;
            lineTo(cx + r * std::cos(a), cy + r * std::sin(a));
        }
    }
    // A bulge arc from the pen to (x+dx, y+dy).
    void bulge(double dx, double dy, double b) {
        const double nx = x + dx, ny = y + dy;
        if (b == 0 || (dx == 0 && dy == 0)) {
            lineTo(nx, ny);
            return;
        }
        const double theta = 4 * std::atan(b);  // included angle, signed
        const double chord = std::hypot(dx, dy);
        const double r = chord / (2 * std::sin(std::abs(theta) / 2));
        const double mx = (x + nx) / 2, my = (y + ny) / 2;
        const double h = r * std::cos(theta / 2);        // centre distance from the chord midpoint
        const double ux = -dy / chord, uy = dx / chord;  // left normal
        const double sgn = b > 0 ? 1 : -1;               // ccw bulge puts the centre on the left
        const double cx = mx + sgn * ux * h, cy = my + sgn * uy * h;
        const double a0 = std::atan2(y - cy, x - cx);
        arc(cx, cy, r, a0, a0 + theta);
        x = nx;
        y = ny;
    }

    std::size_t operandsOf(std::uint8_t b, std::size_t at, std::size_t end) const {
        switch (b) {
            case 3:
            case 4:
                return 1;
            case 7:
                return unicode ? 2 : 1;
            case 8:
                return 2;
            case 0xA:
                return 2;
            case 0xB:
                return 5;
            case 0xC:
                return 3;
            case 9:
            case 0xD: {  // up to the (0,0) pair
                std::size_t i = at;
                const std::size_t step = b == 9 ? 2 : 3;
                while (i + 1 < end) {
                    const bool zero = rd.d[i] == 0 && rd.d[i + 1] == 0;
                    i += zero ? 2 : step;
                    if (zero)
                        break;
                }
                return i - at;
            }
            default:
                return 0;
        }
    }

    bool run(const Def& def, int depth) {
        if (depth > 8)
            return false;
        const std::size_t end = def.off + def.len;
        std::size_t i = def.off;
        auto sbyte = [&](std::size_t p) {
            return static_cast<double>(static_cast<std::int8_t>(rd.d[p]));
        };
        while (i < end) {
            if (++steps > 200000)
                return false;
            const std::uint8_t b = rd.d[i++];
            if (b >= 0x10) {
                const double len = (b >> 4) * sc();
                lineTo(x + kDir[b & 15][0] * len, y + kDir[b & 15][1] * len);
                continue;
            }
            switch (b) {
                case 0:
                    return true;
                case 1:
                    pen = true;
                    break;
                case 2:
                    flush();
                    pen = false;
                    break;
                case 3:
                    if (i >= end)
                        return true;
                    if (rd.d[i])
                        div *= rd.d[i];
                    ++i;
                    break;
                case 4:
                    if (i >= end)
                        return true;
                    if (rd.d[i])
                        mul *= rd.d[i];
                    ++i;
                    break;
                case 5:
                    if (stackPts.size() < 4)
                        stackPts.push_back({x, y});
                    break;
                case 6:
                    if (!stackPts.empty()) {
                        flush();
                        x = stackPts.back().x;
                        y = stackPts.back().y;
                        stackPts.pop_back();
                    }
                    break;
                case 7: {
                    std::uint32_t num = 0;
                    if (unicode) {
                        if (i + 2 > end)
                            return true;
                        num = rd.u16(i);
                        i += 2;
                    } else {
                        if (i >= end)
                            return true;
                        num = rd.d[i++];
                    }
                    auto it = defs.find(num);
                    if (it != defs.end() && !run(it->second, depth + 1))
                        return false;
                    break;
                }
                case 8:
                    if (i + 2 > end)
                        return true;
                    lineTo(x + sbyte(i) * sc(), y + sbyte(i + 1) * sc());
                    i += 2;
                    break;
                case 9:
                    while (i + 2 <= end) {
                        const double dx = sbyte(i), dy = sbyte(i + 1);
                        i += 2;
                        if (dx == 0 && dy == 0)
                            break;
                        lineTo(x + dx * sc(), y + dy * sc());
                    }
                    break;
                case 0xA: {  // octant arc
                    if (i + 2 > end)
                        return true;
                    const double r = rd.d[i] * sc();
                    const std::int8_t o = static_cast<std::int8_t>(rd.d[i + 1]);
                    i += 2;
                    const int startOct = (std::abs(o) >> 4) & 7;
                    int count = std::abs(o) & 7;
                    if (count == 0)
                        count = 8;
                    const double dir = o < 0 ? -1 : 1;
                    const double a0 = startOct * kPi / 4;
                    const double a1 = a0 + dir * count * kPi / 4;
                    arc(x - r * std::cos(a0), y - r * std::sin(a0), r, a0, a1);
                    break;
                }
                case 0xB: {  // fractional arc
                    if (i + 5 > end)
                        return true;
                    const double so = rd.d[i], eo = rd.d[i + 1];
                    const double r = (rd.d[i + 2] * 256.0 + rd.d[i + 3]) * sc();
                    const std::int8_t o = static_cast<std::int8_t>(rd.d[i + 4]);
                    i += 5;
                    const int startOct = (std::abs(o) >> 4) & 7;
                    int count = std::abs(o) & 7;
                    if (count == 0)
                        count = 8;
                    const double dir = o < 0 ? -1 : 1;
                    const double a0 = (startOct + so / 256.0) * kPi / 4;
                    const double a1 = (startOct + dir * count + dir * eo / 256.0) * kPi / 4;
                    arc(x - r * std::cos(a0), y - r * std::sin(a0), r, a0, a1);
                    break;
                }
                case 0xC:
                    if (i + 3 > end)
                        return true;
                    bulge(sbyte(i) * sc(), sbyte(i + 1) * sc(), sbyte(i + 2) / 127.0);
                    i += 3;
                    break;
                case 0xD:
                    while (i + 2 <= end) {
                        const double dx = sbyte(i), dy = sbyte(i + 1);
                        if (dx == 0 && dy == 0) {
                            i += 2;
                            break;
                        }
                        if (i + 3 > end)
                            break;
                        bulge(dx * sc(), dy * sc(), sbyte(i + 2) / 127.0);
                        i += 3;
                    }
                    break;
                case 0xE: {  // the next command is for vertical text only
                    if (i >= end)
                        return true;
                    const std::uint8_t nb = rd.d[i++];
                    if (nb >= 0x10)
                        break;
                    i += operandsOf(nb, i, end);
                    break;
                }
                default:
                    break;
            }
        }
        return true;
    }
};

// Thai shapes fonts number their glyphs by Windows-874 code.
char32_t cp874(unsigned c) {
    if (c >= 0xA1 && c <= 0xDA)
        return 0x0E01 + (c - 0xA1);
    if (c >= 0xDF && c <= 0xFB)
        return 0x0E3F + (c - 0xDF);
    return 0;
}

}  // namespace

std::string StrokeFont::loadShx(const std::uint8_t* data, std::size_t size, bool thai) {
    const Reader rd{data, size};
    auto startsWith = [&](const char* s) {
        const std::size_t l = std::strlen(s);
        return size >= l && std::memcmp(data, s, l) == 0;
    };
    const bool uni = startsWith("AutoCAD-86 unifont 1.0");
    const bool shapes = startsWith("AutoCAD-86 shapes 1.0") || startsWith("AutoCAD-86 shapes 1.1");
    if (!uni && !shapes) {
        if (startsWith("AutoCAD-86 bigfont"))
            return "big fonts are not supported";
        return "not a compiled SHX font";
    }
    std::size_t pos = 0;
    while (pos < size && data[pos] != 0x1A)
        ++pos;
    ++pos;

    std::map<std::uint32_t, Def> defs;
    Def info;
    std::string name;
    if (shapes) {
        if (!rd.ok(pos, 6))
            return "truncated SHX file";
        const std::size_t count = rd.u16(pos + 4);
        pos += 6;
        if (!rd.ok(pos, count * 4))
            return "truncated SHX file";
        std::size_t table = pos;
        pos += count * 4;
        for (std::size_t k = 0; k < count; ++k) {
            const std::uint32_t num = rd.u16(table + k * 4);
            const std::size_t len = rd.u16(table + k * 4 + 2);
            if (!rd.ok(pos, len))
                return "truncated SHX file";
            // name, 0, then the byte code
            std::size_t z = pos;
            while (z < pos + len && data[z] != 0)
                ++z;
            Def d{std::min(z + 1, pos + len), pos + len - std::min(z + 1, pos + len)};
            if (num == 0) {
                info = d;
                name.assign(reinterpret_cast<const char*>(data + pos), z - pos);
            } else {
                defs[num] = d;
            }
            pos += len;
        }
    } else {
        if (!rd.ok(pos, 6))
            return "truncated SHX file";
        const std::size_t count = rd.u32(pos);
        const std::size_t headLen = rd.u16(pos + 4);
        pos += 6;
        if (!rd.ok(pos, headLen))
            return "truncated SHX file";
        std::size_t z = pos;
        while (z < pos + headLen && data[z] != 0)
            ++z;
        name.assign(reinterpret_cast<const char*>(data + pos), z - pos);
        info = Def{std::min(z + 1, pos + headLen), pos + headLen - std::min(z + 1, pos + headLen)};
        pos += headLen;
        for (std::size_t k = 0; k < count && rd.ok(pos, 4); ++k) {
            const std::uint32_t code = rd.u16(pos);
            const std::size_t len = rd.u16(pos + 2);
            pos += 4;
            if (!rd.ok(pos, len))
                break;
            std::size_t g = pos;
            while (g < pos + len && data[g] != 0)
                ++g;
            const std::size_t body = std::min(g + 1, pos + len);
            defs[code] = Def{body, pos + len - body};
            pos += len;
        }
    }
    if (info.len < 2)
        return "SHX font has no header";
    const double above = data[info.off];
    const double below = data[info.off + 1];
    if (!(above > 0))
        return "SHX font has no height";

    glyphs_.clear();
    const double u = 1.0 / above;
    for (const auto& [num, def] : defs) {
        char32_t cp = num;
        if (shapes) {
            if (num < 0x20 || num > 0xFF)
                continue;
            if (num >= 0x80) {
                cp = thai ? cp874(num) : (num >= 0xA0 ? num : 0);
                if (!cp)
                    continue;
            }
        }
        Machine m{rd, defs, uni};
        if (!m.run(def, 0))
            continue;
        m.flush();
        Glyph g;
        g.advance = m.x * u;
        for (StrokePath& p : m.out.paths) {
            for (FPoint& pt : p.points) {
                pt.x *= u;
                pt.y *= u;
            }
            g.strokes.paths.push_back(std::move(p));
        }
        glyphs_[cp] = std::move(g);
    }
    if (glyphs_.empty())
        return "SHX font has no usable glyphs";
    upm_ = static_cast<int>(above);
    metrics_.unitsPerEm = upm_;
    metrics_.capHeight = 1.0;
    metrics_.xHeight = 0.7;
    metrics_.ascender = 1.0;
    metrics_.descender = -below * u;
    family_ = name.empty() ? "SHX" : name;
    toneDrop_ = 0;
    return {};
}

}  // namespace tbcad::font
