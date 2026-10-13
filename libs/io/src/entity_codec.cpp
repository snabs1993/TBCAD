#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

#include "format.h"

namespace tbcad::io::detail {

namespace {

// Common property flags.
constexpr std::uint8_t kCfLinetype = 1;
constexpr std::uint8_t kCfColor = 2;
constexpr std::uint8_t kCfLineweight = 4;
constexpr std::uint8_t kCfLtScale = 8;
constexpr std::uint8_t kCfHidden = 16;
constexpr std::uint8_t kCfOwner = 32;
constexpr std::uint8_t kCfKnownMask = 63;

// ---------------------------------------------------------------- writing

struct PayloadWriter {
    ByteWriter& w;
    StringTable& strings;

    void str(const std::string& s) { w.varint(strings.intern(s)); }

    void polyVertices(const std::vector<PolyVertex>& v) {
        w.varint(v.size());
        for (const auto& p : v) {
            w.vec2(p.p);
            w.f64(p.bulge);
            w.f64(p.startWidth);
            w.f64(p.endWidth);
        }
    }
    void doubles(const std::vector<double>& d) {
        w.varint(d.size());
        for (double x : d)
            w.f64(x);
    }
    void points(const std::vector<Vec2>& d) {
        w.varint(d.size());
        for (const auto& x : d)
            w.vec2(x);
    }
    void spline(const SplineData& d) {
        w.i32(d.degree);
        w.u8(static_cast<std::uint8_t>((d.closed ? 1 : 0) | (d.periodic ? 2 : 0)));
        doubles(d.knots);
        points(d.control);
        doubles(d.weights);
        points(d.fit);
    }
    void style(const DisplayStyle& s) {
        w.u8(s.color.r);
        w.u8(s.color.g);
        w.u8(s.color.b);
        w.u8(s.color.a);
        w.f64(s.lineweight);
        w.f64(s.patternScale);
        w.varint(s.layer.value);
    }

