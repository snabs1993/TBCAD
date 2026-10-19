#include "tbcad/io/delta.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

#include "bytes.h"
#include "format.h"

namespace tbcad::io {

using namespace detail;

namespace {

constexpr std::uint8_t kDeltaMagic[4] = {'T', 'B', 'D', 'L'};
// 3: text style rows carry the fixed height. Version 2 records are still read.
constexpr std::uint8_t kDeltaVersion = 11;

[[noreturn]] void bad(const char* msg) {
    throw FormatError(msg);
}

bool isOpaqueKey(const std::string& k) {
    return k.starts_with(kOpaqueChunkPrefix);
}

// ---- writing -------------------------------------------------------------------------------

void putColor(ByteWriter& w, const Color& c) {
    w.u8(static_cast<std::uint8_t>(c.kind));
    w.u8(c.r);
    w.u8(c.g);
    w.u8(c.b);
}

void putString(ByteWriter& w, StringTable& st, const std::string& s) {
    w.varint(st.intern(s));
}

void putRow(ByteWriter& w, StringTable& st, const Layer& r) {
    putString(w, st, r.name);
    w.varint(r.parent.value);
    putColor(w, r.color);
    w.u32(r.linetype.value);
    w.i16(r.lineweight.v);
    w.u8(static_cast<std::uint8_t>((r.on ? 1 : 0) | (r.frozen ? 2 : 0) | (r.locked ? 4 : 0) |
                                   (r.plot ? 8 : 0) | (r.transparency ? 16 : 0) |
                                   (r.description.empty() ? 0 : 32)));
    if (r.transparency)
        w.u8(r.transparency);
    if (!r.description.empty())
        putString(w, st, r.description);
}

void putRow(ByteWriter& w, StringTable& st, const Level& r) {
    putString(w, st, r.name);
    w.f64(r.elevation);
    w.u8(static_cast<std::uint8_t>((r.visible ? 1 : 0) | (r.locked ? 2 : 0)));
}

void putRow(ByteWriter& w, StringTable& st, const Linetype& r) {
    putString(w, st, r.name);
    putString(w, st, r.description);
    w.varint(r.pattern.size());
    for (double d : r.pattern)
        w.f64(d);
}

void putRow(ByteWriter& w, StringTable& st, const TextStyle& r) {
    putString(w, st, r.name);
    putString(w, st, r.fontFamily);
    putString(w, st, r.fontFile);
    w.f64(r.widthFactor);
    w.f64(r.oblique);
    w.f64(r.height);
    // (version 5) flags: annotative, backward, upside down
    w.u8((r.annotative ? 1 : 0) | (r.backward ? 2 : 0) | (r.upsideDown ? 4 : 0));
}

void putRow(ByteWriter& w, StringTable& st, const DimStyle& r) {
    putString(w, st, r.name);
    w.f64(r.textHeight);
    w.varint(r.textStyle.value);
    w.f64(r.textGap);
    w.u8(static_cast<std::uint8_t>(r.arrowType));
    w.f64(r.arrowSize);
    w.f64(r.extLineOffset);
    w.f64(r.extLineExtension);
    w.f64(r.dimLineExtension);
    w.u8(static_cast<std::uint8_t>(r.textPlacement));
    w.u8(static_cast<std::uint8_t>(r.textAlign));
    w.u8(static_cast<std::uint8_t>(r.units));
    w.i32(r.precision);
    putString(w, st, r.prefix);
    putString(w, st, r.suffix);
    w.f64(r.roundOff);
    w.f64(r.overallScale);
    w.u8(static_cast<std::uint8_t>((r.suppressLeadingZeros ? 1 : 0) |
                                   (r.suppressTrailingZeros ? 2 : 0)));
    putColor(w, r.dimLineColor);
    putColor(w, r.extLineColor);
    putColor(w, r.textColor);
    w.u8(r.altUnits ? 1 : 0);
    w.f64(r.altScale);
    w.u8(static_cast<std::uint8_t>(r.altUnit));
    w.i32(r.altPrecision);
    putString(w, st, r.altPrefix);
    putString(w, st, r.altSuffix);
    w.u8(static_cast<std::uint8_t>(r.tolerance));
    w.f64(r.tolUpper);
    w.f64(r.tolLower);
    w.i32(r.tolPrecision);
    // (version 6) linear factor, fit, text movement
    w.f64(r.linearFactor);
    w.u8(static_cast<std::uint8_t>(r.fit));
    w.u8(static_cast<std::uint8_t>(r.textMove));
    // (version 7) text and line rules
    w.u8(packDimRules(r));
    w.u8(static_cast<std::uint8_t>(r.textJustify));
    // (version 8) number format
    w.u8(static_cast<std::uint8_t>(r.angleFormat));
    w.u8(static_cast<std::uint8_t>(r.angularPrecision + 1));
    w.u8(r.commaDecimal ? 1 : 0);
    // (version 9) baseline spacing, centre mark
    w.f64(r.baselineSpacing);
    w.f64(r.centerMark);
    // (version 10) text fill and frame
    w.u8(static_cast<std::uint8_t>(r.textFill));
    w.u8(static_cast<std::uint8_t>(r.textFillColor));
    w.u8(r.textFrame ? 1 : 0);
    // (version 11) length format
    w.u8(static_cast<std::uint8_t>(r.linearFormat));
}

void putRow(ByteWriter& w, StringTable& st, const BlockDef& r) {
    putString(w, st, r.name);
    w.vec2(r.base);
    w.u8(r.anonymous ? 1 : 0);
    w.varint(r.entities.size());
    for (EntityId e : r.entities)
        w.varint(e.value);
    putString(w, st, r.xrefPath);
    w.i64(r.xrefStamp);
}

template <class Row>
void putRows(ByteWriter& w, StringTable& st, const std::vector<RowDelta<Row>>& rows) {
    w.varint(rows.size());
    for (const auto& r : rows) {
        w.varint(r.id);
        w.u8(r.after ? 1 : 0);
        if (r.after)
            putRow(w, st, *r.after);
    }
}

void putHeader(ByteWriter& w, StringTable& st, const Header& h) {
    w.u8(static_cast<std::uint8_t>(h.units));
    w.f64(h.workArea);
    w.f64(h.ltScale);
    w.varint(h.currentLayer.value);
    w.varint(h.currentLevel.value);
    w.u8(static_cast<std::uint8_t>(h.levelView));
    putColor(w, h.currentColor);
    w.u32(h.currentLinetype.value);
    w.i16(h.currentLineweight.v);
    w.u8(static_cast<std::uint8_t>(h.currentTransparency));
    w.varint(h.currentTextStyle.value);
    w.varint(h.currentDimStyle.value);
    putString(w, st, h.createdBy);
    std::size_t n = 0;
    for (const auto& kv : h.custom)
        n += isOpaqueKey(kv.first) ? 0 : 1;
    w.varint(n);
    for (const auto& [k, v] : h.custom) {
        if (isOpaqueKey(k))
            continue;
        putString(w, st, k);
        putString(w, st, v);
    }
}

// ---- reading -------------------------------------------------------------------------------

struct Reader {
    ByteReader& r;
    const std::vector<std::string>& strings;
    const DeltaLimits& limits;
    std::uint8_t version = kDeltaVersion;

