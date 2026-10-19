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
constexpr std::uint8_t kCfAnnotative = 64;
constexpr std::uint8_t kCfExt = 128;        // an extension byte follows the owner
constexpr std::uint8_t kExtAnnoScales = 1;  // ... a list of annotation scales follows it
constexpr std::uint8_t kExtWipeout = 2;
constexpr std::uint8_t kExtTransparency = 4;  // ... one byte: percent, or 255 ByBlock
constexpr std::uint8_t kExtData = 8;          // ... XDATA and extension dictionary records
constexpr std::uint8_t kCfKnownMask = 255;

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
    void dataItems(const std::vector<DataItem>& items) {
        w.varint(items.size());
        for (const DataItem& it : items) {
            w.varint(static_cast<std::uint64_t>(it.code));
            switch (dataKindOf(it.code)) {
                case DataKind::Text:
                    str(it.text);
                    break;
                case DataKind::Integer:
                    w.i64(it.integer);
                    break;
                case DataKind::Real:
                    w.f64(it.real);
                    break;
            }
        }
    }
    void extData(const ExtData& d) {
        w.varint(d.xdata.size());
        for (const XdataApp& a : d.xdata) {
            str(a.app);
            dataItems(a.items);
        }
        w.varint(d.dict.size());
        for (const DictRecord& r : d.dict) {
            str(r.name);
            dataItems(r.items);
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
        w.u8((d.closed ? 1 : 0) | (d.restartLinetype ? 2 : 0));
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
        // Trailing field (format 1.1): attribute definition; the byte also carries the text
        // generation flags (16 backward, 32 upside down).
        const std::uint8_t gen =
            static_cast<std::uint8_t>((d.backward ? 16 : 0) | (d.upsideDown ? 32 : 0));
        if (d.attdef) {
            const AttDefInfo& a = *d.attdef;
            w.u8(static_cast<std::uint8_t>((a.invisible ? 1 : 0) | (a.constant ? 2 : 0) |
                                           (a.verify ? 4 : 0) | (a.preset ? 8 : 0) | 128 | gen));
            str(a.tag);
            str(a.prompt);
            str(a.defaultValue);
        } else if (gen) {
            w.u8(gen);
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
        if (d.columns > 1 || d.lineSpacing != 1.0 || d.bgMode) {  // a tail older readers ignore
            w.u8(d.columns);
            w.f64(d.columnGap);
            if (d.lineSpacing != 1.0 || d.bgMode)
                w.f64(d.lineSpacing);
            if (d.bgMode) {
                w.u8(d.bgMode);
                w.u8(static_cast<std::uint8_t>(d.bgColor));
                w.f64(d.bgScale);
            }
        }
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
        // Optional tail: 0xF0 marks a double pattern, then a gradient may follow.
        if (d.doubled)
            w.u8(0xF0);
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
        // Trailing fields: attribute values (format 1.1), then tagged sections: 1 = array
        // (format 1.3), 2 = dynamic block values (format 1.4), 3 = clipping boundary (format 1.6),
        // 5 = assembly values and 6 = not joined (format 1.8).
        if (!d.attribs.empty() || d.array || !d.dynValues.empty() || d.clip.size() >= 3 ||
            !d.asmValues.empty() || d.joinOff) {
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
        if (!d.dynValues.empty()) {
            w.u8(2);
            w.varint(d.dynValues.size());
            for (double v : d.dynValues)
                w.f64(v);
        }
        if (d.clip.size() >= 3) {
            w.u8(3);
            w.u8(static_cast<std::uint8_t>((d.clipOn ? 1 : 0) |
                                           (d.clipInvert ? 2 : 0)));  // bit 1: invert
            w.varint(d.clip.size());
            for (const Vec2& p : d.clip)
                w.vec2(p);
        }
        if (!d.asmValues.empty()) {
            w.u8(5);
            w.varint(d.asmValues.size());
            for (const AsmValue& v : d.asmValues) {
                str(v.name);
                w.u8(v.isText ? 1 : 0);
                w.f64(v.number);
                str(v.text);
            }
        }
        if (d.joinOff)
            w.u8(6);
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
        if (!d.breaks.empty()) {  // tag 1: DIMBREAK gaps (a file without breaks has no tail)
            w.u8(1);
            w.varint(d.breaks.size());
            for (const DimBreak& b : d.breaks) {
                w.vec2(b.a);
                w.vec2(b.b);
            }
        }
        if (d.oblique != 0 || d.textRotated) {  // tag 2: DIMEDIT Oblique and Rotate
            w.u8(2);
            w.f64(d.oblique);
            w.u8(d.textRotated ? 1 : 0);
            w.f64(d.textRotation);
        }
        if (!d.jogs.empty()) {  // tag 3: DIMJOGLINE
            w.u8(3);
            w.varint(d.jogs.size());
            for (const Vec2& p : d.jogs)
                w.vec2(p);
        }
        std::uint8_t linked = 0;
        for (std::size_t i = 0; i < d.assoc.size(); ++i)
            if (d.assoc[i].kind != DimAssoc::Kind::None)
                linked |= static_cast<std::uint8_t>(1u << i);
        if (linked) {  // tag 4: associative dimension points, one entry per set bit
            w.u8(4);
            w.u8(linked);
            for (const DimAssoc& a : d.assoc) {
                if (a.kind == DimAssoc::Kind::None)
                    continue;
                w.u8(static_cast<std::uint8_t>(a.kind));
                w.varint(a.target.value);
                w.varint(a.index);
            }
        }
    }
    void operator()(const LeaderData& d) {
        points(d.vertices);
        w.varint(d.style.value);
        str(d.utf8);
        w.vec2(d.textPos);
    }
    void operator()(const RayData& d) {
        w.vec2(d.origin);
        w.vec2(d.dir);
        w.u8(d.infinite ? 1 : 0);
    }
    void operator()(const MlineData& d) {
        str(d.style);
        w.f64(d.scale);
        w.u8(static_cast<std::uint8_t>(d.justification));
        w.u8(d.closed ? 1 : 0);
        w.varint(d.v.size());
        for (const Vec2& p : d.v)
            w.vec2(p);
        if (!d.cuts.empty()) {  // optional tail: the gaps of open tees and crosses
            w.varint(d.cuts.size());
            for (const MlineCut& c : d.cuts) {
                w.varint(c.seg);
                w.f64(c.t0);
                w.f64(c.t1);
                w.u8(c.lo);
                w.u8(c.hi);
            }
        }
    }
    void operator()(const MLeaderData& d) {
        w.varint(d.lines.size());
        for (const auto& line : d.lines)
            points(line);
        w.vec2(d.landing);
        w.vec2(d.contentPos);
        w.u8(d.dogleg ? 1 : 0);
        w.varint(d.style.value);
        w.u8(static_cast<std::uint8_t>(d.content));
        str(d.utf8);
        w.f64(d.textHeight);
        w.varint(d.block.value);
        w.f64(d.blockScale);
        w.f64(d.blockRotation);
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
    std::vector<DataItem> dataItems() {
        const std::uint64_t n = r.varint();
        if (n > (1u << 20))
            bad("too many data items");
        std::vector<DataItem> items;
        for (std::uint64_t i = 0; i < n; ++i) {
            DataItem it;
            const std::uint64_t code = r.varint();
            if (code > 5000)
                bad("invalid data group code");
            it.code = static_cast<int>(code);
            switch (dataKindOf(it.code)) {
                case DataKind::Text:
                    it.text = str();
                    break;
                case DataKind::Integer:
                    it.integer = r.i64();
                    break;
                case DataKind::Real:
                    it.real = r.f64();
                    break;
            }
            items.push_back(std::move(it));
        }
        return items;
    }
    ExtData extData() {
        ExtData d;
        const std::uint64_t apps = r.varint();
        if (apps > (1u << 16))
            bad("too many XDATA applications");
        for (std::uint64_t i = 0; i < apps; ++i) {
            XdataApp a;
            a.app = str();
            a.items = dataItems();
            d.xdata.push_back(std::move(a));
        }
        const std::uint64_t recs = r.varint();
        if (recs > (1u << 16))
            bad("too many dictionary records");
        for (std::uint64_t i = 0; i < recs; ++i) {
            DictRecord rec;
            rec.name = str();
            rec.items = dataItems();
            d.dict.push_back(std::move(rec));
        }
        return d;
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
                const std::uint8_t flags = r.u8();
                d.closed = (flags & 1) != 0;
                d.restartLinetype = (flags & 2) != 0;
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
                    d.backward = (f & 16) != 0;
                    d.upsideDown = (f & 32) != 0;
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
                if (r.remaining() > 0) {
                    d.columns = std::max<std::uint8_t>(1, r.u8());
                    d.columnGap = r.f64();
                    if (r.remaining() > 0) {
                        d.lineSpacing = r.f64();
                        if (!(d.lineSpacing >= 0.25 && d.lineSpacing <= 4.0))
                            d.lineSpacing = 1.0;
                        if (r.remaining() > 0) {
                            const std::uint8_t mode = r.u8();
                            d.bgMode = mode <= 2 ? mode : 0;
                            d.bgColor = r.u8();
                            d.bgScale = r.f64();
                            if (!(d.bgScale >= 1.0 && d.bgScale <= 5.0))
                                d.bgScale = 1.5;
                        }
                    }
                }
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
                while (r.remaining() > 0) {
                    std::uint8_t tag = r.u8();
                    if (tag == 2) {
                        std::size_t n = r.count(8);
                        d.dynValues.reserve(n);
                        for (std::size_t i = 0; i < n; ++i)
                            d.dynValues.push_back(r.f64());
                        continue;
                    }
                    if (tag == 3) {
                        const std::uint8_t flags = r.u8();
                        d.clipOn = (flags & 1) != 0;
                        d.clipInvert = (flags & 2) != 0;
                        std::size_t n = r.count(16);
                        d.clip.reserve(n);
                        for (std::size_t i = 0; i < n; ++i)
                            d.clip.push_back(r.vec2());
                        continue;
                    }
                    if (tag == 5) {
                        std::size_t n = r.count(10);
                        d.asmValues.reserve(n);
                        for (std::size_t i = 0; i < n; ++i) {
                            AsmValue v;
                            v.name = str();
                            v.isText = r.boolean();
                            v.number = r.f64();
                            v.text = str();
                            d.asmValues.push_back(std::move(v));
                        }
                        continue;
                    }
                    if (tag == 6) {
                        d.joinOff = true;
                        continue;
                    }
                    if (tag != 1)
                        bad("unknown insert section");
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
                d.kind = enumeration<DimKind>(static_cast<std::uint8_t>(DimKind::Jogged));
                d.rotation = r.f64();
                for (auto& p : d.defPts)
                    p = r.vec2();
                d.textPos = r.vec2();
                d.textPosUser = r.boolean();
                d.textOverride = str();
                d.style = id<DimStyleId>();
                d.measurement = r.f64();
                while (r.remaining() > 0) {
                    const std::uint8_t tag = r.u8();
                    if (tag == 2) {
                        d.oblique = r.f64();
                        d.textRotated = r.u8() != 0;
                        d.textRotation = r.f64();
                        continue;
                    }
                    if (tag == 3) {
                        std::size_t nj = r.count(16);
                        d.jogs.reserve(nj);
                        for (std::size_t i = 0; i < nj; ++i)
                            d.jogs.push_back(r.vec2());
                        continue;
                    }
                    if (tag == 4) {
                        const std::uint8_t linked = r.u8();
                        for (std::size_t i = 0; i < d.assoc.size(); ++i) {
                            if (!(linked & (1u << i)))
                                continue;
                            DimAssoc a;
                            a.kind = enumeration<DimAssoc::Kind>(
                                static_cast<std::uint8_t>(DimAssoc::Kind::Center));
                            a.target = id<EntityId>();
                            a.index = static_cast<std::uint32_t>(r.varint());
                            d.assoc[i] = a;
                        }
                        continue;
                    }
                    if (tag != 1)
                        break;  // a tail of a newer minor version: ignored
                    std::size_t n = r.count(32);
                    d.breaks.reserve(n);
                    for (std::size_t i = 0; i < n; ++i) {
                        DimBreak b;
                        b.a = r.vec2();
                        b.b = r.vec2();
                        d.breaks.push_back(b);
                    }
                }
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
            case EntityKind::Ray: {
                RayData d;
                d.origin = r.vec2();
                d.dir = r.vec2();
                d.infinite = r.boolean();
                return make(d);
            }
            case EntityKind::MLine: {
                MlineData d;
                d.style = str();
                d.scale = r.f64();
                d.justification = r.u8();
                d.closed = r.boolean();
                std::size_t n = r.count(16);
                d.v.resize(n);
                for (Vec2& p : d.v)
                    p = r.vec2();
                if (r.remaining() > 0) {
                    const std::size_t nc = r.count(18);
                    d.cuts.resize(nc);
                    for (MlineCut& c : d.cuts) {
                        c.seg = static_cast<std::uint32_t>(r.varint());
                        c.t0 = r.f64();
                        c.t1 = r.f64();
                        c.lo = r.u8();
                        c.hi = r.u8();
                    }
                }
                return make(std::move(d));
            }
            case EntityKind::MLeader: {
                MLeaderData d;
                std::size_t n = r.count(1);
                d.lines.resize(n);
                for (auto& line : d.lines)
                    line = points();
                d.landing = r.vec2();
                d.contentPos = r.vec2();
                d.dogleg = r.boolean();
                d.style = id<DimStyleId>();
                d.content = enumeration<MLeaderData::Content>(2);
                d.utf8 = str();
                d.textHeight = r.f64();
                d.block = id<BlockId>();
                d.blockScale = r.f64();
                d.blockRotation = r.f64();
                return make(std::move(d));
            }
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
            if (k == 0xF0) {  // a double pattern; a gradient may still follow
                d.doubled = true;
                k = r.remaining() > 0 ? r.u8() : std::uint8_t{0};
            }
            if (k != 0) {
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

constexpr std::uint16_t kMaxKnownKind = static_cast<std::uint16_t>(EntityKind::MLeader);

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
    if (c.annotative)
        flags |= kCfAnnotative;
    std::uint8_t ext = 0;
    if (c.annotative && !c.annoScales.empty())
        ext |= kExtAnnoScales;
    if (c.wipeout)
        ext |= kExtWipeout;
    if (c.transparency != kTransparencyByLayer)
        ext |= kExtTransparency;
    if (!c.ext.empty())
        ext |= kExtData;
    if (ext)
        flags |= kCfExt;
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
    if (flags & kCfExt) {
        w.u8(ext);
        if (ext & kExtTransparency)
            w.u8(c.transparency == kTransparencyByBlock
                     ? 255
                     : static_cast<std::uint8_t>(c.transparency));
        if (ext & kExtAnnoScales) {
            w.varint(c.annoScales.size());
            for (double v : c.annoScales)
                w.f64(v);
        }
        if (ext & kExtData) {
            PayloadWriter pw{w, strings};
            pw.extData(c.ext.get());
        }
    }

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
        c.linetypeScale = static_cast<float>(r.f64());
    c.visible = !(flags & kCfHidden);
    c.annotative = (flags & kCfAnnotative) != 0;
    if (flags & kCfOwner)
        out.owner = pr.id<BlockId>();
    if (flags & kCfExt) {
        const std::uint8_t ext = r.u8();
        if (ext & ~(kExtAnnoScales | kExtWipeout | kExtTransparency | kExtData))
            bad("unknown entity extension flags");
        c.wipeout = (ext & kExtWipeout) != 0;
        if (ext & kExtTransparency) {
            const std::uint8_t t = r.u8();
            if (t == 255)
                c.transparency = kTransparencyByBlock;
            else if (t <= kTransparencyMax)
                c.transparency = static_cast<std::int8_t>(t);
            else
                bad("invalid transparency");
        }
        if (ext & kExtAnnoScales) {
            const std::uint64_t n = r.varint();
            if (n > 64)
                bad("too many annotation scales");
            std::vector<double> found;
            for (std::uint64_t i = 0; i < n; ++i) {
                double v = r.f64();
                if (std::isfinite(v) && v > 0)
                    found.push_back(v);
            }
            c.annoScales = ScaleList(std::move(found));
        }
        if (ext & kExtData)
            c.ext = ExtDataPtr(pr.extData());
    }
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