    void operator()(const LineData& d) {
        w.vec2(d.a);
        w.vec2(d.b);
    }
    void operator()(const CircleData& d) {
        w.vec2(d.center);
        w.f64(d.radius);
    }
    void operator()(const ArcData& d) {
        w.vec2(d.center);
        w.f64(d.radius);
        w.f64(d.startAngle);
        w.f64(d.endAngle);
    }
    void operator()(const PolylineData& d) {
        w.u8(d.closed ? 1 : 0);
        w.f64(d.constantWidth);
        polyVertices(d.v);
    }
    void operator()(const PointData& d) { w.vec2(d.p); }
    void operator()(const EllipseData& d) {
        w.vec2(d.center);
        w.vec2(d.majorAxis);
        w.f64(d.ratio);
        w.f64(d.startParam);
        w.f64(d.endParam);
    }
    void operator()(const SplineData& d) { spline(d); }
    void operator()(const TextData& d) {
        w.vec2(d.position);
        w.vec2(d.alignPoint);
        w.f64(d.height);
        w.f64(d.rotation);
        w.f64(d.widthFactor);
        w.f64(d.oblique);
        w.varint(d.style.value);
        w.u8(static_cast<std::uint8_t>(d.h));
        w.u8(static_cast<std::uint8_t>(d.v));
        str(d.utf8);
        // Trailing field (format 1.1): attribute definition.
        if (d.attdef) {
            const AttDefInfo& a = *d.attdef;
            w.u8(static_cast<std::uint8_t>((a.invisible ? 1 : 0) | (a.constant ? 2 : 0) |
                                           (a.verify ? 4 : 0) | (a.preset ? 8 : 0) | 128));
            str(a.tag);
            str(a.prompt);
            str(a.defaultValue);
        }
    }
    void operator()(const MTextData& d) {
        w.vec2(d.position);
        w.f64(d.height);
        w.f64(d.width);
        w.f64(d.rotation);
        w.u8(static_cast<std::uint8_t>(d.attach));
        w.varint(d.style.value);
        str(d.utf8);
    }
    void operator()(const HatchData& d) {
        w.u8(d.solid ? 1 : 0);
        str(d.pattern);
        w.f64(d.patternScale);
        w.f64(d.patternAngle);
        w.vec2(d.patternOrigin);
        color(d.fill);
        w.varint(d.sourceBoundary.size());
        for (EntityId e : d.sourceBoundary)
            w.varint(e.value);
        w.varint(d.loops.size());
        for (const auto& loop : d.loops) {
            w.u8(loop.outer ? 1 : 0);
            w.varint(loop.edges.size());
            for (const auto& edge : loop.edges)
                std::visit([&](const auto& e) { hatchEdge(e); }, edge);
        }
        // A gradient is appended after the loops; readers that do not know it skip the bytes.
        if (d.gradient) {
            w.u8(static_cast<std::uint8_t>(d.gradient->kind));
            color(d.gradient->color1);
            color(d.gradient->color2);
            w.f64(d.gradient->angle);
        }
    }
    void hatchEdge(const HatchLineEdge& e) {
        w.u8(0);
        w.vec2(e.a);
        w.vec2(e.b);
    }
    void hatchEdge(const HatchArcEdge& e) {
        w.u8(1);
        w.vec2(e.center);
        w.f64(e.radius);
        w.f64(e.startAngle);
        w.f64(e.endAngle);
        w.u8(e.ccw ? 1 : 0);
    }
    void hatchEdge(const HatchEllipseEdge& e) {
        w.u8(2);
        w.vec2(e.center);
        w.vec2(e.majorAxis);
        w.f64(e.ratio);
        w.f64(e.startParam);
        w.f64(e.endParam);
        w.u8(e.ccw ? 1 : 0);
    }
    void hatchEdge(const HatchSplineEdge& e) {
        w.u8(3);
        spline(e.spline);
    }
    void hatchEdge(const HatchPolylineEdge& e) {
        w.u8(4);
        w.u8(e.closed ? 1 : 0);
        polyVertices(e.v);
    }
    void operator()(const InsertData& d) {
        w.varint(d.block.value);
        w.vec2(d.position);
        w.vec2(d.scale);
        w.f64(d.rotation);
        // Trailing fields: attribute values (format 1.1), then an array (format 1.3).
        if (!d.attribs.empty() || d.array) {
            w.varint(d.attribs.size());
            for (const AttribValue& a : d.attribs) {
                str(a.tag);
                str(a.value);
            }
        }
        if (d.array) {
            const ArrayInfo& a = *d.array;
            w.u8(1);
            w.u8(static_cast<std::uint8_t>(a.kind));
            w.varint(static_cast<std::uint64_t>(std::max(a.rows, 0)));
            w.varint(static_cast<std::uint64_t>(std::max(a.columns, 0)));
            w.f64(a.rowSpacing);
            w.f64(a.columnSpacing);
            w.f64(a.angle);
            w.vec2(a.center);
            w.varint(static_cast<std::uint64_t>(std::max(a.count, 0)));
            w.f64(a.fillAngle);
            w.u8(a.rotateItems ? 1 : 0);
            w.vec2(a.reference);
            w.u8(a.pathClosed ? 1 : 0);
            w.f64(a.pathSpacing);
            polyVertices(a.path);
        }
    }
    void operator()(const DimensionData& d) {
        w.u8(static_cast<std::uint8_t>(d.kind));
        w.f64(d.rotation);
        for (const auto& p : d.defPts)
            w.vec2(p);
        w.vec2(d.textPos);
        w.u8(d.textPosUser ? 1 : 0);
        str(d.textOverride);
        w.varint(d.style.value);
        w.f64(d.measurement);
    }
    void operator()(const LeaderData& d) {
        points(d.vertices);
        w.varint(d.style.value);
        str(d.utf8);
        w.vec2(d.textPos);
    }
    void operator()(const TableData& d) {
        w.vec2(d.origin);
        w.f64(d.rotation);
        w.f64(d.textHeight);
        w.f64(d.margin);
        w.varint(d.style.value);
        w.u8(d.titleRow ? 1 : 0);
        w.varint(d.colWidths.size());
        for (double x : d.colWidths)
            w.f64(x);
        w.varint(d.rowHeights.size());
        for (double y : d.rowHeights)
            w.f64(y);
        w.varint(d.cells.size());
        for (const TableCell& c : d.cells) {
            str(c.utf8);
            w.varint(c.colSpan);
            w.varint(c.rowSpan);
            w.u8(c.covered ? 1 : 0);
        }
    }
    void operator()(const ProxyData& d) {
        str(d.originalType);
        w.varint(d.raw.size());
        w.raw(d.raw);
        w.varint(d.graphics.items.size());
        for (const auto& item : d.graphics.items) {
            std::visit(
                [&](const auto& it) {
                    using I = std::decay_t<decltype(it)>;
                    if constexpr (std::is_same_v<I, DisplayPolyline>) {
                        w.u8(0);
                        w.u8(it.closed ? 1 : 0);
                        points(it.pts);
                    } else if constexpr (std::is_same_v<I, DisplayFill>) {
                        w.u8(1);
                        w.varint(it.rings.size());
                        for (const auto& ring : it.rings)
                            points(ring);
                    } else {
                        w.u8(2);
                        points(it.pts);
                    }
                    style(it.style);
                },
                item);
        }
    }