    const std::string& str() {
        std::uint64_t i = r.varint();
        if (i >= strings.size())
            bad("string index out of range");
        return strings[static_cast<std::size_t>(i)];
    }
    std::uint32_t id32() {
        std::uint64_t v = r.varint();
        if (v > std::numeric_limits<std::uint32_t>::max())
            bad("id out of range");
        return static_cast<std::uint32_t>(v);
    }
    Color color() {
        Color c;
        std::uint8_t k = r.u8();
        if (k > static_cast<std::uint8_t>(ColorKind::ByParent))
            bad("invalid colour kind");
        c.kind = static_cast<ColorKind>(k);
        c.r = r.u8();
        c.g = r.u8();
        c.b = r.u8();
        return c;
    }
    template <class E>
    E enumeration(E last) {
        std::uint8_t v = r.u8();
        if (v > static_cast<std::uint8_t>(last))
            bad("enumeration value out of range");
        return static_cast<E>(v);
    }

    void row(Layer& l) {
        l.name = str();
        l.parent = LayerId{id32()};
        l.color = color();
        l.linetype = LinetypeId{r.u32()};
        l.lineweight = LineWeight{r.i16()};
        std::uint8_t f = r.u8();
        if (f > 63)
            bad("invalid layer flags");
        l.on = (f & 1) != 0;
        l.frozen = (f & 2) != 0;
        l.locked = (f & 4) != 0;
        l.plot = (f & 8) != 0;
        if (f & 16) {
            l.transparency = r.u8();
            if (l.transparency > kTransparencyMax)
                bad("invalid layer transparency");
        }
        if (f & 32)
            l.description = str();
    }
    void row(Level& l) {
        l.name = str();
        l.elevation = r.f64();
        std::uint8_t f = r.u8();
        if (f > 3)
            bad("invalid level flags");
        l.visible = (f & 1) != 0;
        l.locked = (f & 2) != 0;
    }
    void row(Linetype& l) {
        l.name = str();
        l.description = str();
        std::size_t n = r.count(8);
        l.pattern.resize(n);
        for (double& d : l.pattern)
            d = r.f64();
    }
    void row(TextStyle& t) {
        t.name = str();
        t.fontFamily = str();
        t.fontFile = str();
        t.widthFactor = r.f64();
        t.oblique = r.f64();
        if (version >= 3)
            t.height = r.f64();
        if (version >= 5) {
            const std::uint8_t f = r.u8();
            t.annotative = (f & 1) != 0;
            t.backward = (f & 2) != 0;
            t.upsideDown = (f & 4) != 0;
        }
    }
    void row(DimStyle& d) {
        d.name = str();
        d.textHeight = r.f64();
        d.textStyle = TextStyleId{id32()};
        d.textGap = r.f64();
        d.arrowType = enumeration(ArrowType::None);
        d.arrowSize = r.f64();
        d.extLineOffset = r.f64();
        d.extLineExtension = r.f64();
        d.dimLineExtension = r.f64();
        d.textPlacement = enumeration(DimTextPlacement::Below);
        d.textAlign = enumeration(DimTextAlign::Horizontal);
        d.units = enumeration(LengthUnit::Foot);
        d.precision = r.i32();
        d.prefix = str();
        d.suffix = str();
        d.roundOff = r.f64();
        d.overallScale = r.f64();
        std::uint8_t f = r.u8();
        if (f > 3)
            bad("invalid dimension style flags");
        d.suppressLeadingZeros = (f & 1) != 0;
        d.suppressTrailingZeros = (f & 2) != 0;
        d.dimLineColor = color();
        d.extLineColor = color();
        d.textColor = color();
        if (version >= 4) {
            d.altUnits = r.u8() != 0;
            d.altScale = r.f64();
            d.altUnit = enumeration(LengthUnit::Foot);
            d.altPrecision = r.i32();
            d.altPrefix = str();
            d.altSuffix = str();
            d.tolerance = enumeration(DimTolerance::Limits);
            d.tolUpper = r.f64();
            d.tolLower = r.f64();
            d.tolPrecision = r.i32();
        }
        if (version >= 6) {
            d.linearFactor = r.f64();
            d.fit = enumeration(DimFit::BothOutside);
            d.textMove = enumeration(DimTextMove::Free);
        }
        if (version >= 7) {
            const std::uint8_t rules = r.u8();
            unpackDimRules(d, rules, r.u8());
        }
        if (version >= 8) {
            d.angleFormat = enumeration(DimAngleFormat::Radians);
            d.angularPrecision = std::clamp(static_cast<int>(r.u8()) - 1, -1, 8);
            d.commaDecimal = r.u8() != 0;
        }
        if (version >= 9) {
            d.baselineSpacing = r.f64();
            d.centerMark = r.f64();
            if (!std::isfinite(d.baselineSpacing) || d.baselineSpacing < 0 ||
                !std::isfinite(d.centerMark))
                bad("invalid dimension style spacing");
        }
        if (version >= 10) {
            d.textFill = std::min<int>(r.u8(), 2);
            d.textFillColor = r.u8();
            d.textFrame = r.u8() != 0;
        }
        if (version >= 11)
            d.linearFormat = enumeration(DimLinearFormat::Fractional);
    }
    void row(BlockDef& b) {
        b.name = str();
        b.base = r.vec2();
        b.anonymous = r.boolean();
        std::size_t n = r.count(1);
        if (n > limits.maxEntities)
            bad("too many entities in a block");
        b.entities.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            EntityId e{r.varint()};
            if (!e)
                bad("entity id 0");
            b.entities.push_back(e);
        }
        b.xrefPath = str();
        b.xrefStamp = r.i64();
    }

