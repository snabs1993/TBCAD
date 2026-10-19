// Proxy graphics: the drawing that ACAD_PROXY_ENTITY and many "objects of other applications" (AEC,
// Civil, ACIS solids ...) carry so that programs without the application can still show them.
//
// The data is a sequence of records `size:u32 type:u32 payload`, after an 8 byte header. Only the
// parts that matter for a 2D plan are read: curves, polylines, polygons, shells and meshes (drawn
// as their outlines), a few attributes (colour, true colour, line weight, transformation) and text.
// Everything else is skipped by its size. A damaged record ends the decoding with a warning; what
// was read so far stays.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <type_traits>
#include <variant>

#include "importer.h"
#include "tbcad/geom/entity.h"

namespace tbcad::dxf::detail {

namespace {

// Record types (the numbering of the published proxy graphics format).
enum ProxyType : std::uint32_t {
    kCircle = 2,
    kCircle3P = 3,
    kArc = 4,
    kArc3P = 5,
    kPolyline = 6,
    kPolygon = 7,
    kMesh = 8,
    kShell = 9,
    kText = 10,
    kText2 = 11,
    kColor = 14,
    kFill = 20,
    kTrueColor = 22,
    kLineweight = 23,
    kPushMatrix = 29,
    kPushMatrix2 = 30,
    kPopMatrix = 31,
    kPolylineNormals = 32,
    kUnicodeText = 36,
    kUnicodeText2 = 38,
    kEllipticArc = 44,
};

constexpr std::size_t kMaxItems = 200000;       // display items per proxy
constexpr std::uint32_t kMaxPoints = 1u << 20;  // vertices in one record

struct V3 {
    double x = 0, y = 0, z = 0;
};

// Bounds-checked little endian reader over one record's payload.
class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> s) : s_(s) {}
    bool ok() const { return ok_; }
    std::size_t remaining() const { return s_.size() - pos_; }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        if (!need(4))
            return 0;
        std::memcpy(&v, s_.data() + pos_, 4);
        pos_ += 4;
        return v;
    }
    double f64() {
        double v = 0;
        if (!need(8))
            return 0;
        std::memcpy(&v, s_.data() + pos_, 8);
        pos_ += 8;
        return std::isfinite(v) ? v : (ok_ = false, 0.0);
    }
    V3 v3() {
        V3 v;
        v.x = f64();
        v.y = f64();
        v.z = f64();
        return v;
    }
    // Zero terminated string, then padding to a multiple of 4 bytes from the payload start.
    std::string cstring() {
        std::size_t end = pos_;
        while (end < s_.size() && s_[end] != 0)
            ++end;
        if (end >= s_.size()) {
            ok_ = false;
            return {};
        }
        std::string r(reinterpret_cast<const char*>(s_.data() + pos_), end - pos_);
        pos_ = (end + 1 + 3) & ~std::size_t{3};
        if (pos_ > s_.size())
            pos_ = s_.size();
        return r;
    }
    // UTF-16LE string ended by two zero bytes, padded like cstring.
    std::string ustring() {
        std::u16string w;
        std::size_t i = pos_;
        for (; i + 1 < s_.size(); i += 2) {
            char16_t c = static_cast<char16_t>(s_[i] | (s_[i + 1] << 8));
            if (c == 0)
                break;
            w.push_back(c);
        }
        if (i + 1 >= s_.size()) {
            ok_ = false;
            return {};
        }
        pos_ = (i + 2 + 3) & ~std::size_t{3};
        if (pos_ > s_.size())
            pos_ = s_.size();
        std::string out;
        for (std::size_t k = 0; k < w.size(); ++k) {
            std::uint32_t cp = w[k];
            if (cp >= 0xD800 && cp < 0xDC00 && k + 1 < w.size() && w[k + 1] >= 0xDC00 &&
                w[k + 1] < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (w[++k] - 0xDC00);
            } else if (cp >= 0xD800 && cp < 0xE000) {
                cp = 0xFFFD;
            }
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        return out;
    }

private:
    bool need(std::size_t n) {
        if (!ok_ || remaining() < n) {
            ok_ = false;
            return false;
        }
        return true;
    }
    std::span<const std::uint8_t> s_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

struct State {
    Color color = Color::byLayer();
    LineWeight lineweight = LineWeight::byLayer();
    bool fill = false;
    bool haveMatrix = false;
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::vector<std::array<double, 16>> stack{};
};

}  // namespace