    void color(const Color& c) {
        w.u8(static_cast<std::uint8_t>(c.kind));
        w.u8(c.r);
        w.u8(c.g);
        w.u8(c.b);
    }
};

// ---------------------------------------------------------------- reading

[[noreturn]] void bad(const char* msg) {
    throw FormatError(msg);
}

struct PayloadReader {
    ByteReader& r;
    const std::vector<std::string>& strings;

    std::string str() {
        std::uint64_t i = r.varint();
        if (i >= strings.size())
            bad("string index out of range");
        return strings[static_cast<std::size_t>(i)];
    }
    template <class IdT>
    IdT id() {
        std::uint64_t v = r.varint();
        if (v > std::numeric_limits<decltype(IdT::value)>::max())
            bad("id out of range");
        return IdT{static_cast<decltype(IdT::value)>(v)};
    }
    template <class E>
    E enumeration(std::uint8_t maxValue) {
        std::uint8_t v = r.u8();
        if (v > maxValue)
            bad("enumeration value out of range");
        return static_cast<E>(v);
    }
    // Item counts of an array are bounded so a damaged file cannot ask for billions of copies.
    int arrayCount() {
        std::uint64_t n = r.varint();
        if (n > 1000000)
            bad("array item count out of range");
        return static_cast<int>(n);
    }
    std::vector<PolyVertex> polyVertices() {
        std::size_t n = r.count(40);
        std::vector<PolyVertex> v(n);
        for (auto& p : v) {
            p.p = r.vec2();
            p.bulge = r.f64();
            p.startWidth = r.f64();
            p.endWidth = r.f64();
        }
        return v;
    }
    std::vector<double> doubles() {
        std::size_t n = r.count(8);
        std::vector<double> d(n);
        for (auto& x : d)
            x = r.f64();
        return d;
    }
    std::vector<Vec2> points() {
        std::size_t n = r.count(16);
        std::vector<Vec2> d(n);
        for (auto& x : d)
            x = r.vec2();
        return d;
    }
    SplineData spline() {
        SplineData d;
        d.degree = r.i32();
        std::uint8_t f = r.u8();
        if (f > 3)
            bad("invalid spline flags");
        d.closed = (f & 1) != 0;
        d.periodic = (f & 2) != 0;
        d.knots = doubles();
        d.control = points();
        d.weights = doubles();
        d.fit = points();
        return d;
    }
    Color color() {
        Color c;
        c.kind = enumeration<ColorKind>(static_cast<std::uint8_t>(ColorKind::ByParent));
        c.r = r.u8();
        c.g = r.u8();
        c.b = r.u8();
        return c;
    }
    DisplayStyle style() {
        DisplayStyle s;
        s.color.r = r.u8();
        s.color.g = r.u8();
        s.color.b = r.u8();
        s.color.a = r.u8();
        s.lineweight = r.f64();
        s.patternScale = r.f64();
        s.layer = id<LayerId>();
        return s;
    }

    Entity line() {
        LineData d;
        d.a = r.vec2();
        d.b = r.vec2();
        return make(d);
    }
    template <class T>
    static Entity make(T d) {
        Entity e;
        e.data = std::move(d);
        return e;
    }