    template <class Row>
    void rows(std::vector<RowDelta<Row>>& out) {
        std::size_t n = r.count(2);
        if (n > limits.maxRows)
            bad("too many table rows");
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            RowDelta<Row> d;
            d.id = id32();
            if (d.id == 0)
                bad("table row id 0");
            std::uint8_t present = r.u8();
            if (present > 1)
                bad("invalid presence flag");
            if (present) {
                Row row_;
                row(row_);
                row_.id = decltype(row_.id){d.id};
                d.after = std::move(row_);
            }
            out.push_back(std::move(d));
        }
    }

    Header header() {
        Header h;
        h.units = enumeration(LengthUnit::Foot);
        h.workArea = r.f64();
        h.ltScale = r.f64();
        h.currentLayer = LayerId{id32()};
        h.currentLevel = LevelId{id32()};
        h.levelView = enumeration(LevelView::ShowAll);
        h.currentColor = color();
        h.currentLinetype = LinetypeId{r.u32()};
        h.currentLineweight = LineWeight{r.i16()};
        h.currentTransparency = static_cast<std::int8_t>(r.u8());
        if (!validObjectTransparency(h.currentTransparency))
            bad("invalid current transparency");
        h.currentTextStyle = TextStyleId{id32()};
        h.currentDimStyle = DimStyleId{id32()};
        h.createdBy = str();
        std::size_t n = r.count(2);
        if (n > limits.maxRows)
            bad("too many header entries");
        for (std::size_t i = 0; i < n; ++i) {
            std::string k = str();
            std::string v = str();
            if (isOpaqueKey(k))
                bad("reserved header key");
            h.custom[std::move(k)] = std::move(v);
        }
        return h;
    }
};