// Draws the proxy graphics `data` into `proxy.graphics` and adds loose text entities to `texts`.
// Returns the number of records that were not understood. `problem` says why decoding stopped
// early.
std::size_t Importer::decodeProxyGraphics(std::span<const std::uint8_t> data,
                                          const EntityCommon& base, ProxyData& proxy,
                                          std::vector<Entity>& texts, std::string& problem) {
    std::size_t unknown = 0;
    if (data.size() < 16) {
        problem = "proxy graphics are too short";
        return 0;
    }
    State st;
    geom::DisplayContext ctx;
    ctx.chordTolMm = 0.01;

    auto xf = [&](V3 p) -> Vec2 {
        if (!st.haveMatrix)
            return {p.x * k_, p.y * k_};
        const double* m = st.m;
        double x = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
        double y = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
        return {x * k_, y * k_};
    };
    auto emit = [&](Entity e) {
        if (proxy.graphics.items.size() >= kMaxItems)
            return;
        e.common = base;
        if (!(st.color == Color::byLayer()))
            e.common.color = st.color;
        if (!st.lineweight.isByLayer())
            e.common.lineweight = st.lineweight;
        geom::buildDisplay(*doc_, e, ctx, proxy.graphics);
    };
    auto polyline = [&](const std::vector<Vec2>& pts, bool closed) {
        if (pts.size() < 2)
            return;
        PolylineData pl;
        pl.closed = closed;
        for (const Vec2& p : pts)
            pl.v.push_back({p, 0, 0, 0});
        Entity e;
        e.data = std::move(pl);
        emit(std::move(e));
    };
    auto fillRing = [&](const std::vector<Vec2>& pts) {
        if (pts.size() < 3 || proxy.graphics.items.size() >= kMaxItems)
            return;
        Entity probe;
        probe.common = base;
        if (!(st.color == Color::byLayer()))
            probe.common.color = st.color;
        probe.data = PointData{};
        DisplayFill f;
        f.style = geom::resolveStyle(*doc_, probe, ctx);
        f.rings.push_back(pts);
        proxy.graphics.items.emplace_back(std::move(f));
    };
    // Arc of the circle around `c` in the plane (ux, uy) from angle a for sweep s.
    auto sampleArc = [&](V3 c, double r, V3 ux, V3 uy, double a, double s) {
        std::vector<Vec2> pts;
        const double want = std::isfinite(s) ? std::ceil(std::abs(s) / kTwoPi * 96) : 192.0;
        const int n = static_cast<int>(std::clamp(want, 4.0, 192.0));
        for (int i = 0; i <= n; ++i) {
            double t = a + s * i / n;
            double ct = std::cos(t) * r, stt = std::sin(t) * r;
            pts.push_back(xf({c.x + ct * ux.x + stt * uy.x, c.y + ct * ux.y + stt * uy.y,
                              c.z + ct * ux.z + stt * uy.z}));
        }
        return pts;
    };
    auto unit = [](V3 v) {
        double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 0 ? V3{v.x / l, v.y / l, v.z / l} : V3{1, 0, 0};
    };
    auto cross = [](V3 a, V3 b) {
        return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    // Centre of the circle through three points in the XY plane (false when collinear).
    auto circumcircle = [](V3 a, V3 b, V3 c, double& cx, double& cy, double& r) {
        // Worked out around `a`: survey-sized coordinates would lose every digit of the squares.
        b = {b.x - a.x, b.y - a.y, b.z};
        c = {c.x - a.x, c.y - a.y, c.z};
        const double ax = a.x, ay = a.y;
        a = {0, 0, a.z};
        double d = 2 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
        if (std::abs(d) < 1e-12)
            return false;
        double a2 = a.x * a.x + a.y * a.y, b2 = b.x * b.x + b.y * b.y, c2 = c.x * c.x + c.y * c.y;
        cx = (a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d;
        cy = (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d;
        r = std::hypot(a.x - cx, a.y - cy);
        cx += ax;
        cy += ay;
        return std::isfinite(r) && r > 0;
    };

    std::size_t pos = 8;
    std::size_t records = 0;
    while (pos + 8 <= data.size()) {
        std::uint32_t size = 0, type = 0;
        std::memcpy(&size, data.data() + pos, 4);
        std::memcpy(&type, data.data() + pos + 4, 4);
        if (size < 8 || size > data.size() - pos) {
            problem = "a proxy graphics record has an impossible size; the rest was ignored";
            break;
        }
        if (++records > 1000000) {
            problem = "too many proxy graphics records; the rest was ignored";
            break;
        }
        Cursor c(data.subspan(pos + 8, size - 8));
        pos += size;
        bool understood = true;
        switch (type) {
            case kCircle: {
                V3 ctr = c.v3();
                double r = c.f64();
                V3 n = c.v3();
                if (!c.ok() || !(r > 0)) {
                    understood = false;
                    break;
                }
                if (!st.haveMatrix && std::abs(n.z) > 0.999) {
                    Entity e;
                    e.data = CircleData{{ctr.x * k_, ctr.y * k_}, r * k_};
                    emit(std::move(e));
                } else {
                    V3 ux = unit(std::abs(n.z) > 0.999 ? V3{1, 0, 0} : cross(V3{0, 0, 1}, n));
                    V3 uy = cross(unit(n), ux);
                    polyline(sampleArc(ctr, r, ux, uy, 0, kTwoPi), true);
                }
                break;
            }
            case kCircle3P: {
                V3 a = c.v3(), b = c.v3(), d = c.v3();
                double cx, cy, r;
                if (!c.ok() || !circumcircle(a, b, d, cx, cy, r)) {
                    understood = false;
                    break;
                }
                polyline(sampleArc({cx, cy, a.z}, r, {1, 0, 0}, {0, 1, 0}, 0, kTwoPi), true);
                break;
            }
            case kArc: {
                V3 ctr = c.v3();
                double r = c.f64();
                V3 n = unit(c.v3());
                V3 sv = unit(c.v3());
                double sweep = c.f64();
                if (!c.ok() || !(r > 0) || sweep == 0) {
                    understood = false;
                    break;
                }
                V3 uy = cross(n, sv);
                polyline(sampleArc(ctr, r, sv, uy, 0, sweep), false);
                break;
            }
            case kArc3P: {
                V3 a = c.v3(), b = c.v3(), d = c.v3();
                double cx, cy, r;
                if (!c.ok() || !circumcircle(a, b, d, cx, cy, r)) {
                    understood = false;
                    break;
                }
                double a0 = std::atan2(a.y - cy, a.x - cx), a1 = std::atan2(b.y - cy, b.x - cx),
                       a2 = std::atan2(d.y - cy, d.x - cx);
                auto ccw = [](double from, double to) {
                    double s = std::fmod(to - from, kTwoPi);
                    return s < 0 ? s + kTwoPi : s;
                };
                // Counter-clockwise from a to d passes through b, or the clockwise way does.
                double sweep = ccw(a0, a1) <= ccw(a0, a2) ? ccw(a0, a2) : ccw(a0, a2) - kTwoPi;
                polyline(sampleArc({cx, cy, a.z}, r, {1, 0, 0}, {0, 1, 0}, a0, sweep), false);
                break;
            }
            case kPolyline:
            case kPolygon:
            case kPolylineNormals: {
                std::uint32_t n = c.u32();
                if (!c.ok() || n == 0 || n > kMaxPoints ||
                    c.remaining() < static_cast<std::size_t>(n) * 24) {
                    understood = false;
                    break;
                }
                std::vector<Vec2> pts;
                pts.reserve(n);
                for (std::uint32_t i = 0; i < n; ++i)
                    pts.push_back(xf(c.v3()));
                if (!c.ok()) {
                    understood = false;
                    break;
                }
                if (n == 1) {
                    proxy.graphics.items.emplace_back(
                        DisplayPoints{pts, resolveProxyStyle(base, st.color)});
                } else if (type == kPolygon && st.fill) {
                    fillRing(pts);
                } else {
                    polyline(pts, type == kPolygon);
                }
                break;
            }
            case kEllipticArc: {
                V3 ctr = c.v3();
                V3 n = c.v3();
                double major = c.f64(), minor = c.f64(), t0 = c.f64(), t1 = c.f64(), ang = c.f64();
                (void)n;
                if (!c.ok() || !(major > 0) || !(minor > 0)) {
                    understood = false;
                    break;
                }
                V3 ux{std::cos(ang), std::sin(ang), 0}, uy{-std::sin(ang), std::cos(ang), 0};
                std::vector<Vec2> pts;
                double span = t1 - t0;
                if (span <= 0)
                    span += kTwoPi;
                const int steps = 96;
                for (int i = 0; i <= steps; ++i) {
                    double t = t0 + span * i / steps;
                    double a = std::cos(t) * major, b = std::sin(t) * minor;
                    pts.push_back(
                        xf({ctr.x + a * ux.x + b * uy.x, ctr.y + a * ux.y + b * uy.y, ctr.z}));
                }
                polyline(pts, false);
                break;
            }
            case kShell: {
                std::uint32_t nv = c.u32();
                if (!c.ok() || nv > kMaxPoints ||
                    c.remaining() < static_cast<std::size_t>(nv) * 24) {
                    understood = false;
                    break;
                }
                std::vector<Vec2> verts;
                verts.reserve(nv);
                for (std::uint32_t i = 0; i < nv; ++i)
                    verts.push_back(xf(c.v3()));
                std::uint32_t entries = c.u32();
                std::uint32_t read = 0;
                while (c.ok() && read < entries && proxy.graphics.items.size() < kMaxItems) {
                    auto count = static_cast<std::int32_t>(c.u32());
                    std::uint32_t edges =
                        count < 0 ? static_cast<std::uint32_t>(-static_cast<std::int64_t>(count))
                                  : static_cast<std::uint32_t>(count);
                    read += 1 + edges;
                    if (!c.ok() || edges > kMaxPoints ||
                        c.remaining() < static_cast<std::size_t>(edges) * 4) {
                        understood = false;
                        break;
                    }
                    std::vector<Vec2> ring;
                    for (std::uint32_t i = 0; i < edges; ++i) {
                        std::uint32_t idx = c.u32();
                        if (idx < verts.size())
                            ring.push_back(verts[idx]);
                    }
                    polyline(ring, true);
                }
                break;
            }
            case kMesh: {
                std::uint32_t rows = c.u32(), cols = c.u32();
                if (!c.ok() || rows == 0 || cols == 0 || rows > 4096 || cols > 4096 ||
                    c.remaining() < static_cast<std::size_t>(rows) * cols * 24) {
                    understood = false;
                    break;
                }
                std::vector<Vec2> grid(static_cast<std::size_t>(rows) * cols);
                for (Vec2& p : grid)
                    p = xf(c.v3());
                for (std::uint32_t r = 0; r < rows && c.ok(); ++r) {
                    std::vector<Vec2> line(
                        grid.begin() + static_cast<std::ptrdiff_t>(r * cols),
                        grid.begin() + static_cast<std::ptrdiff_t>((r + 1) * cols));
                    polyline(line, false);
                }
                for (std::uint32_t cc = 0; cc < cols; ++cc) {
                    std::vector<Vec2> line;
                    for (std::uint32_t r = 0; r < rows; ++r)
                        line.push_back(grid[static_cast<std::size_t>(r) * cols + cc]);
                    polyline(line, false);
                }
                break;
            }
            case kText:
            case kText2:
            case kUnicodeText:
            case kUnicodeText2: {
                V3 start = c.v3();
                (void)c.v3();  // normal
                V3 dir = c.v3();
                double height = 0, width = 1, oblique = 0;
                std::string text;
                const bool unicode = type == kUnicodeText || type == kUnicodeText2;
                if (type == kText || type == kUnicodeText) {
                    height = c.f64();
                    width = c.f64();
                    oblique = c.f64();
                    text = unicode ? c.ustring() : dec(c.cstring());
                } else {
                    text = unicode ? c.ustring() : dec(c.cstring());
                    (void)c.u32();
                    (void)c.u32();
                    height = c.f64();
                    width = c.f64();
                    oblique = c.f64();
                }
                if (!c.ok() || !(height > 0) || text.empty()) {
                    understood = false;
                    break;
                }
                TextData t;
                t.position = xf(start);
                t.alignPoint = t.position;
                t.height = height * k_;
                t.rotation = std::atan2(dir.y, dir.x);
                t.widthFactor = width > 0 ? width : 1;
                t.oblique = oblique;
                t.utf8 = std::move(text);
                Entity e;
                e.common = base;
                if (!(st.color == Color::byLayer()))
                    e.common.color = st.color;
                e.data = std::move(t);
                texts.push_back(std::move(e));
                break;
            }
            case kColor: {
                std::uint32_t aci = c.u32();
                if (!c.ok())
                    break;
                st.color = aci == 256 ? Color::byLayer()
                           : aci == 0
                               ? Color::byBlock()
                               : (aci < 256 ? mapAci(static_cast<int>(aci)) : Color::byLayer());
                break;
            }
            case kTrueColor: {
                std::uint32_t raw = c.u32();
                if (!c.ok())
                    break;
                switch (raw >> 24) {
                    case 0xC2:
                        st.color = Color::rgb(static_cast<std::uint8_t>(raw >> 16),
                                              static_cast<std::uint8_t>(raw >> 8),
                                              static_cast<std::uint8_t>(raw));
                        break;
                    case 0xC3:
                        st.color = mapAci(static_cast<int>(raw & 0xFF));
                        break;
                    case 0xC1:
                        st.color = Color::byBlock();
                        break;
                    default:
                        st.color = Color::byLayer();
                }
                break;
            }
            case kLineweight: {
                std::uint32_t lw = c.u32();
                if (!c.ok())
                    break;
                auto v = static_cast<std::int32_t>(lw);
                st.lineweight = v >= 0 && v <= 2100
                                    ? LineWeight::hundredths(static_cast<std::int16_t>(v))
                                    : LineWeight::byLayer();
                break;
            }
            case kFill:
                st.fill = c.u32() != 0;
                break;
            case kPushMatrix:
            case kPushMatrix2: {
                std::array<double, 16> m{};
                for (double& v : m)
                    v = c.f64();
                if (!c.ok() || st.stack.size() > 64)
                    break;
                st.stack.push_back(m);
                std::memcpy(st.m, m.data(), sizeof st.m);
                st.haveMatrix = true;
                break;
            }
            case kPopMatrix:
                if (!st.stack.empty())
                    st.stack.pop_back();
                if (st.stack.empty()) {
                    st.haveMatrix = false;
                } else {
                    std::memcpy(st.m, st.stack.back().data(), sizeof st.m);
                }
                break;
            default:
                understood = false;
        }
        if (!understood)
            ++unknown;
        // The fill flag applies to the next drawn primitive only.
        if ((type >= kCircle && type <= kShell) || type == kPolylineNormals || type == kEllipticArc)
            st.fill = false;
    }
    return unknown;
}

// Removes the text records from a proxy graphics stream (the header's total size and record count
// are rewritten; bytes after the stream are kept). The texts live on as ordinary Text entities,
// so exporting the proxy again does not draw them twice. A stream that does not parse is left as
// it is.
void stripProxyText(std::vector<std::uint8_t>& raw) {
    if (raw.size() < 16)
        return;
    std::uint32_t total = 0, count = 0;
    std::memcpy(&total, raw.data(), 4);
    std::memcpy(&count, raw.data() + 4, 4);
    if (total < 8 || total > raw.size())
        return;
    std::vector<std::uint8_t> out(raw.begin(), raw.begin() + 8);
    std::uint32_t kept = 0;
    std::size_t pos = 8;
    while (pos + 8 <= total) {
        std::uint32_t size = 0, type = 0;
        std::memcpy(&size, raw.data() + pos, 4);
        std::memcpy(&type, raw.data() + pos + 4, 4);
        if (size < 8 || size > total - pos)
            return;
        if (type != kText && type != kText2 && type != kUnicodeText && type != kUnicodeText2) {
            out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                       raw.begin() + static_cast<std::ptrdiff_t>(pos + size));
            ++kept;
        }
        pos += size;
    }
    if (pos != total)
        return;
    const std::uint32_t newTotal = static_cast<std::uint32_t>(out.size());
    std::memcpy(out.data(), &newTotal, 4);
    std::memcpy(out.data() + 4, &kept, 4);
    out.insert(out.end(), raw.begin() + total, raw.end());
    raw = std::move(out);
}

bool stripProxyText(Record& r) {
    std::vector<std::uint8_t> gfx;
    bool seenSize = false;
    std::size_t sizeAt = r.g.size();
    for (std::size_t i = 0; i < r.g.size(); ++i) {
        const Group& g = r.g[i];
        if (g.code == 92 || g.code == 160) {
            if (!seenSize)
                sizeAt = i;
            seenSize = true;
        } else if (g.code >= 310 && g.code <= 319 && seenSize) {
            if (const auto* bytes = std::get_if<std::vector<std::byte>>(&g.value))
                for (std::byte b : *bytes)
                    gfx.push_back(static_cast<std::uint8_t>(b));
        }
    }
    const std::size_t before = gfx.size();
    stripProxyText(gfx);
    if (gfx.size() == before)
        return false;
    // The size group keeps its integer type; the bytes go back as 310 groups of 127 bytes, where
    // the first graphics group was.
    Group& size = r.g[sizeAt];
    std::visit(
        [&](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
                v = static_cast<T>(gfx.size());
        },
        size.value);
    std::vector<Group> out;
    out.reserve(r.g.size());
    bool placed = false;
    for (std::size_t i = 0; i < r.g.size(); ++i) {
        Group& g = r.g[i];
        if (i > sizeAt && g.code >= 310 && g.code <= 319 &&
            std::holds_alternative<std::vector<std::byte>>(g.value)) {
            if (!placed) {
                placed = true;
                for (std::size_t at = 0; at < gfx.size(); at += 127) {
                    Group chunk;
                    chunk.code = 310;
                    chunk.line = g.line;
                    std::vector<std::byte> bytes;
                    for (std::size_t k = at; k < std::min(gfx.size(), at + 127); ++k)
                        bytes.push_back(static_cast<std::byte>(gfx[k]));
                    chunk.value = std::move(bytes);
                    out.push_back(std::move(chunk));
                }
            }
            continue;
        }
        out.push_back(std::move(g));
    }
    r.g = std::move(out);
    return true;
}

DisplayStyle Importer::resolveProxyStyle(const EntityCommon& base, Color color) {
    Entity probe;
    probe.common = base;
    if (!(color == Color::byLayer()))
        probe.common.color = color;
    probe.data = PointData{};
    geom::DisplayContext ctx;
    return geom::resolveStyle(*doc_, probe, ctx);
}

// Collects the proxy graphics bytes of a record: the 310 data that follows group 92 or 160, up to
// group 93 (the entity data of an ACAD_PROXY_ENTITY comes after it).
std::vector<std::uint8_t> Importer::proxyGraphicsOf(const Record& r) const {
    std::vector<std::uint8_t> out;
    bool seenSize = false;
    for (const Group& g : r.g) {
        if (g.code == 92 || g.code == 160) {
            seenSize = true;
        } else if (g.code == 93 && r.type == "ACAD_PROXY_ENTITY") {
            break;
        } else if (g.code >= 310 && g.code <= 319 && (seenSize || r.type == "ACAD_PROXY_ENTITY")) {
            if (const auto* bytes = std::get_if<std::vector<std::byte>>(&g.value)) {
                for (std::byte b : *bytes)
                    out.push_back(static_cast<std::uint8_t>(b));
            }
        }
    }
    return out;
}


// A MESH (subdivision mesh) carries its control cage in plain groups: vertices (92 then 10/20/30),
// faces (93 then a list of 90: count followed by vertex indices) and edges (94 then pairs of 90).
// The cage edges are drawn as seen from above; the smoothing of higher subdivision levels is not.
bool Importer::meshEdges(const Record& r, const EntityCommon& base, ProxyData& proxy) {
    std::vector<Vec2> verts;
    std::vector<std::int64_t> faceList, edgeList;
    enum class Part { None, Verts, Faces, Edges } part = Part::None;
    std::int64_t left = 0;
    bool inMesh = false;
    for (const Group& g : r.g) {
        if (g.code == 100) {
            inMesh = g.str() == "AcDbSubDMesh";
            part = Part::None;
            continue;
        }
        if (!inMesh)
            continue;
        if (g.code == 92) {
            part = Part::Verts;
            left = g.integer();
        } else if (g.code == 93) {
            part = Part::Faces;
            left = g.integer();
        } else if (g.code == 94) {
            part = Part::Edges;
            left = 2 * g.integer();
        } else if (g.code == 95 || (g.code == 90 && left <= 0)) {
            part = Part::None;
        } else if (part == Part::Verts && g.code == 10 && left > 0) {
            verts.push_back({g.number() * k_, 0});
        } else if (part == Part::Verts && g.code == 20 && !verts.empty()) {
            verts.back().y = g.number() * k_;
            --left;
        } else if (g.code == 90 && left > 0 && part == Part::Faces) {
            faceList.push_back(g.integer());
            --left;
        } else if (g.code == 90 && left > 0 && part == Part::Edges) {
            edgeList.push_back(g.integer());
            --left;
        }
    }
    const auto nv = static_cast<std::int64_t>(verts.size());
    std::set<std::pair<std::int64_t, std::int64_t>> seen;
    std::vector<std::pair<std::int64_t, std::int64_t>> edges;
    auto add = [&](std::int64_t a, std::int64_t b) {
        if (a < 0 || b < 0 || a >= nv || b >= nv || a == b)
            return;
        if (seen.insert({std::min(a, b), std::max(a, b)}).second)
            edges.push_back({a, b});
    };
    for (std::size_t i = 0; i + 1 < edgeList.size(); i += 2)
        add(edgeList[i], edgeList[i + 1]);
    for (std::size_t i = 0; i < faceList.size();) {
        const std::int64_t n = faceList[i];
        if (n < 2 || i + 1 + static_cast<std::size_t>(n) > faceList.size())
            break;
        for (std::int64_t j = 0; j < n; ++j)
            add(faceList[i + 1 + j], faceList[i + 1 + (j + 1) % n]);
        i += 1 + static_cast<std::size_t>(n);
    }
    if (edges.empty())
        return false;
    geom::DisplayContext ctx;
    for (const auto& [a, b] : edges) {
        Entity e;
        e.common = base;
        e.data = LineData{verts[static_cast<std::size_t>(a)], verts[static_cast<std::size_t>(b)]};
        geom::buildDisplay(*doc_, e, ctx, proxy.graphics);
    }
    return true;
}

}  // namespace tbcad::dxf::detail