    Entity read(EntityKind kind) {
        switch (kind) {
            case EntityKind::Line:
                return line();
            case EntityKind::Circle: {
                CircleData d;
                d.center = r.vec2();
                d.radius = r.f64();
                return make(d);
            }
            case EntityKind::Arc: {
                ArcData d;
                d.center = r.vec2();
                d.radius = r.f64();
                d.startAngle = r.f64();
                d.endAngle = r.f64();
                return make(d);
            }
            case EntityKind::Polyline: {
                PolylineData d;
                d.closed = r.boolean();
                d.constantWidth = r.f64();
                d.v = polyVertices();
                return make(std::move(d));
            }
            case EntityKind::Point: {
                PointData d;
                d.p = r.vec2();
                return make(d);
            }
            case EntityKind::Ellipse: {
                EllipseData d;
                d.center = r.vec2();
                d.majorAxis = r.vec2();
                d.ratio = r.f64();
                d.startParam = r.f64();
                d.endParam = r.f64();
                return make(d);
            }
            case EntityKind::Spline:
                return make(spline());
            case EntityKind::Text: {
                TextData d;
                d.position = r.vec2();
                d.alignPoint = r.vec2();
                d.height = r.f64();
                d.rotation = r.f64();
                d.widthFactor = r.f64();
                d.oblique = r.f64();
                d.style = id<TextStyleId>();
                d.h = enumeration<HAlign>(static_cast<std::uint8_t>(HAlign::Fit));
                d.v = enumeration<VAlign>(static_cast<std::uint8_t>(VAlign::Top));
                d.utf8 = str();
                if (r.remaining() > 0) {
                    std::uint8_t f = r.u8();
                    if (f & 128) {
                        AttDefInfo a;
                        a.invisible = f & 1;
                        a.constant = f & 2;
                        a.verify = f & 4;
                        a.preset = f & 8;
                        a.tag = str();
                        a.prompt = str();
                        a.defaultValue = str();
                        d.attdef = std::move(a);
                    }
                }
                return make(std::move(d));
            }
            case EntityKind::MText: {
                MTextData d;
                d.position = r.vec2();
                d.height = r.f64();
                d.width = r.f64();
                d.rotation = r.f64();
                d.attach =
                    enumeration<Attachment>(static_cast<std::uint8_t>(Attachment::BottomRight));
                d.style = id<TextStyleId>();
                d.utf8 = str();
                return make(std::move(d));
            }
            case EntityKind::Hatch:
                return hatch();
            case EntityKind::Insert: {
                InsertData d;
                d.block = id<BlockId>();
                d.position = r.vec2();
                d.scale = r.vec2();
                d.rotation = r.f64();
                if (r.remaining() > 0) {
                    std::size_t n = r.count(2);
                    d.attribs.reserve(n);
                    for (std::size_t i = 0; i < n; ++i) {
                        AttribValue a;
                        a.tag = str();
                        a.value = str();
                        d.attribs.push_back(std::move(a));
                    }
                }
                if (r.remaining() > 0 && r.u8() == 1) {
                    ArrayInfo a;
                    a.kind = enumeration<ArrayInfo::Kind>(
                        static_cast<std::uint8_t>(ArrayInfo::Kind::Path));
                    a.rows = arrayCount();
                    a.columns = arrayCount();
                    a.rowSpacing = r.f64();
                    a.columnSpacing = r.f64();
                    a.angle = r.f64();
                    a.center = r.vec2();
                    a.count = arrayCount();
                    a.fillAngle = r.f64();
                    a.rotateItems = r.boolean();
                    a.reference = r.vec2();
                    a.pathClosed = r.boolean();
                    a.pathSpacing = r.f64();
                    a.path = polyVertices();
                    d.array = std::move(a);
                }
                return make(std::move(d));
            }
            case EntityKind::Dimension: {
                DimensionData d;
                d.kind = enumeration<DimKind>(static_cast<std::uint8_t>(DimKind::ArcLength));
                d.rotation = r.f64();
                for (auto& p : d.defPts)
                    p = r.vec2();
                d.textPos = r.vec2();
                d.textPosUser = r.boolean();
                d.textOverride = str();
                d.style = id<DimStyleId>();
                d.measurement = r.f64();
                return make(std::move(d));
            }
            case EntityKind::Leader: {
                LeaderData d;
                d.vertices = points();
                d.style = id<DimStyleId>();
                d.utf8 = str();
                d.textPos = r.vec2();
                return make(std::move(d));
            }
            case EntityKind::Proxy:
                return proxy();
            case EntityKind::Table: {
                TableData d;
                d.origin = r.vec2();
                d.rotation = r.f64();
                d.textHeight = r.f64();
                d.margin = r.f64();
                d.style = id<TextStyleId>();
                d.titleRow = r.boolean();
                std::size_t nc = r.count(8);
                d.colWidths.resize(nc);
                for (double& x : d.colWidths)
                    x = r.f64();
                std::size_t nr = r.count(8);
                d.rowHeights.resize(nr);
                for (double& y : d.rowHeights)
                    y = r.f64();
                std::size_t n = r.count(4);
                if (nc == 0 || nr == 0 || n != nc * nr)
                    bad("table cell count does not match its rows and columns");
                d.cells.resize(n);
                for (TableCell& c : d.cells) {
                    c.utf8 = str();
                    c.colSpan =
                        static_cast<std::uint16_t>(std::min<std::uint64_t>(r.varint(), 65535));
                    c.rowSpan =
                        static_cast<std::uint16_t>(std::min<std::uint64_t>(r.varint(), 65535));
                    c.covered = r.boolean();
                }
                return make(std::move(d));
            }
        }
        bad("unknown entity kind");
    }