template <class Row, class IdT>
void collect(const std::vector<RowChange<Row>>& in, std::vector<RowDelta<Row>>& out) {
    for (const auto& c : in) {
        RowDelta<Row> d;
        d.id = (c.after ? c.after->id : c.before->id).value;
        d.after = c.after;
        out.push_back(std::move(d));
    }
}

template <class Row, class IdT, class Find>
void expand(const std::vector<RowDelta<Row>>& in, std::vector<RowChange<Row>>& out, Find&& find) {
    for (const auto& d : in) {
        RowChange<Row> c;
        if (const Row* old = find(IdT{d.id}))
            c.before = *old;
        c.after = d.after;
        out.push_back(std::move(c));
    }
}

}  // namespace

bool Delta::empty() const {
    return entities.empty() && layers.empty() && levels.empty() && linetypes.empty() &&
           textStyles.empty() && dimStyles.empty() && blocks.empty() && !header && order.empty();
}

Delta deltaOf(const ChangeSet& cs) {
    Delta d;
    d.name = cs.name;
    for (const EntityChange& c : cs.entities) {
        EntityDelta e;
        e.id = cs.idOf(c);
        if (const Entity* after = cs.after(c))
            e.after = *after;
        d.entities.push_back(std::move(e));
    }
    collect<Layer, LayerId>(cs.layers, d.layers);
    collect<Level, LevelId>(cs.levels, d.levels);
    collect<Linetype, LinetypeId>(cs.linetypes, d.linetypes);
    collect<TextStyle, TextStyleId>(cs.textStyles, d.textStyles);
    collect<DimStyle, DimStyleId>(cs.dimStyles, d.dimStyles);
    collect<BlockDef, BlockId>(cs.blocks, d.blocks);
    if (cs.header)
        d.header = cs.header->after;
    for (const OrderChange& o : cs.order)
        d.order.push_back({o.id, o.after});
    return d;
}

std::vector<std::uint8_t> encodeDelta(const Delta& delta) {
    StringTable st;
    ByteWriter body;
    putString(body, st, delta.name);

    body.varint(delta.entities.size());
    for (const EntityDelta& e : delta.entities) {
        body.u8(e.after ? 1 : 0);
        if (e.after) {
            Entity copy = *e.after;
            copy.common.id = e.id;
            encodeEntity(body, st, copy, BlockId{});
            body.varint(copy.common.level.value);
        } else {
            body.varint(e.id.value);
        }
    }
    putRows(body, st, delta.layers);
    putRows(body, st, delta.levels);
    putRows(body, st, delta.linetypes);
    putRows(body, st, delta.textStyles);
    putRows(body, st, delta.dimStyles);
    putRows(body, st, delta.blocks);
    body.u8(delta.header ? 1 : 0);
    if (delta.header)
        putHeader(body, st, *delta.header);
    body.varint(delta.order.size());
    for (const OrderDelta& o : delta.order) {
        body.varint(o.id.value);
        body.u8(o.after ? 1 : 0);
        if (o.after)
            body.i64(*o.after);
    }

    ByteWriter out;
    out.raw(kDeltaMagic, sizeof kDeltaMagic);
    out.u8(kDeltaVersion);
    out.varint(st.all().size());
    for (const std::string& s : st.all()) {
        out.varint(s.size());
        out.raw(s.data(), s.size());
    }
    out.raw(body.bytes());
    return std::move(out.bytes());
}

Result<Delta> decodeDelta(std::span<const std::uint8_t> bytes, const DeltaLimits& limits) {
    if (bytes.size() > limits.maxBytes)
        return makeError(ErrorCode::OutOfRange, "change is too large");
    try {
        ByteReader r(bytes);
        auto magic = r.bytes(sizeof kDeltaMagic);
        if (!std::equal(magic.begin(), magic.end(), kDeltaMagic))
            bad("not a change record");
        const std::uint8_t version = r.u8();
        if (version < 2 || version > kDeltaVersion)
            bad("unsupported change record version");
        std::size_t ns = r.count(1);
        std::vector<std::string> strings;
        strings.reserve(ns);
        for (std::size_t i = 0; i < ns; ++i) {
            std::size_t len = r.count(1);
            auto b = r.bytes(len);
            std::string s(reinterpret_cast<const char*>(b.data()), b.size());
            if (!validUtf8(s))
                bad("string is not valid UTF-8");
            strings.push_back(std::move(s));
        }
        if (strings.empty() || !strings[0].empty())
            bad("string table must start with the empty string");

        Reader rd{r, strings, limits, version};
        Delta d;
        d.name = rd.str();
        std::size_t ne = r.count(2);
        if (ne > limits.maxEntities)
            bad("too many entities");
        d.entities.reserve(ne);
        std::unordered_set<EntityId> seen;
        for (std::size_t i = 0; i < ne; ++i) {
            EntityDelta e;
            std::uint8_t present = r.u8();
            if (present > 1)
                bad("invalid presence flag");
            if (present) {
                DecodedEntity dec = decodeEntity(r, strings);
                dec.entity.common.level = LevelId{rd.id32()};
                e.id = dec.entity.common.id;
                e.after = std::move(dec.entity);
            } else {
                e.id = EntityId{r.varint()};
                if (!e.id)
                    bad("entity id 0");
            }
            if (!seen.insert(e.id).second)
                bad("entity listed twice");
            d.entities.push_back(std::move(e));
        }
        rd.rows(d.layers);
        rd.rows(d.levels);
        rd.rows(d.linetypes);
        rd.rows(d.textStyles);
        rd.rows(d.dimStyles);
        rd.rows(d.blocks);
        std::uint8_t hasHeader = r.u8();
        if (hasHeader > 1)
            bad("invalid presence flag");
        if (hasHeader)
            d.header = rd.header();
        std::size_t no = r.count(2);
        if (no > limits.maxEntities)
            bad("too many order entries");
        for (std::size_t i = 0; i < no; ++i) {
            OrderDelta o;
            o.id = EntityId{r.varint()};
            if (!o.id)
                bad("entity id 0");
            std::uint8_t present = r.u8();
            if (present > 1)
                bad("invalid presence flag");
            if (present)
                o.after = r.i64();
            d.order.push_back(o);
        }
        if (!r.atEnd())
            bad("trailing data");
        return d;
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::Parse, std::string("bad change record: ") + ex.what());
    }
}