    Entity hatch() {
        HatchData d;
        d.solid = r.boolean();
        d.pattern = str();
        d.patternScale = r.f64();
        d.patternAngle = r.f64();
        d.patternOrigin = r.vec2();
        d.fill = color();
        std::size_t ns = r.count(1);
        d.sourceBoundary.reserve(ns);
        for (std::size_t i = 0; i < ns; ++i)
            d.sourceBoundary.push_back(EntityId{r.varint()});
        std::size_t nl = r.count(2);
        d.loops.resize(nl);
        for (auto& loop : d.loops) {
            loop.outer = r.boolean();
            std::size_t ne = r.count(1);
            loop.edges.reserve(ne);
            for (std::size_t i = 0; i < ne; ++i) {
                switch (r.u8()) {
                    case 0: {
                        HatchLineEdge e;
                        e.a = r.vec2();
                        e.b = r.vec2();
                        loop.edges.emplace_back(e);
                        break;
                    }
                    case 1: {
                        HatchArcEdge e;
                        e.center = r.vec2();
                        e.radius = r.f64();
                        e.startAngle = r.f64();
                        e.endAngle = r.f64();
                        e.ccw = r.boolean();
                        loop.edges.emplace_back(e);
                        break;
                    }
                    case 2: {
                        HatchEllipseEdge e;
                        e.center = r.vec2();
                        e.majorAxis = r.vec2();
                        e.ratio = r.f64();
                        e.startParam = r.f64();
                        e.endParam = r.f64();
                        e.ccw = r.boolean();
                        loop.edges.emplace_back(e);
                        break;
                    }
                    case 3: {
                        HatchSplineEdge e;
                        e.spline = spline();
                        loop.edges.emplace_back(std::move(e));
                        break;
                    }
                    case 4: {
                        HatchPolylineEdge e;
                        e.closed = r.boolean();
                        e.v = polyVertices();
                        loop.edges.emplace_back(std::move(e));
                        break;
                    }
                    default:
                        bad("unknown hatch edge type");
                }
            }
        }
        if (r.remaining() > 0) {
            std::uint8_t k = r.u8();
            if (k != static_cast<std::uint8_t>(GradientKind::Linear) &&
                k != static_cast<std::uint8_t>(GradientKind::Cylinder))
                bad("unknown gradient kind");
            GradientData g;
            g.kind = static_cast<GradientKind>(k);
            g.color1 = color();
            g.color2 = color();
            g.angle = r.f64();
            d.gradient = g;
        }
        return make(std::move(d));
    }

    Entity proxy() {
        ProxyData d;
        d.originalType = str();
        std::size_t n = r.count(1);
        auto raw = r.bytes(n);
        d.raw.assign(raw.begin(), raw.end());
        std::size_t items = r.count(2);
        d.graphics.items.reserve(items);
        for (std::size_t i = 0; i < items; ++i) {
            switch (r.u8()) {
                case 0: {
                    DisplayPolyline it;
                    it.closed = r.boolean();
                    it.pts = points();
                    it.style = style();
                    d.graphics.items.emplace_back(std::move(it));
                    break;
                }
                case 1: {
                    DisplayFill it;
                    std::size_t rings = r.count(1);
                    it.rings.reserve(rings);
                    for (std::size_t k = 0; k < rings; ++k)
                        it.rings.push_back(points());
                    it.style = style();
                    d.graphics.items.emplace_back(std::move(it));
                    break;
                }
                case 2: {
                    DisplayPoints it;
                    it.pts = points();
                    it.style = style();
                    d.graphics.items.emplace_back(std::move(it));
                    break;
                }
                default:
                    bad("unknown display item type");
            }
        }
        return make(std::move(d));
    }
};

constexpr std::uint16_t kMaxKnownKind = static_cast<std::uint16_t>(EntityKind::Table);

}  // namespace

void encodeEntity(ByteWriter& w, StringTable& strings, const Entity& e, BlockId owner) {
    // Proxies that stand for a kind this build did not know are written back as that kind.
    const ProxyData* unknown = nullptr;
    if (const auto* p = entityAs<ProxyData>(e);
        p && p->originalType.starts_with(kUnknownKindPrefix)) {
        unknown = p;
    }
    std::uint16_t kindNo = static_cast<std::uint16_t>(kind(e));
    std::uint16_t version = kRecordVersion;
    if (unknown) {
        // "<prefix><kind>:<version>"
        std::string_view rest =
            std::string_view(unknown->originalType).substr(kUnknownKindPrefix.size());
        auto colon = rest.find(':');
        kindNo = static_cast<std::uint16_t>(std::stoul(std::string(rest.substr(0, colon))));
        version = static_cast<std::uint16_t>(std::stoul(std::string(rest.substr(colon + 1))));
    }
    w.u16(kindNo);
    w.u16(version);
    std::size_t lenAt = w.size();
    w.u32(0);
    std::size_t bodyStart = w.size();

    const EntityCommon& c = e.common;
    std::uint8_t flags = 0;
    if (c.linetype != kLinetypeByLayer)
        flags |= kCfLinetype;
    if (!(c.color == Color::byLayer()))
        flags |= kCfColor;
    if (!c.lineweight.isByLayer())
        flags |= kCfLineweight;
    if (c.linetypeScale != 1)
        flags |= kCfLtScale;
    if (!c.visible)
        flags |= kCfHidden;
    if (owner)
        flags |= kCfOwner;
    w.varint(c.id.value);
    w.u8(flags);
    w.varint(c.layer.value);
    if (flags & kCfLinetype)
        w.u32(c.linetype.value);
    if (flags & kCfColor) {
        w.u8(static_cast<std::uint8_t>(c.color.kind));
        w.u8(c.color.r);
        w.u8(c.color.g);
        w.u8(c.color.b);
    }
    if (flags & kCfLineweight)
        w.i16(c.lineweight.v);
    if (flags & kCfLtScale)
        w.f64(c.linetypeScale);
    if (flags & kCfOwner)
        w.varint(owner.value);

    if (unknown) {
        w.raw(unknown->raw);
    } else {
        PayloadWriter pw{w, strings};
        visitEntity(e, pw);
    }
    w.patchU32(lenAt, static_cast<std::uint32_t>(w.size() - bodyStart));
}

DecodedEntity decodeEntity(ByteReader& rec, const std::vector<std::string>& strings) {
    std::uint16_t kindNo = rec.u16();
    std::uint16_t version = rec.u16();
    std::uint32_t length = rec.u32();
    ByteReader r(rec.bytes(length));

    DecodedEntity out;
    EntityCommon& c = out.entity.common;
    PayloadReader pr{r, strings};
    c.id = EntityId{r.varint()};
    std::uint8_t flags = r.u8();
    if (flags & ~kCfKnownMask)
        bad("unknown entity property flags");
    c.layer = pr.id<LayerId>();
    if (flags & kCfLinetype)
        c.linetype = LinetypeId{r.u32()};
    if (flags & kCfColor)
        c.color = pr.color();
    if (flags & kCfLineweight)
        c.lineweight = LineWeight{r.i16()};
    if (flags & kCfLtScale)
        c.linetypeScale = r.f64();
    c.visible = !(flags & kCfHidden);
    if (flags & kCfOwner)
        out.owner = pr.id<BlockId>();
    if (!c.id)
        bad("entity id 0");

    if (kindNo > kMaxKnownKind || version == 0 || version > kRecordVersion) {
        out.unknownKind = true;
        ProxyData p;
        p.originalType = std::string(kUnknownKindPrefix) + std::to_string(kindNo) + ":" +
                         std::to_string(version);
        auto rest = r.rest();
        p.raw.assign(rest.begin(), rest.end());
        Entity e;
        e.common = c;
        e.data = Indirect<ProxyData>(std::move(p));
        out.entity = std::move(e);
        return out;
    }
    EntityCommon common = c;
    out.entity = pr.read(static_cast<EntityKind>(kindNo));
    out.entity.common = common;
    out.trailingBytes = r.remaining();
    return out;
}

}  // namespace tbcad::io::detail