Result<ChangeSet> materialize(const Delta& d, const Document& doc) {
    ChangeSet cs;
    cs.name = d.name;
    auto snaps = std::make_shared<std::vector<Entity>>();
    snaps->reserve(d.entities.size() * 2);
    for (const EntityDelta& e : d.entities) {
        EntityChange c;
        if (const Entity* old = doc.find(e.id)) {
            c.before = static_cast<std::uint32_t>(snaps->size());
            snaps->push_back(*old);
        } else if (!e.after) {
            return makeError(ErrorCode::NotFound, "change erases an entity that does not exist");
        }
        if (e.after) {
            c.after = static_cast<std::uint32_t>(snaps->size());
            snaps->push_back(*e.after);
            (*snaps)[c.after].common.id = e.id;
        }
        cs.entities.push_back(c);
    }
    cs.snapshots = std::move(snaps);
    expand<Layer, LayerId>(d.layers, cs.layers, [&](LayerId id) { return doc.layer(id); });
    expand<Level, LevelId>(d.levels, cs.levels, [&](LevelId id) { return doc.level(id); });
    expand<Linetype, LinetypeId>(d.linetypes, cs.linetypes,
                                 [&](LinetypeId id) { return doc.linetype(id); });
    expand<TextStyle, TextStyleId>(d.textStyles, cs.textStyles,
                                   [&](TextStyleId id) { return doc.textStyle(id); });
    expand<DimStyle, DimStyleId>(d.dimStyles, cs.dimStyles,
                                 [&](DimStyleId id) { return doc.dimStyle(id); });
    expand<BlockDef, BlockId>(d.blocks, cs.blocks, [&](BlockId id) { return doc.block(id); });
    for (const auto& r : cs.layers) {
        if (!r.before && !r.after)
            return makeError(ErrorCode::NotFound, "change erases a layer that does not exist");
    }
    for (const auto& r : cs.levels) {
        if (!r.before && !r.after)
            return makeError(ErrorCode::NotFound, "change erases a level that does not exist");
    }
    for (const auto& r : cs.linetypes) {
        if (!r.before && !r.after)
            return makeError(ErrorCode::NotFound, "change erases a linetype that does not exist");
    }
    for (const auto& r : cs.textStyles) {
        if (!r.before && !r.after)
            return makeError(ErrorCode::NotFound, "change erases a text style that does not exist");
    }
    for (const auto& r : cs.dimStyles) {
        if (!r.before && !r.after)
            return makeError(ErrorCode::NotFound,
                             "change erases a dimension style that does not exist");
    }
    for (const auto& r : cs.blocks) {
        if (!r.before && !r.after)
            return makeError(ErrorCode::NotFound, "change erases a block that does not exist");
    }
    if (d.header) {
        Header after = *d.header;
        const Header& now = doc.header();
        after.activeOnly = now.activeOnly;
        for (const auto& [k, v] : now.custom) {
            if (isOpaqueKey(k))
                after.custom[k] = v;
        }
        cs.header = HeaderChange{now, std::move(after)};
    }
    for (const OrderDelta& o : d.order) {
        OrderChange oc;
        oc.id = o.id;
        oc.before = doc.drawOrderOverride(o.id);
        oc.after = o.after;
        cs.order.push_back(oc);
    }
    return cs;
}

}  // namespace tbcad::io
