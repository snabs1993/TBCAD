#include "tbcad/core/document.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>

#include "tbcad/core/dim_assoc.h"

namespace tbcad {

namespace {

std::string foldName(std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

bool validName(std::string_view s) {
    if (s.empty())
        return false;
    for (char c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
            return false;
    }
    return true;
}

// Index key of a table row: names are unique per parent for layers, globally otherwise.
std::string layerKey(LayerId parent, std::string_view name) {
    return std::to_string(parent.value) + "/" + foldName(name);
}
template <class Row>
std::string nameKey(const Row& r) {
    if constexpr (std::is_same_v<Row, Layer>)
        return layerKey(r.parent, r.name);
    else
        return foldName(r.name);
}

Error invalid(std::string msg) {
    return makeError(ErrorCode::InvalidArgument, std::move(msg));
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// EntityStore, tables, ChangeSet

namespace detail {

EntityStore::~EntityStore() {
    while (size_ > 0)
        popBack();
    for (Entity* c : chunks_)
        ::operator delete(c, std::align_val_t{alignof(Entity)});
}

std::size_t EntityStore::pushBack(Entity&& e) {
    if (size_ == chunks_.size() * kChunk) {
        void* mem = ::operator new(sizeof(Entity) * kChunk, std::align_val_t{alignof(Entity)});
        chunks_.push_back(static_cast<Entity*>(mem));
    }
    new (&at(size_)) Entity(std::move(e));
    return size_++;
}

void EntityStore::popBack() {
    at(size_ - 1).~Entity();
    --size_;
}

template <class Row, class IdT>
const Row* Table<Row, IdT>::findByName(std::string_view name) const {
    auto it = byName.find(std::is_same_v<Row, Layer> ? layerKey(LayerId{}, name) : foldName(name));
    return it == byName.end() ? nullptr : find(it->second);
}

template struct Table<Layer, LayerId>;
template struct Table<Level, LevelId>;
template struct Table<Linetype, LinetypeId>;
template struct Table<TextStyle, TextStyleId>;
template struct Table<DimStyle, DimStyleId>;
template struct Table<BlockDef, BlockId>;

}  // namespace detail

bool ChangeSet::empty() const {
    return entities.empty() && layers.empty() && levels.empty() && linetypes.empty() &&
           textStyles.empty() && dimStyles.empty() && blocks.empty() && !header && order.empty();
}

namespace {
template <class Row>
std::vector<RowChange<Row>> invertRows(const std::vector<RowChange<Row>>& in) {
    std::vector<RowChange<Row>> out;
    out.reserve(in.size());
    for (const auto& c : in)
        out.push_back({c.after, c.before});
    return out;
}
}  // namespace

ChangeSet ChangeSet::inverted() const {
    ChangeSet r;
    r.name = name;
    r.snapshots = snapshots;
    if (!boxMemo)
        boxMemo = std::make_shared<BoxMemo>();
    r.boxMemo = boxMemo;  // the same snapshots, so the same boxes
    r.entities.reserve(entities.size());
    for (const auto& c : entities)
        r.entities.push_back({c.after, c.before});
    r.layers = invertRows(layers);
    r.levels = invertRows(levels);
    r.linetypes = invertRows(linetypes);
    r.textStyles = invertRows(textStyles);
    r.dimStyles = invertRows(dimStyles);
    r.blocks = invertRows(blocks);
    if (header)
        r.header = HeaderChange{header->after, header->before};
    for (const auto& o : order)
        r.order.push_back({o.id, o.after, o.before});
    r.outOfArea = outOfArea;
    return r;
}

// ---------------------------------------------------------------------------------------------
// Validation

namespace {

constexpr int kMaxLayerDepth = 8;
constexpr std::int64_t kFrontBase = std::int64_t{1} << 62;

// Checks one entity's payload, normalizes where the model says so (arc angles) and measures the
// largest absolute coordinate.
struct Checker {
    const Document& doc;
    BlockId owner;
    double extent = 0;
    std::string err;

    void bad(std::string msg) {
        if (err.empty())
            err = std::move(msg);
    }
    void v(double x) {
        if (!std::isfinite(x))
            bad("non-finite number");
        else
            extent = std::max(extent, std::abs(x));
    }
    void p(const Vec2& a) {
        v(a.x);
        v(a.y);
    }
    void pr(const Vec2& c, double r) {
        p(c);
        v(r);
        if (err.empty())
            extent = std::max(extent, std::max(std::abs(c.x), std::abs(c.y)) + std::abs(r));
    }
    void finite(double x) {
        if (!std::isfinite(x))
            bad("non-finite number");
    }
    void textStyleOk(TextStyleId s) {
        if (s && !doc.textStyle(s))
            bad("unknown text style");
    }
    void dimStyleOk(DimStyleId s) {
        if (s && !doc.dimStyle(s))
            bad("unknown dimension style");
    }
    void polyVertices(const std::vector<PolyVertex>& vs) {
        for (const auto& pv : vs) {
            p(pv.p);
            finite(pv.bulge);
            finite(pv.startWidth);
            finite(pv.endWidth);
        }
    }
    void spline(const SplineData& d) {
        if (d.degree < 1)
            bad("spline degree must be at least 1");
        for (const auto& c : d.control)
            p(c);
        for (const auto& c : d.fit)
            p(c);
        for (double k : d.knots)
            finite(k);
        for (double w : d.weights)
            finite(w);
        if (d.control.empty() && d.fit.empty())
            bad("spline has no control or fit points");
        if (!d.weights.empty() && d.weights.size() != d.control.size()) {
            bad("spline weights must match control points");
        }
        if (!d.knots.empty() && !d.control.empty() &&
            d.knots.size() != d.control.size() + static_cast<std::size_t>(d.degree) + 1) {
            bad("spline knot count must be control points + degree + 1");
        }
        if (!std::is_sorted(d.knots.begin(), d.knots.end()))
            bad("spline knots must not decrease");
    }

    void operator()(LineData& d) {
        p(d.a);
        p(d.b);
    }
    void operator()(CircleData& d) {
        pr(d.center, d.radius);
        if (!(d.radius > 0))
            bad("circle radius must be positive");
    }
    void operator()(ArcData& d) {
        pr(d.center, d.radius);
        finite(d.startAngle);
        finite(d.endAngle);
        if (!(d.radius > 0))
            bad("arc radius must be positive");
        if (!err.empty())
            return;
        d.startAngle = normalizeAngle(d.startAngle);
        d.endAngle = normalizeAngle(d.endAngle);
        if (tol::nearlyEqualAngle(d.startAngle, d.endAngle)) {
            bad("arc start and end angle are equal; use a circle");
        }
    }
    void operator()(PolylineData& d) {
        polyVertices(d.v);
        finite(d.constantWidth);
        if (d.v.empty())
            bad("polyline has no vertices");
    }
    void operator()(PointData& d) { p(d.p); }
    void operator()(EllipseData& d) {
        p(d.center);
        p(d.majorAxis);
        finite(d.ratio);
        finite(d.startParam);
        finite(d.endParam);
        if (err.empty())
            extent = std::max(
                extent, std::max(std::abs(d.center.x), std::abs(d.center.y)) + length(d.majorAxis));
        if (!(length(d.majorAxis) > 0))
            bad("ellipse major axis has zero length");
        if (!(d.ratio > 0 && d.ratio <= 1))
            bad("ellipse ratio must be in (0, 1]");
    }
    void operator()(SplineData& d) { spline(d); }
    void operator()(TextData& d) {
        p(d.position);
        p(d.alignPoint);
        finite(d.rotation);
        finite(d.widthFactor);
        finite(d.oblique);
        if (!(d.height > 0) || !std::isfinite(d.height))
            bad("text height must be positive");
        textStyleOk(d.style);
    }
    void operator()(MTextData& d) {
        p(d.position);
        finite(d.width);
        finite(d.rotation);
        if (!(d.height > 0) || !std::isfinite(d.height))
            bad("text height must be positive");
        textStyleOk(d.style);
    }
    void operator()(HatchData& d) {
        p(d.patternOrigin);
        finite(d.patternScale);
        finite(d.patternAngle);
        for (auto& loop : d.loops) {
            for (auto& e : loop.edges) {
                std::visit(
                    [&](auto& edge) {
                        using E = std::decay_t<decltype(edge)>;
                        if constexpr (std::is_same_v<E, HatchLineEdge>) {
                            p(edge.a);
                            p(edge.b);
                        } else if constexpr (std::is_same_v<E, HatchArcEdge>) {
                            pr(edge.center, edge.radius);
                            finite(edge.startAngle);
                            finite(edge.endAngle);
                        } else if constexpr (std::is_same_v<E, HatchEllipseEdge>) {
                            p(edge.center);
                            p(edge.majorAxis);
                            finite(edge.ratio);
                            finite(edge.startParam);
                            finite(edge.endParam);
                        } else if constexpr (std::is_same_v<E, HatchSplineEdge>) {
                            spline(edge.spline);
                        } else {
                            polyVertices(edge.v);
                        }
                    },
                    e);
            }
        }
    }
    void operator()(InsertData& d) {
        p(d.position);
        finite(d.scale.x);
        finite(d.scale.y);
        finite(d.rotation);
        if (!doc.block(d.block)) {
            bad("insert refers to an unknown block");
            return;
        }
        if (owner) {
            // Reject cycles: the inserted block must not (transitively) insert the owner.
            std::vector<BlockId> stack{d.block};
            std::unordered_set<BlockId> seen;
            while (!stack.empty() && err.empty()) {
                BlockId b = stack.back();
                stack.pop_back();
                if (b == owner) {
                    bad("block insert would create a cycle");
                    break;
                }
                if (!seen.insert(b).second)
                    continue;
                const BlockDef* def = doc.block(b);
                if (!def)
                    continue;
                for (EntityId id : def->entities) {
                    const Entity* e = doc.find(id);
                    if (const auto* ins = e ? entityAs<InsertData>(*e) : nullptr) {
                        stack.push_back(ins->block);
                    }
                }
            }
        }
    }
    void operator()(DimensionData& d) {
        for (const auto& q : d.defPts)
            p(q);
        p(d.textPos);
        finite(d.rotation);
        finite(d.measurement);
        dimStyleOk(d.style);
    }
    void operator()(LeaderData& d) {
        for (const auto& q : d.vertices)
            p(q);
        p(d.textPos);
        dimStyleOk(d.style);
    }
    void operator()(MLeaderData& d) {
        for (const auto& line : d.lines)
            for (const auto& q : line)
                p(q);
        p(d.landing);
        p(d.contentPos);
        finite(d.textHeight);
        finite(d.blockScale);
        finite(d.blockRotation);
        if (d.textHeight < 0)
            bad("multileader text height must not be negative");
        if (d.lines.size() > 10000)
            bad("multileader is too large");
        dimStyleOk(d.style);
    }
    void operator()(RayData& d) {
        p(d.origin);
        p(d.dir);
        const double len = std::hypot(d.dir.x, d.dir.y);
        if (!(len > 1e-12))
            bad("ray direction must not be zero");
        else
            d.dir = {d.dir.x / len, d.dir.y / len};
    }
    void operator()(MlineData& d) {
        for (const Vec2& a : d.v)
            p(a);
        finite(d.scale);
        if (!(d.scale > 0))
            bad("multiline scale must be positive");
        if (d.justification < 0 || d.justification > 2)
            bad("multiline justification must be 0, 1 or 2");
        if (d.v.size() > 100000)
            bad("multiline is too large");
    }
    void operator()(TableData& d) {
        p(d.origin);
        finite(d.rotation);
        finite(d.textHeight);
        finite(d.margin);
        if (d.colWidths.empty() || d.rowHeights.empty() ||
            d.cells.size() != d.colWidths.size() * d.rowHeights.size()) {
            bad("table cells do not match its rows and columns");
            return;
        }
        if (d.colWidths.size() > 1000 || d.rowHeights.size() > 100000) {
            bad("table is too large");
            return;
        }
        for (double w : d.colWidths) {
            finite(w);
            if (!(w > 0))
                bad("table column width must be positive");
        }
        for (double h : d.rowHeights) {
            finite(h);
            if (!(h > 0))
                bad("table row height must be positive");
        }
        if (!(d.textHeight > 0))
            bad("table text height must be positive");
        const std::size_t cols = d.colWidths.size(), rows = d.rowHeights.size();
        for (std::size_t i = 0; i < d.cells.size(); ++i) {
            const TableCell& c = d.cells[i];
            if (c.colSpan < 1 || c.rowSpan < 1 || i % cols + c.colSpan > cols ||
                i / cols + c.rowSpan > rows)
                bad("table cell merge goes outside the table");
        }
    }
    void operator()(ProxyData& d) {
        for (const auto& item : d.graphics.items) {
            std::visit(
                [&](const auto& it) {
                    using I = std::decay_t<decltype(it)>;
                    if constexpr (std::is_same_v<I, DisplayFill>) {
                        for (const auto& ring : it.rings) {
                            for (const auto& q : ring)
                                p(q);
                        }
                    } else {
                        for (const auto& q : it.pts)
                            p(q);
                    }
                },
                item);
        }
    }
};

// Returns the largest absolute coordinate of the entity, or an error. Fills in a default layer.
Result<double> validateEntity(const Document& doc, Entity& e, BlockId owner) {
    EntityCommon& c = e.common;
    if (!c.layer)
        c.layer = doc.header().currentLayer;
    if (!doc.layer(c.layer))
        return invalid("unknown layer");
    if (!c.level)
        c.level = doc.header().currentLevel;
    if (!doc.level(c.level))
        return invalid("unknown level");
    if (c.linetype != kLinetypeByLayer && c.linetype != kLinetypeByBlock &&
        !doc.linetype(c.linetype)) {
        return invalid("unknown linetype");
    }
    if (c.color.kind == ColorKind::ByParent)
        return invalid("ByParent applies to layers only");
    if (c.color.kind == ColorKind::Index && c.color.indexValue() == 0) {
        return invalid("color index 0 is not valid");
    }
    if (c.lineweight.v < -3)
        return invalid("invalid lineweight");
    if (!validObjectTransparency(c.transparency))
        return invalid("transparency must be ByLayer, ByBlock or 0 to 90");
    if (!std::isfinite(c.linetypeScale) || !(c.linetypeScale > 0)) {
        return invalid("linetype scale must be positive");
    }
    Checker chk{doc, owner, 0, {}};
    visitEntity(e, chk);
    if (!chk.err.empty())
        return invalid(chk.err);
    if (chk.extent > kHardCap) {
        return makeError(ErrorCode::OutOfRange,
                         "geometry lies beyond the hard coordinate limit of 1e10 mm");
    }
    return chk.extent;
}

// Per-row validation. `old` is the row before an edit (null for a new row).
Result<void> checkRow(const Document& doc, Layer& r, const Layer* old) {
    if (!validName(r.name))
        return invalid("invalid layer name");
    if (old && old->id == kLayerZero && r.name != old->name)
        return invalid("layer \"0\" cannot be renamed");
    if (r.parent) {
        if (old && old->id == kLayerZero)
            return invalid("layer \"0\" cannot have a parent");
        if (r.parent == kLayerZero)
            return invalid("layer \"0\" cannot have sub-layers");
        if (!doc.layer(r.parent))
            return invalid("unknown parent layer");
        int depth = 1;
        for (LayerId p = r.parent; p; p = doc.layer(p)->parent) {
            if (old && p == old->id)
                return invalid("layer parent would create a cycle");
            ++depth;
        }
        // Height of the subtree below this layer (0 for a leaf).
        int height = 0;
        if (old) {
            for (const auto& [lid, l] : doc.layers()) {
                int d = 1;
                bool below = false;
                for (LayerId p = l.parent; p; p = doc.layer(p)->parent, ++d)
                    below = below || p == old->id;
                if (below) {
                    d = 0;
                    for (LayerId p = l.id; p != old->id; p = doc.layer(p)->parent)
                        ++d;
                    height = std::max(height, d);
                }
            }
        }
        if (depth + height > kMaxLayerDepth)
            return invalid("sub-layers nest at most 8 levels deep");
    } else if (r.color.kind == ColorKind::ByParent || r.lineweight.isByParent() ||
               r.linetype == kLinetypeByParent) {
        return invalid("ByParent needs a parent layer");
    }
    if (r.color.kind == ColorKind::ByLayer || r.color.kind == ColorKind::ByBlock)
        return invalid("a layer color cannot be ByLayer or ByBlock");
    if (r.color.kind == ColorKind::Index && r.color.indexValue() == 0)
        return invalid("color index 0 is not valid");
    if (!r.linetype)
        r.linetype = kLinetypeContinuous;
    if (r.linetype != kLinetypeByParent && !doc.linetype(r.linetype))
        return invalid("unknown linetype");
    if (r.lineweight.isByLayer() || r.lineweight.isByBlock() || r.lineweight.v < -4)
        return invalid("a layer lineweight must be Default, a value or ByParent");
    if (r.transparency > kTransparencyMax)
        return invalid("a layer transparency is 0 to 90");
    return {};
}
Result<void> checkRow(const Document&, Level& r, const Level*) {
    if (!validName(r.name))
        return invalid("invalid level name");
    if (!std::isfinite(r.elevation))
        return invalid("non-finite level elevation");
    return {};
}
Result<void> checkRow(const Document&, Linetype& r, const Linetype* old) {
    if (!validName(r.name))
        return invalid("invalid linetype name");
    double total = 0;
    for (double d : r.pattern) {
        if (!std::isfinite(d))
            return invalid("linetype pattern has a non-finite element");
        total += std::abs(d);
    }
    if (!r.pattern.empty() && !(total > 0))
        return invalid("linetype pattern has no length");
    if (old && old->id == kLinetypeContinuous) {
        if (r.name != old->name || !r.pattern.empty()) {
            return invalid("the Continuous linetype cannot be changed");
        }
    }
    return {};
}
Result<void> checkRow(const Document&, TextStyle& r, const TextStyle* old) {
    if (!validName(r.name))
        return invalid("invalid text style name");
    if (old && old->id == kTextStyleStandard && r.name != old->name) {
        return invalid("the Standard text style cannot be renamed");
    }
    if (!std::isfinite(r.widthFactor) || !(r.widthFactor > 0)) {
        return invalid("width factor must be positive");
    }
    if (!std::isfinite(r.oblique))
        return invalid("non-finite oblique angle");
    return {};
}
Result<void> checkRow(const Document& doc, DimStyle& r, const DimStyle* old) {
    if (!validName(r.name))
        return invalid("invalid dimension style name");
    if (old && old->id == kDimStyleStandard && r.name != old->name) {
        return invalid("the Standard dimension style cannot be renamed");
    }
    if (r.textStyle && !doc.textStyle(r.textStyle))
        return invalid("unknown text style");
    for (double d : {r.textHeight, r.arrowSize, r.overallScale}) {
        if (!std::isfinite(d) || !(d > 0))
            return invalid("dimension size must be positive");
    }
    for (double d :
         {r.textGap, r.extLineOffset, r.extLineExtension, r.dimLineExtension, r.roundOff}) {
        if (!std::isfinite(d) || d < 0)
            return invalid("dimension distance must not be negative");
    }
    r.precision = std::clamp(r.precision, 0, 16);
    if (!std::isfinite(r.altScale) || !(r.altScale > 0))
        return invalid("alternate unit scale must be positive");
    for (double d : {r.tolUpper, r.tolLower}) {
        if (!std::isfinite(d) || d < 0)
            return invalid("tolerance must not be negative");
    }
    if (!std::isfinite(r.linearFactor) || !(r.linearFactor > 0))
        return invalid("linear scale factor must be positive");
    r.textJustify = std::clamp(r.textJustify, 0, 2);
    r.angularPrecision = std::clamp(r.angularPrecision, -1, 8);
    r.textFill = std::clamp(r.textFill, 0, 2);
    r.textFillColor = std::clamp(r.textFillColor, 0, 255);
    if (!std::isfinite(r.baselineSpacing) || r.baselineSpacing < 0)
        return invalid("baseline spacing must not be negative");
    if (!std::isfinite(r.centerMark))
        return invalid("centre mark size must be a number");
    if (static_cast<std::uint8_t>(r.angleFormat) >
        static_cast<std::uint8_t>(DimAngleFormat::Radians))
        r.angleFormat = DimAngleFormat::Degrees;
    r.altPrecision = std::clamp(r.altPrecision, 0, 16);
    r.tolPrecision = std::clamp(r.tolPrecision, 0, 16);
    return {};
}
Result<void> checkRow(const Document&, BlockDef& r, const BlockDef*) {
    if (!validName(r.name))
        return invalid("invalid block name");
    if (!isFinite(r.base))
        return invalid("non-finite base point");
    return {};
}

// Header rules shared by Transaction::modifyHeader and Document::applyChecked.
Result<void> checkHeader(const Document& d, const Header& work) {
    if (!std::isfinite(work.workArea) || !(work.workArea > 0) || work.workArea > kHardCap) {
        return makeError(ErrorCode::OutOfRange, "work area must be in (0, 1e10] mm");
    }
    if (!std::isfinite(work.ltScale) || !(work.ltScale > 0)) {
        return invalid("linetype scale must be positive");
    }
    if (!d.layer(work.currentLayer))
        return invalid("unknown current layer");
    if (!d.level(work.currentLevel))
        return invalid("unknown current level");
    if (work.currentColor.kind == ColorKind::ByParent)
        return invalid("the current colour cannot be ByParent");
    if (work.currentColor.kind == ColorKind::Index && work.currentColor.indexValue() == 0)
        return invalid("color index 0 is not valid");
    if (work.currentLinetype != kLinetypeByLayer && work.currentLinetype != kLinetypeByBlock &&
        !d.linetype(work.currentLinetype))
        return invalid("unknown current linetype");
    if (work.currentLineweight.v < -3)
        return invalid("invalid current lineweight");
    if (!validObjectTransparency(work.currentTransparency))
        return invalid("invalid current transparency");
    if (!d.textStyle(work.currentTextStyle))
        return invalid("unknown current text style");
    if (!d.dimStyle(work.currentDimStyle))
        return invalid("unknown current dimension style");
    return {};
}

bool isBuiltin(LayerId id) {
    return id == kLayerZero;
}
bool isBuiltin(LevelId id) {
    return id == kLevelDefault;
}
bool isBuiltin(LinetypeId id) {
    return id == kLinetypeContinuous;
}
bool isBuiltin(TextStyleId id) {
    return id == kTextStyleStandard;
}
bool isBuiltin(BlockId) {
    return false;
}
bool isBuiltin(DimStyleId id) {
    return id == kDimStyleStandard;
}

}  // namespace

namespace detail {
// Which table rows the objects (blocks' included) refer to; see Transaction::cacheObjectReferences.
struct ObjectRefs {
    std::unordered_set<std::uint32_t> layers, levels, linetypes, textStyles, dimStyles;
    // Inserts per block, as a count, so erasing an unused block frees the blocks it inserted in
    // the same pass (a chain of 300 nested unused blocks goes in one scan, not 300).
    std::unordered_map<std::uint32_t, std::uint32_t> blockUses;
};
}  // namespace detail

namespace {

std::unique_ptr<detail::ObjectRefs> scanObjectRefs(const Document& d) {
    auto r = std::make_unique<detail::ObjectRefs>();
    d.forEachEntity([&](const Entity& e) {
        r->layers.insert(e.common.layer.value);
        r->levels.insert(e.common.level.value);
        r->linetypes.insert(e.common.linetype.value);
        if (const auto* t = entityAs<TextData>(e))
            r->textStyles.insert(t->style.value);
        else if (const auto* t = entityAs<MTextData>(e))
            r->textStyles.insert(t->style.value);
        else if (const auto* t = entityAs<DimensionData>(e))
            r->dimStyles.insert(t->style.value);
        else if (const auto* t = entityAs<LeaderData>(e))
            r->dimStyles.insert(t->style.value);
        else if (const auto* t = entityAs<MLeaderData>(e))
            r->dimStyles.insert(t->style.value);
        else if (const auto* t = entityAs<InsertData>(e))
            ++r->blockUses[t->block.value];
    });
    return r;
}

bool inUse(const Document& d, LayerId id, const detail::ObjectRefs* refs = nullptr) {
    if (d.header().currentLayer == id)
        return true;
    for (const auto& [lid, l] : d.layers()) {
        if (l.parent == id)
            return true;  // has sub-layers
    }
    if (refs)
        return refs->layers.count(id.value) != 0;
    bool used = false;
    d.forEachEntity([&](const Entity& e) { used = used || e.common.layer == id; });
    return used;
}
bool inUse(const Document& d, LevelId id, const detail::ObjectRefs* refs = nullptr) {
    if (d.header().currentLevel == id)
        return true;
    if (refs)
        return refs->levels.count(id.value) != 0;
    bool used = false;
    d.forEachEntity([&](const Entity& e) { used = used || e.common.level == id; });
    return used;
}
bool inUse(const Document& d, LinetypeId id, const detail::ObjectRefs* refs = nullptr) {
    if (d.header().currentLinetype == id)
        return true;
    for (const auto& [lid, l] : d.layers()) {
        if (l.linetype == id)
            return true;
    }
    if (refs)
        return refs->linetypes.count(id.value) != 0;
    bool used = false;
    d.forEachEntity([&](const Entity& e) { used = used || e.common.linetype == id; });
    return used;
}
bool inUse(const Document& d, TextStyleId id, const detail::ObjectRefs* refs = nullptr) {
    if (d.header().currentTextStyle == id)
        return true;
    for (const auto& [sid, s] : d.dimStyles()) {
        if (s.textStyle == id)
            return true;
    }
    if (refs)
        return refs->textStyles.count(id.value) != 0;
    bool used = false;
    d.forEachEntity([&](const Entity& e) {
        if (const auto* t = entityAs<TextData>(e))
            used = used || t->style == id;
        if (const auto* t = entityAs<MTextData>(e))
            used = used || t->style == id;
    });
    return used;
}
bool inUse(const Document& d, DimStyleId id, const detail::ObjectRefs* refs = nullptr) {
    if (d.header().currentDimStyle == id)
        return true;
    if (refs)
        return refs->dimStyles.count(id.value) != 0;
    bool used = false;
    d.forEachEntity([&](const Entity& e) {
        if (const auto* t = entityAs<DimensionData>(e))
            used = used || t->style == id;
        if (const auto* t = entityAs<LeaderData>(e))
            used = used || t->style == id;
        if (const auto* t = entityAs<MLeaderData>(e))
            used = used || t->style == id;
    });
    return used;
}
bool inUse(const Document& d, BlockId id, const detail::ObjectRefs* refs = nullptr) {
    if (refs)
        return refs->blockUses.count(id.value) != 0;
    bool used = false;
    d.forEachEntity([&](const Entity& e) {
        if (const auto* t = entityAs<InsertData>(e))
            used = used || t->block == id;
    });
    return used;
}

// Replaces (or removes, when newRow is empty) a table row and keeps the name index in step.
template <class Row, class IdT>
void replaceRow(detail::Table<Row, IdT>& t, IdT id, const std::optional<Row>& newRow) {
    ++t.version;
    auto it = t.rows.find(id);
    if (it != t.rows.end()) {
        t.byName.erase(nameKey(it->second));
        t.rows.erase(it);
    }
    if (newRow) {
        t.rows.emplace(id, *newRow);
        t.byName[nameKey(*newRow)] = id;
        t.next = std::max<std::uint32_t>(t.next, id.value + 1);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Document

NextIds Document::nextIds() const {
    NextIds n;
    n.entity = nextEntityId_;
    n.layer = layers_.next;
    n.level = levels_.next;
    n.linetype = linetypes_.next;
    n.textStyle = textStyles_.next;
    n.dimStyle = dimStyles_.next;
    n.block = blocks_.next;
    return n;
}

Result<void> Document::reserveIds(const NextIds& ids) {
    if (active_)
        return makeError(ErrorCode::Conflict, "cannot reserve ids while a transaction is open");
    nextEntityId_ = std::max(nextEntityId_, ids.entity);
    layers_.next = std::max(layers_.next, ids.layer);
    levels_.next = std::max(levels_.next, ids.level);
    linetypes_.next = std::max(linetypes_.next, ids.linetype);
    textStyles_.next = std::max(textStyles_.next, ids.textStyle);
    dimStyles_.next = std::max(dimStyles_.next, ids.dimStyle);
    blocks_.next = std::max(blocks_.next, ids.block);
    return {};
}

Document::Document() {
    static std::atomic<std::uint64_t> next{1};
    uid_ = next.fetch_add(1);
    Linetype cont;
    cont.id = kLinetypeContinuous;
    cont.name = "Continuous";
    cont.description = "Solid line";
    replaceRow<Linetype, LinetypeId>(linetypes_, cont.id, cont);

    Layer zero;
    zero.id = kLayerZero;
    zero.name = "0";
    zero.linetype = kLinetypeContinuous;
    replaceRow<Layer, LayerId>(layers_, zero.id, zero);

    Level lv;
    lv.id = kLevelDefault;
    lv.name = "Level 1";
    replaceRow<Level, LevelId>(levels_, lv.id, lv);

    TextStyle ts;
    ts.id = kTextStyleStandard;
    ts.name = "Standard";
    replaceRow<TextStyle, TextStyleId>(textStyles_, ts.id, ts);

    DimStyle ds;
    ds.id = kDimStyleStandard;
    ds.name = "Standard";
    ds.textStyle = kTextStyleStandard;
    replaceRow<DimStyle, DimStyleId>(dimStyles_, ds.id, ds);

    posOf_.assign(1, 0xFFFFFFFFu);
}

Document::~Document() = default;

const Entity* Document::find(EntityId id) const {
    if (id.value >= posOf_.size())
        return nullptr;
    std::uint32_t pos = posOf_[id.value];
    return pos == 0xFFFFFFFFu ? nullptr : &store_.at(pos);
}

Entity* Document::findMutable(EntityId id) {
    return const_cast<Entity*>(static_cast<const Document*>(this)->find(id));
}

void Document::insertEntity(Entity&& e) {
    std::uint64_t v = e.common.id.value;
    if (v >= posOf_.size())
        posOf_.resize(v + 1, 0xFFFFFFFFu);
    posOf_[v] = static_cast<std::uint32_t>(store_.pushBack(std::move(e)));
    nextEntityId_ = std::max(nextEntityId_, v + 1);
}

void Document::removeEntity(EntityId id) {
    std::uint32_t pos = id.value < posOf_.size() ? posOf_[id.value] : 0xFFFFFFFFu;
    if (pos == 0xFFFFFFFFu)
        return;
    std::size_t last = store_.size() - 1;
    if (pos != last) {
        Entity& moved = store_.at(last);
        posOf_[moved.common.id.value] = pos;
        store_.at(pos) = std::move(moved);
    }
    store_.popBack();
    posOf_[id.value] = 0xFFFFFFFFu;
}

void Document::removeEntities(std::span<const EntityId> ids, std::vector<Entity>& out) {
    constexpr std::uint32_t kGone = 0xFFFFFFFFu;
    std::vector<std::uint8_t> mark(posOf_.size(), 0);
    std::size_t n = 0;
    for (EntityId id : ids) {
        if (id.value < posOf_.size() && posOf_[id.value] != kGone && !mark[id.value]) {
            mark[id.value] = 1;
            ++n;
        }
    }
    if (n == 0)
        return;
    out.reserve(out.size() + n);
    const std::size_t total = store_.size();
    std::size_t w = 0;
    for (std::size_t r = 0; r < total; ++r) {
        Entity& e = store_.at(r);
        const std::uint64_t v = e.common.id.value;
        if (mark[v]) {
            out.push_back(std::move(e));
            posOf_[v] = kGone;
        } else {
            if (w != r) {
                store_.at(w) = std::move(e);
                posOf_[v] = static_cast<std::uint32_t>(w);
            }
            ++w;
        }
    }
    while (store_.size() > w)
        store_.popBack();
}

BlockId Document::ownerBlock(EntityId id) const {
    auto it = owner_.find(id);
    return it == owner_.end() ? BlockId{} : it->second;
}

std::int64_t Document::orderKey(EntityId id) const {
    auto it = orderOverride_.find(id);
    return it == orderOverride_.end() ? static_cast<std::int64_t>(id.value) : it->second;
}

std::vector<EntityId> Document::modelSpace() const {
    std::vector<EntityId> out;
    out.reserve(modelSpaceCount());
    for (std::uint64_t v = 1; v < posOf_.size(); ++v) {
        if (posOf_[v] == 0xFFFFFFFFu)
            continue;
        EntityId id{v};
        if (owner_.empty() || owner_.find(id) == owner_.end())
            out.push_back(id);
    }
    if (!orderOverride_.empty()) {
        // (keys looked up once per object, not once per comparison)
        std::vector<std::pair<std::int64_t, EntityId>> keyed;
        keyed.reserve(out.size());
        for (EntityId id : out)
            keyed.emplace_back(orderKey(id), id);
        std::stable_sort(keyed.begin(), keyed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < keyed.size(); ++i)
            out[i] = keyed[i].second;
    }
    return out;
}

std::vector<EntityId> Document::inDrawOrder(std::span<const EntityId> ids) const {
    std::vector<std::pair<std::int64_t, EntityId>> keyed;
    keyed.reserve(ids.size());
    for (EntityId id : ids) {
        if (!id || id.value >= posOf_.size() || posOf_[id.value] == 0xFFFFFFFFu)
            continue;
        if (!owner_.empty() && owner_.find(id) != owner_.end())
            continue;
        keyed.emplace_back(orderOverride_.empty() ? 0 : orderKey(id), id);
    }
    std::sort(keyed.begin(), keyed.end());  // (key, id): what the stable sort over ascending ids does
    keyed.erase(std::unique(keyed.begin(), keyed.end()), keyed.end());
    std::vector<EntityId> out;
    out.reserve(keyed.size());
    for (const auto& k : keyed)
        out.push_back(k.second);
    return out;
}

void Document::setBlockRow(const BlockDef* oldRow, const BlockDef* newRow) {
    if (oldRow) {
        for (EntityId id : oldRow->entities)
            owner_.erase(id);
    }
    if (newRow) {
        for (EntityId id : newRow->entities)
            owner_[id] = newRow->id;
    }
}

const Layer* Document::layerByName(LayerId parent, std::string_view name) const {
    return layers_.findByKey(layerKey(parent, name));
}

EffectiveLayer Document::effective(LayerId id) const {
    if (effectiveVersion_ != layers_.version) {
        effectiveCache_.clear();
        effectiveVersion_ = layers_.version;
    }
    auto hit = effectiveCache_.find(id);
    if (hit != effectiveCache_.end())
        return withActiveOnly(hit->second);
    EffectiveLayer e;
    const Layer* own = layer(id);
    if (!own)
        return e;
    e.id = id;
    bool haveColor = false, haveLt = false, haveLw = false;
    e.transparency = own->transparency;
    for (const Layer* l = own; l; l = l->parent ? layer(l->parent) : nullptr) {
        e.on = e.on && l->on;
        e.plot = e.plot && l->plot;
        e.frozen = e.frozen || l->frozen;
        e.locked = e.locked || l->locked;
        if (!haveColor && l->color.kind != ColorKind::ByParent) {
            e.color = l->color;
            haveColor = true;
        }
        if (!haveLt && l->linetype != kLinetypeByParent) {
            e.linetype = l->linetype;
            haveLt = true;
        }
        if (!haveLw && !l->lineweight.isByParent()) {
            e.lineweight = l->lineweight;
            haveLw = true;
        }
    }
    effectiveCache_.emplace(id, e);
    return withActiveOnly(e);
}

EffectiveLayer Document::effective(const EntityCommon& c) const {
    EffectiveLayer e = effective(c.layer);
    // ANNOALLVISIBLE off: an annotative object made for other scales is not there.
    if (c.annotative && !header_.custom.empty() && !annoAllVisible(header_) &&
        !supportsCurrentAnnotationScale(header_, c))
        e.on = false;
    const Level* lv = level(c.level);
    if (!lv)
        return e;
    if (!lv->visible) {
        e.on = false;
        return e;
    }
    const bool other = c.level != header_.currentLevel;
    if (other && header_.levelView == LevelView::HideOthers) {
        e.on = false;
        return e;
    }
    if (lv->locked)
        e.locked = true;
    if (other && header_.levelView == LevelView::DimOthers) {
        e.locked = true;
        e.otherLevel = true;
    }
    return e;
}

// The active-only override depends on the header, which the cache does not watch, so it is
// applied on every lookup.
EffectiveLayer Document::withActiveOnly(EffectiveLayer e) const {
    if (!header_.activeOnly || !e.id)
        return e;
    for (const Layer* l = layer(e.id); l; l = l->parent ? layer(l->parent) : nullptr)
        if (l->id == header_.currentLayer)
            return e;
    if (!e.locked)
        e.inactive = true;
    e.locked = true;
    return e;
}

Transaction Document::begin(std::string name) {
    if (active_)
        throw std::logic_error("a transaction is already open on this document");
    return Transaction(this, std::move(name));
}

void Document::addObserver(DocumentObserver* observer) {
    observers_.push_back(observer);
}

void Document::removeObserver(DocumentObserver* observer) {
    observers_.erase(std::remove(observers_.begin(), observers_.end(), observer), observers_.end());
}

void Document::notify(const ChangeSet& cs) {
    auto copy = observers_;
    for (DocumentObserver* o : copy) {
        if (std::find(observers_.begin(), observers_.end(), o) != observers_.end()) {
            o->onChanged(cs);
        }
    }
}

namespace {
template <class Row, class IdT>
void applyRows(detail::Table<Row, IdT>& t, const std::vector<RowChange<Row>>& changes,
               const std::function<void(const Row*, const Row*)>& hook = {}) {
    for (const auto& c : changes) {
        IdT id = c.after ? c.after->id : c.before->id;
        if (hook)
            hook(t.find(id), c.after ? &*c.after : nullptr);
        replaceRow(t, id, c.after);
    }
}
}  // namespace

void Document::applyForward(const ChangeSet& cs) {
    for (const EntityChange& c : cs.entities) {
        const Entity* after = cs.after(c);
        if (after) {
            if (Entity* live = findMutable(after->common.id)) {
                *live = *after;
            } else {
                insertEntity(Entity(*after));
            }
        } else {
            removeEntity(cs.before(c)->common.id);
        }
    }
    applyRows(layers_, cs.layers);
    applyRows(levels_, cs.levels);
    applyRows(linetypes_, cs.linetypes);
    applyRows(textStyles_, cs.textStyles);
    applyRows(dimStyles_, cs.dimStyles);
    applyRows<BlockDef, BlockId>(
        blocks_, cs.blocks, [this](const BlockDef* o, const BlockDef* n) { setBlockRow(o, n); });
    if (cs.header)
        header_ = cs.header->after;
    for (const OrderChange& o : cs.order) {
        if (o.after)
            orderOverride_[o.id] = *o.after;
        else
            orderOverride_.erase(o.id);
    }
}

void Document::apply(const ChangeSet& changes, Direction direction) {
    if (active_)
        throw std::logic_error("cannot undo or redo while a transaction is open");
    if (direction == Direction::Redo) {
        applyForward(changes);
        ++revision_;
        if (!changes.empty())
            notify(changes);
    } else {
        ChangeSet inv = changes.inverted();
        applyForward(inv);
        ++revision_;
        if (!inv.empty())
            notify(inv);
    }
}

// ---------------------------------------------------------------------------------------------
// Collaboration support

Result<void> Document::setEntityIdWindow(std::uint64_t first, std::uint64_t limit) {
    if (active_)
        return makeError(ErrorCode::Conflict, "cannot change the id window during a transaction");
    if (first == 0 || limit <= first)
        return makeError(ErrorCode::InvalidArgument, "empty entity id window");
    windowNext_ = first;
    windowLimit_ = limit;
    nextEntityId_ = std::max(nextEntityId_, first);
    return {};
}

void Document::clearEntityIdWindow() {
    if (active_)
        throw std::logic_error("cannot change the id window during a transaction");
    windowNext_ = windowLimit_ = 0;
}

std::uint64_t Document::entityIdsLeft() const {
    if (!windowLimit_)
        return std::numeric_limits<std::uint64_t>::max();
    return windowNext_ >= windowLimit_ ? 0 : windowLimit_ - windowNext_;
}

namespace {

// Two rows of the final table must not share a name.
template <class Row, class IdT>
Result<void> checkFinalNames(const detail::Table<Row, IdT>& t,
                             const std::vector<RowChange<Row>>& changes) {
    std::unordered_map<std::string, IdT> names;
    std::unordered_set<IdT> touched;
    for (const auto& c : changes)
        touched.insert(c.after ? c.after->id : c.before->id);
    for (const auto& [id, row] : t.rows) {
        if (!touched.count(id))
            names.emplace(nameKey(row), id);
    }
    for (const auto& c : changes) {
        if (!c.after)
            continue;
        if (!names.emplace(nameKey(*c.after), c.after->id).second)
            return makeError(ErrorCode::Conflict, "name already in use: " + c.after->name);
    }
    return {};
}

template <class Row, class IdT>
Result<void> checkRowIds(const detail::Table<Row, IdT>& t,
                         const std::vector<RowChange<Row>>& changes) {
    std::unordered_set<IdT> seen;
    for (const auto& c : changes) {
        if (!c.before && !c.after)
            return makeError(ErrorCode::InvalidArgument, "empty row change");
        IdT id = c.after ? c.after->id : c.before->id;
        if (!id || !seen.insert(id).second)
            return makeError(ErrorCode::InvalidArgument, "bad or repeated row id");
        if (c.before && c.after && c.before->id != c.after->id)
            return makeError(ErrorCode::InvalidArgument, "row id changed");
        if (c.before && !t.find(id))
            return makeError(ErrorCode::Conflict, "row to change does not exist");
        if (!c.before && t.find(id))
            return makeError(ErrorCode::Conflict, "row to add already exists");
        if (!c.after && isBuiltin(id))
            return makeError(ErrorCode::InvalidArgument, "a built-in row cannot be erased");
    }
    return {};
}

template <class Row>
bool anyErased(const std::vector<RowChange<Row>>& changes) {
    for (const auto& c : changes)
        if (!c.after)
            return true;
    return false;
}

template <class Row, class IdT>
Result<void> checkRowsAfter(const Document& d, const std::vector<RowChange<Row>>& changes) {
    for (const auto& c : changes) {
        if (!c.after)
            continue;
        Row copy = *c.after;
        const Row* old = c.before ? &*c.before : nullptr;
        if (Result<void> r = checkRow(d, copy, old); !r)
            return r;
    }
    return {};
}

}  // namespace

Result<void> Document::applyChecked(const ChangeSet& cs) {
    if (active_)
        return makeError(ErrorCode::Conflict, "cannot apply changes while a transaction is open");

    // 1. Checks that need only the document as it is now.
    std::unordered_set<EntityId> deltaIds, erasedIds;
    for (const EntityChange& c : cs.entities) {
        if (c.before == kNoSnapshot && c.after == kNoSnapshot)
            return makeError(ErrorCode::InvalidArgument, "empty entity change");
        const Entity* before = cs.before(c);
        const Entity* after = cs.after(c);
        EntityId id = after ? after->common.id : before->common.id;
        if (!id || (before && after && before->common.id != after->common.id))
            return makeError(ErrorCode::InvalidArgument, "bad entity id");
        if (!deltaIds.insert(id).second)
            return makeError(ErrorCode::InvalidArgument, "entity listed twice");
        if (before && !find(id))
            return makeError(ErrorCode::Conflict, "entity to change does not exist");
        if (!before && find(id))
            return makeError(ErrorCode::Conflict, "entity to add already exists");
        if (!after)
            erasedIds.insert(id);
    }
    if (Result<void> r = checkRowIds(layers_, cs.layers); !r)
        return r;
    if (Result<void> r = checkRowIds(linetypes_, cs.linetypes); !r)
        return r;
    if (Result<void> r = checkRowIds(textStyles_, cs.textStyles); !r)
        return r;
    if (Result<void> r = checkRowIds(dimStyles_, cs.dimStyles); !r)
        return r;
    if (Result<void> r = checkRowIds(blocks_, cs.blocks); !r)
        return r;
    if (Result<void> r = checkFinalNames(layers_, cs.layers); !r)
        return r;
    if (Result<void> r = checkFinalNames(linetypes_, cs.linetypes); !r)
        return r;
    if (Result<void> r = checkFinalNames(textStyles_, cs.textStyles); !r)
        return r;
    if (Result<void> r = checkFinalNames(dimStyles_, cs.dimStyles); !r)
        return r;
    if (Result<void> r = checkFinalNames(blocks_, cs.blocks); !r)
        return r;

    // Entities of a block belong to it for good: a block row may list entities that are new
    // in this change or already in the block, and must not list an entity it has lost without
    // erasing it.
    std::unordered_set<EntityId> listed;
    std::unordered_set<BlockId> blockRowsChanged;
    for (const auto& c : cs.blocks) {
        BlockId bid = c.after ? c.after->id : c.before->id;
        blockRowsChanged.insert(bid);
        if (!c.after)
            continue;
        for (EntityId e : c.after->entities) {
            if (!listed.insert(e).second)
                return makeError(ErrorCode::Conflict, "entity listed by two blocks");
            BlockId prev = ownerBlock(e);
            if (find(e) ? prev != bid : !deltaIds.count(e))
                return makeError(ErrorCode::Conflict, "block lists an entity it does not own");
        }
    }
    for (EntityId e : erasedIds) {
        BlockId prev = ownerBlock(e);
        if (!prev)
            continue;
        if (!blockRowsChanged.count(prev))
            return makeError(ErrorCode::Conflict, "block still lists an erased entity");
        for (const auto& c : cs.blocks) {
            if (c.after && c.after->id == prev &&
                std::find(c.after->entities.begin(), c.after->entities.end(), e) !=
                    c.after->entities.end())
                return makeError(ErrorCode::Conflict, "block still lists an erased entity");
        }
    }

    // 2. Apply, then check the result as a Transaction would have; undo if anything fails.
    applyForward(cs);
    auto fail = [&](Error err) -> Result<void> {
        applyForward(cs.inverted());
        return err;
    };
    if (cs.header) {
        if (Result<void> r = checkHeader(*this, header_); !r)
            return fail(r.error());
    }
    if (Result<void> r = checkRowsAfter<Layer, LayerId>(*this, cs.layers); !r)
        return fail(r.error());
    if (Result<void> r = checkRowsAfter<Linetype, LinetypeId>(*this, cs.linetypes); !r)
        return fail(r.error());
    if (Result<void> r = checkRowsAfter<TextStyle, TextStyleId>(*this, cs.textStyles); !r)
        return fail(r.error());
    if (Result<void> r = checkRowsAfter<DimStyle, DimStyleId>(*this, cs.dimStyles); !r)
        return fail(r.error());
    if (Result<void> r = checkRowsAfter<BlockDef, BlockId>(*this, cs.blocks); !r)
        return fail(r.error());
    for (const EntityChange& c : cs.entities) {
        const Entity* after = cs.after(c);
        if (!after)
            continue;
        Entity copy = *after;
        if (Result<double> r = validateEntity(*this, copy, ownerBlock(after->common.id)); !r)
            return fail(r.error());
    }
    // Erasing a row other objects still use leaves them dangling: look at every entity again.
    if (anyErased(cs.layers) || anyErased(cs.linetypes) || anyErased(cs.textStyles) ||
        anyErased(cs.dimStyles) || anyErased(cs.blocks)) {
        for (std::size_t i = 0; i < store_.size(); ++i) {
            Entity copy = store_.at(i);
            if (Result<double> r = validateEntity(*this, copy, ownerBlock(copy.common.id)); !r)
                return fail(r.error());
        }
    }
    ++revision_;
    if (!cs.empty())
        notify(cs);
    return {};
}

// ---------------------------------------------------------------------------------------------
// Transaction

Transaction::Transaction(Document* doc, std::string name)
    : doc_(doc), name_(std::move(name)), startNextId_(doc->allocCursor()) {
    doc->active_ = this;
}

Transaction::Transaction(Transaction&& o) noexcept
    : doc_(o.doc_),
      name_(std::move(o.name_)),
      startNextId_(o.startNextId_),
      beforeSnaps_(std::move(o.beforeSnaps_)),
      touched_(std::move(o.touched_)),
      outOfArea_(std::move(o.outOfArea_)),
      defaultOwner_(o.defaultOwner_),
      headerBefore_(std::move(o.headerBefore_)),
      order_(std::move(o.order_)),
      pLayers_(std::move(o.pLayers_)),
      pLevels_(std::move(o.pLevels_)),
      pLinetypes_(std::move(o.pLinetypes_)),
      pTextStyles_(std::move(o.pTextStyles_)),
      pDimStyles_(std::move(o.pDimStyles_)),
      pBlocks_(std::move(o.pBlocks_)) {
    o.doc_ = nullptr;
    if (doc_)
        doc_->active_ = this;
}

void Transaction::cacheObjectReferences(bool on) {
    cacheRefs_ = on;
    refs_.reset();
}

const detail::ObjectRefs* Transaction::objectRefs() {
    if (!cacheRefs_)
        return nullptr;
    if (!refs_)
        refs_ = scanObjectRefs(*doc_);
    return refs_.get();
}

Transaction::~Transaction() {
    if (doc_)
        rollback();
}

Document& Transaction::doc() const {
    if (!doc_)
        throw std::logic_error("transaction already finished");
    return *doc_;
}

std::vector<EntityId> Transaction::touchedEntities() const {
    std::vector<EntityId> ids;
    ids.reserve(beforeSnaps_.size());
    for (const Entity& snap : beforeSnaps_)
        ids.push_back(snap.common.id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

void Transaction::touchEntity(EntityId id, bool consume) {
    if ((id.value >= startNextId_ && id.value < doc_->allocCursor()) ||
        touched_.find(id) != TouchedIndex::kNone)
        return;
    Entity* e = doc_->findMutable(id);
    if (!e)
        return;
    touched_.insert(id, static_cast<std::uint32_t>(beforeSnaps_.size()));
    // An entity that is erased right after is moved into the snapshot instead of copied.
    if (consume)
        beforeSnaps_.push_back(std::move(*e));
    else
        beforeSnaps_.push_back(*e);
}

template <class Row, class IdT>
void Transaction::touchRow(detail::Table<Row, IdT>& t, Pending<Row, IdT>& p, IdT id) {
    if (p.rows.find(id) != p.rows.end())
        return;
    RowChange<Row> rc;
    if (const Row* r = t.find(id))
        rc.before = *r;
    p.rows.emplace(id, std::move(rc));
}

void Transaction::touchBlock(BlockId id) {
    touchRow(doc_->blocks_, pBlocks_, id);
}

void Transaction::touchOrder(EntityId id) {
    if (order_.count(id))
        return;
    OrderChange oc;
    oc.id = id;
    auto it = doc_->orderOverride_.find(id);
    if (it != doc_->orderOverride_.end())
        oc.before = it->second;
    order_.emplace(id, oc);
}

Result<EntityId> Transaction::add(Entity entity, BlockId owner) {
    Document& d = doc();
    refs_.reset();
    if (!owner)
        owner = defaultOwner_;
    if (owner && !d.block(owner))
        return makeError(ErrorCode::NotFound, "unknown block");
    Result<double> ext = validateEntity(d, entity, owner);
    if (!ext)
        return ext.error();
    if (d.windowLimit_ && d.windowNext_ >= d.windowLimit_)
        return makeError(ErrorCode::OutOfRange, "this peer has used up its range of entity ids");
    EntityId id{d.windowLimit_ ? d.windowNext_++ : d.nextEntityId_++};
    d.nextEntityId_ = std::max(d.nextEntityId_, id.value + 1);
    entity.common.id = id;
    if (ext.value() > d.header_.workArea)
        outOfArea_.push_back(id);
    d.insertEntity(std::move(entity));
    if (owner) {
        touchBlock(owner);
        d.blocks_.rows.find(owner)->second.entities.push_back(id);
        d.owner_[id] = owner;
    }
    return id;
}

Result<void> Transaction::modify(EntityId id, const std::function<void(Entity&)>& fn) {
    refs_.reset();
    Document& d = doc();
    Entity* live = d.findMutable(id);
    if (!live)
        return makeError(ErrorCode::NotFound, "unknown entity");
    Result<double> ext = 0.0;
    if (touched_.find(id) == TouchedIndex::kNone &&
        !(id.value >= startNextId_ && id.value < d.allocCursor())) {
        // First touch: the snapshot is the copy that keeps the original, so the entity is edited
        // in place and put back from the snapshot if the edit is not valid.
        touchEntity(id);
        fn(*live);
        live->common.id = id;
        ext = validateEntity(d, *live, d.ownerBlock(id));
        if (!ext) {
            *live = std::move(beforeSnaps_.back());
            beforeSnaps_.pop_back();
            touched_.erase(id);
            return ext.error();
        }
    } else {
        Entity work = *live;
        fn(work);
        work.common.id = id;
        ext = validateEntity(d, work, d.ownerBlock(id));
        if (!ext)
            return ext.error();
        touchEntity(id);  // live still holds the original state
        *live = std::move(work);
    }
    if (ext.value() > d.header_.workArea)
        outOfArea_.push_back(id);
    return {};
}

Result<void> Transaction::eraseEntityInternal(EntityId id) {
    Document& d = doc();
    if (!d.find(id))
        return makeError(ErrorCode::NotFound, "unknown entity");
    touchEntity(id, true);
    auto ow = d.owner_.find(id);
    if (ow != d.owner_.end()) {
        BlockId b = ow->second;
        touchBlock(b);
        auto& list = d.blocks_.rows.find(b)->second.entities;
        auto pos = std::find(list.rbegin(), list.rend(), id);
        if (pos != list.rend())
            list.erase(std::next(pos).base());
        d.owner_.erase(ow);
    }
    if (d.orderOverride_.count(id)) {
        touchOrder(id);
        d.orderOverride_.erase(id);
    }
    d.removeEntity(id);
    return {};
}

std::size_t Transaction::eraseMany(std::span<const EntityId> ids) {
    Document& d = doc();
    // Objects that sit directly in model space, were not edited yet in this transaction and keep
    // their default draw order go in one pass straight into the snapshots (in store order).
    // The few others (block members, sent to back, already touched, new) go one by one first.
    std::size_t n = 0;
    std::vector<EntityId> bulk;
    bulk.reserve(ids.size());
    for (EntityId id : ids) {
        const bool plain = id.value < startNextId_ && touched_.find(id) == TouchedIndex::kNone &&
                           !d.owner_.count(id) && !d.orderOverride_.count(id);
        if (plain) {
            bulk.push_back(id);
        } else if (eraseEntityInternal(id)) {
            ++n;
        }
    }
    const std::size_t first = beforeSnaps_.size();
    d.removeEntities(bulk, beforeSnaps_);
    touched_.reserve(touched_.size() + (beforeSnaps_.size() - first));
    for (std::size_t i = first; i < beforeSnaps_.size(); ++i)
        touched_.insert(beforeSnaps_[i].common.id, static_cast<std::uint32_t>(i));
    return n + (beforeSnaps_.size() - first);
}

std::size_t Transaction::eraseAllIn(BlockId block) {
    Document& d = doc();
    auto row = d.blocks_.rows.find(block);
    if (row == d.blocks_.rows.end() || row->second.entities.empty())
        return 0;
    touchBlock(block);
    row = d.blocks_.rows.find(block);
    std::vector<EntityId> ids = std::move(row->second.entities);
    row->second.entities.clear();
    std::size_t n = 0;
    for (EntityId id : ids) {
        if (!d.find(id))
            continue;
        touchEntity(id, true);
        d.owner_.erase(id);
        if (d.orderOverride_.count(id)) {
            touchOrder(id);
            d.orderOverride_.erase(id);
        }
        d.removeEntity(id);
        ++n;
    }
    return n;
}

Result<void> Transaction::erase(EntityId id) {
    doc();
    return eraseEntityInternal(id);
}

Result<void> Transaction::bringToFront(EntityId id) {
    Document& d = doc();
    if (!d.find(id))
        return makeError(ErrorCode::NotFound, "unknown entity");
    if (auto ow = d.owner_.find(id); ow != d.owner_.end()) {
        touchBlock(ow->second);
        auto& list = d.blocks_.rows.find(ow->second)->second.entities;
        list.erase(std::remove(list.begin(), list.end(), id), list.end());
        list.push_back(id);
        return {};
    }
    if (!nextFront_) {
        std::int64_t key = kFrontBase;
        for (const auto& [eid, k] : d.orderOverride_)
            if (eid != id && k >= kFrontBase)
                key = std::max(key, k + 1);
        nextFront_ = key;
    }
    touchOrder(id);
    d.orderOverride_[id] = (*nextFront_)++;
    return {};
}

Result<void> Transaction::sendToBack(EntityId id) {
    Document& d = doc();
    if (!d.find(id))
        return makeError(ErrorCode::NotFound, "unknown entity");
    if (auto ow = d.owner_.find(id); ow != d.owner_.end()) {
        touchBlock(ow->second);
        auto& list = d.blocks_.rows.find(ow->second)->second.entities;
        list.erase(std::remove(list.begin(), list.end(), id), list.end());
        list.insert(list.begin(), id);
        return {};
    }
    if (!nextBack_) {
        std::int64_t key = -kFrontBase;
        for (const auto& [eid, k] : d.orderOverride_)
            if (eid != id && k <= -kFrontBase)
                key = std::min(key, k - 1);
        nextBack_ = key;
    }
    touchOrder(id);
    d.orderOverride_[id] = (*nextBack_)--;
    return {};
}

Result<void> Transaction::setDrawOrder(const std::vector<EntityId>& order, BlockId block) {
    Document& d = doc();
    if (block) {
        const BlockDef* def = d.block(block);
        if (!def)
            return makeError(ErrorCode::NotFound, "unknown block");
        std::vector<EntityId> a = order, b = def->entities;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (a != b)
            return makeError(ErrorCode::InvalidArgument,
                             "the draw order is not the entities of the block");
        if (order == def->entities)
            return {};
        touchBlock(block);
        d.blocks_.rows.find(block)->second.entities = order;
        return {};
    }
    std::vector<EntityId> a = order, b = d.modelSpace();
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b)
        return makeError(ErrorCode::InvalidArgument,
                         "the draw order is not the entities of model space");
    // Entities without an override sit in id order. The longest ascending window of the order
    // can stay that way; what precedes it is sent to the back and what follows is brought to the
    // front.
    std::size_t bestStart = 0, bestLen = 0, runStart = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i > 0 && !(order[i - 1] < order[i]))
            runStart = i;
        if (i + 1 - runStart > bestLen) {
            bestLen = i + 1 - runStart;
            bestStart = runStart;
        }
    }
    for (std::size_t i = bestStart; i-- > 0;)
        if (auto r = sendToBack(order[i]); !r)
            return r;
    for (std::size_t i = bestStart + bestLen; i < order.size(); ++i)
        if (auto r = bringToFront(order[i]); !r)
            return r;
    return {};
}

// Tables ------------------------------------------------------------------------------------

template <class Row, class IdT>
Result<IdT> Transaction::addRow(detail::Table<Row, IdT>& t, Pending<Row, IdT>& p, Row row) {
    doc();
    if (Result<void> r = checkRow(*doc_, row, nullptr); !r)
        return r.error();
    if (t.byName.count(nameKey(row))) {
        return makeError(ErrorCode::Conflict, "name already in use: " + row.name);
    }
    IdT id{t.next};
    row.id = id;
    if constexpr (std::is_same_v<Row, BlockDef>)
        row.entities.clear();
    touchRow(t, p, id);
    ++t.next;
    replaceRow<Row, IdT>(t, id, std::optional<Row>(std::move(row)));
    return id;
}

template <class Row, class IdT>
Result<void> Transaction::modifyRow(detail::Table<Row, IdT>& t, Pending<Row, IdT>& p, IdT id,
                                    const std::function<void(Row&)>& fn) {
    doc();
    const Row* live = t.find(id);
    if (!live)
        return makeError(ErrorCode::NotFound, "unknown table row");
    Row work = *live;
    fn(work);
    work.id = id;
    if constexpr (std::is_same_v<Row, BlockDef>)
        work.entities = live->entities;
    if (Result<void> r = checkRow(*doc_, work, live); !r)
        return r.error();
    if (nameKey(work) != nameKey(*live) && t.byName.count(nameKey(work))) {
        return makeError(ErrorCode::Conflict, "name already in use: " + work.name);
    }
    touchRow(t, p, id);
    replaceRow<Row, IdT>(t, id, std::optional<Row>(std::move(work)));
    return {};
}

template <class Row, class IdT>
Result<void> Transaction::eraseRow(detail::Table<Row, IdT>& t, Pending<Row, IdT>& p, IdT id) {
    doc();
    if (!t.find(id))
        return makeError(ErrorCode::NotFound, "unknown table row");
    if (isBuiltin(id))
        return makeError(ErrorCode::Conflict, "built-in row cannot be deleted");
    if (inUse(*doc_, id, objectRefs()))
        return makeError(ErrorCode::Conflict, "row is in use");
    touchRow(t, p, id);
    replaceRow<Row, IdT>(t, id, std::nullopt);
    return {};
}

Result<LayerId> Transaction::addLayer(Layer r) {
    return addRow(doc().layers_, pLayers_, std::move(r));
}
Result<void> Transaction::modifyLayer(LayerId id, const std::function<void(Layer&)>& fn) {
    return modifyRow(doc().layers_, pLayers_, id, fn);
}
Result<void> Transaction::eraseLayer(LayerId id) {
    auto r = eraseRow(doc().layers_, pLayers_, id);
    if (r) {
        // A per-command layer setting must not point at a layer that is gone.
        const std::string text = std::to_string(id.value);
        for (const char* key : {kDimLayerKey, kHatchLayerKey, kTextLayerKey}) {
            auto it = doc().header().custom.find(key);
            if (it != doc().header().custom.end() && it->second == text)
                (void)modifyHeader([key](Header& h) { h.custom.erase(key); });
        }
    }
    return r;
}

Result<LevelId> Transaction::addLevel(Level r) {
    return addRow(doc().levels_, pLevels_, std::move(r));
}
Result<void> Transaction::modifyLevel(LevelId id, const std::function<void(Level&)>& fn) {
    return modifyRow(doc().levels_, pLevels_, id, fn);
}
Result<void> Transaction::eraseLevel(LevelId id) {
    return eraseRow(doc().levels_, pLevels_, id);
}

Result<LinetypeId> Transaction::addLinetype(Linetype r) {
    return addRow(doc().linetypes_, pLinetypes_, std::move(r));
}
Result<void> Transaction::modifyLinetype(LinetypeId id, const std::function<void(Linetype&)>& fn) {
    return modifyRow(doc().linetypes_, pLinetypes_, id, fn);
}
Result<void> Transaction::eraseLinetype(LinetypeId id) {
    return eraseRow(doc().linetypes_, pLinetypes_, id);
}

Result<TextStyleId> Transaction::addTextStyle(TextStyle r) {
    return addRow(doc().textStyles_, pTextStyles_, std::move(r));
}
Result<void> Transaction::modifyTextStyle(TextStyleId id,
                                          const std::function<void(TextStyle&)>& fn) {
    return modifyRow(doc().textStyles_, pTextStyles_, id, fn);
}
Result<void> Transaction::eraseTextStyle(TextStyleId id) {
    return eraseRow(doc().textStyles_, pTextStyles_, id);
}

Result<DimStyleId> Transaction::addDimStyle(DimStyle r) {
    return addRow(doc().dimStyles_, pDimStyles_, std::move(r));
}
Result<void> Transaction::modifyDimStyle(DimStyleId id, const std::function<void(DimStyle&)>& fn) {
    return modifyRow(doc().dimStyles_, pDimStyles_, id, fn);
}
Result<void> Transaction::eraseDimStyle(DimStyleId id) {
    return eraseRow(doc().dimStyles_, pDimStyles_, id);
}

Result<BlockId> Transaction::addBlock(BlockDef r) {
    return addRow(doc().blocks_, pBlocks_, std::move(r));
}
Result<void> Transaction::modifyBlock(BlockId id, const std::function<void(BlockDef&)>& fn) {
    return modifyRow(doc().blocks_, pBlocks_, id, fn);
}

Result<void> Transaction::eraseBlock(BlockId id) {
    Document& d = doc();
    const BlockDef* def = d.block(id);
    if (!def)
        return makeError(ErrorCode::NotFound, "unknown block");
    const detail::ObjectRefs* refs = objectRefs();
    if (inUse(d, id, refs))
        return makeError(ErrorCode::Conflict, "block is referenced by an insert");
    if (refs_) {
        // The inserts inside this block go with it: what they used is free again.
        for (EntityId eid : def->entities) {
            const Entity* e = d.find(eid);
            const auto* in = e ? entityAs<InsertData>(*e) : nullptr;
            if (!in)
                continue;
            if (auto it = refs_->blockUses.find(in->block.value);
                it != refs_->blockUses.end() && --it->second == 0)
                refs_->blockUses.erase(it);
        }
    }
    (void)eraseAllIn(id);
    touchBlock(id);
    replaceRow<BlockDef, BlockId>(d.blocks_, id, std::nullopt);
    return {};
}

Result<void> Transaction::modifyHeader(const std::function<void(Header&)>& fn) {
    Document& d = doc();
    Header work = d.header_;
    fn(work);
    if (Result<void> r = checkHeader(d, work); !r)
        return r;
    if (!headerBefore_)
        headerBefore_ = d.header_;
    d.header_ = std::move(work);
    return {};
}

// Commit / rollback -------------------------------------------------------------------------

namespace {
template <class Row, class IdT, class P>
void collectRows(const detail::Table<Row, IdT>& t, const P& pending,
                 std::vector<RowChange<Row>>& out) {
    for (const auto& [id, rc] : pending.rows) {
        RowChange<Row> c;
        c.before = rc.before;
        if (const Row* r = t.find(id))
            c.after = *r;
        if (c.before || c.after)
            out.push_back(std::move(c));
    }
}
}  // namespace

// Brings the definition points of associative dimensions along with the objects they follow. A
// point follows only while it still sits where the object's point was before this transaction (or
// already sits where it is now); a dimension moved or copied away from its objects loses the link.
void Transaction::followAssociations() {
    Document& d = doc();
    struct Plan {
        EntityId id;
        std::array<Vec2, 4> pts;
        std::array<DimAssoc, 4> assoc;
        bool moved;
        Vec2 shift{};  // average move of the followed points, for a text placed by hand
        int followed = 0;
        Vec2 first{};         // the move of the first followed point
        bool uniform = true;  // every followed point moved by the same vector
    };
    std::vector<Plan> plans;
    auto close = [](Vec2 a, Vec2 b) {
        return distance(a, b) <= 1e-7 * (1.0 + std::abs(a.x) + std::abs(a.y));
    };
    d.forEachEntity([&](const Entity& e) {
        const auto* dm = entityAs<DimensionData>(e);
        if (!dm)
            return;
        Plan p{e.common.id, dm->defPts, dm->assoc, false, {}, 0, {}, true};
        bool changed = false;
        for (std::size_t i = 0; i < 4; ++i) {
            DimAssoc& a = p.assoc[i];
            if (a.kind == DimAssoc::Kind::None)
                continue;
            const Entity* t = d.find(a.target);
            const auto live = t ? dimAssocPoint(*t, a) : std::nullopt;
            if (!live) {
                a = {};
                changed = true;
            } else if (close(p.pts[i], *live)) {
                continue;
            } else {
                const std::uint32_t at = touched_.find(a.target);
                std::optional<Vec2> before;
                if (at != TouchedIndex::kNone)
                    before = dimAssocPoint(beforeSnaps_[at], a);
                if (before && close(p.pts[i], *before)) {
                    const Vec2 move = *live - p.pts[i];
                    if (p.followed == 0)
                        p.first = move;
                    else if (!close(move, p.first))
                        p.uniform = false;
                    p.shift += move;
                    ++p.followed;
                    p.pts[i] = *live;
                    p.moved = true;
                } else {
                    a = {};
                }
                changed = true;
            }
        }
        if (changed)
            plans.push_back(p);
    });
    for (const Plan& p : plans) {
        (void)modify(p.id, [&](Entity& e) {
            auto* dm = entityAs<DimensionData>(e);
            dm->defPts = p.pts;
            dm->assoc = p.assoc;
            if (p.moved) {
                if (dm->textPosUser && p.followed > 0)
                    dm->textPos += p.shift * (1.0 / p.followed);
                // The whole measured geometry moved the same way (the object was moved, not
                // stretched): the dimension line goes along, so its distance from the object stays.
                int measured = 0;
                switch (dm->kind) {
                    case DimKind::Linear:
                    case DimKind::Aligned:
                        measured = 2;
                        break;
                    case DimKind::Angular3Point:
                        measured = 3;
                        break;
                    case DimKind::Angular2Line:
                        measured = 4;
                        break;
                    default:
                        break;
                }
                if (measured > 0 && p.followed == measured && p.uniform) {
                    if (dm->kind == DimKind::Linear || dm->kind == DimKind::Aligned)
                        dm->defPts[2] += p.first;
                    else if (dm->kind == DimKind::Angular3Point)
                        dm->defPts[3] += p.first;
                    else if (!dm->textPosUser)
                        dm->textPos += p.first;
                }
                if (DimRefreshFn f = dimRefresh())
                    f(*doc_, e);
            }
        });
    }
}

ChangeSet Transaction::commit() {
    Document& d = doc();
    if (!touched_.empty() || d.allocCursor() > startNextId_)
        followAssociations();
    ChangeSet cs;
    cs.name = name_;

    // Touched ids in id order. The snapshots are kept in the order the entities were touched,
    // which for a bulk edit of the whole drawing is already ascending: sort only if it is not.
    std::vector<std::pair<EntityId, std::uint32_t>> touchedIds;  // id, index into beforeSnaps_
    touchedIds.reserve(beforeSnaps_.size());
    for (std::uint32_t i = 0; i < beforeSnaps_.size(); ++i)
        touchedIds.emplace_back(beforeSnaps_[i].common.id, i);
    if (auto brk = std::is_sorted_until(touchedIds.begin(), touchedIds.end());
        brk != touchedIds.end()) {
        // (two ascending runs, as an erase of everything with a few special objects leaves them,
        // merge in linear time)
        if (std::is_sorted(brk, touchedIds.end()))
            std::inplace_merge(touchedIds.begin(), brk, touchedIds.end());
        else
            std::sort(touchedIds.begin(), touchedIds.end());
    }

    std::size_t added = 0;
    for (std::uint64_t v = startNextId_; v < d.allocCursor(); ++v) {
        if (v < d.posOf_.size() && d.posOf_[v] != 0xFFFFFFFFu)
            ++added;
    }
    // The snapshots taken as the entities were touched are the "before" half as they lie; the
    // "after" copies are appended behind them. (Nothing is moved for an erase of everything.)
    std::size_t live = 0;
    for (const auto& kv : touchedIds)
        if (d.find(kv.first))
            ++live;
    auto snaps = std::make_shared<std::vector<Entity>>(std::move(beforeSnaps_));
    snaps->reserve(snaps->size() + live + added);
    cs.entities.reserve(touchedIds.size() + added);

    for (const auto& [id, at] : touchedIds) {
        EntityChange c;
        c.before = at;
        if (const Entity* lv = d.find(id)) {
            c.after = static_cast<std::uint32_t>(snaps->size());
            snaps->push_back(*lv);
        }
        cs.entities.push_back(c);
    }
    for (std::uint64_t v = startNextId_; v < d.allocCursor(); ++v) {
        if (v >= d.posOf_.size() || d.posOf_[v] == 0xFFFFFFFFu)
            continue;
        EntityChange c;
        c.after = static_cast<std::uint32_t>(snaps->size());
        snaps->push_back(d.store_.at(d.posOf_[v]));
        cs.entities.push_back(c);
    }
    cs.snapshots = std::move(snaps);

    collectRows(d.layers_, pLayers_, cs.layers);
    collectRows(d.levels_, pLevels_, cs.levels);
    collectRows(d.linetypes_, pLinetypes_, cs.linetypes);
    collectRows(d.textStyles_, pTextStyles_, cs.textStyles);
    collectRows(d.dimStyles_, pDimStyles_, cs.dimStyles);
    collectRows(d.blocks_, pBlocks_, cs.blocks);
    if (headerBefore_)
        cs.header = HeaderChange{*headerBefore_, d.header_};
    for (auto& [id, oc] : order_) {
        auto it = d.orderOverride_.find(id);
        oc.after = it == d.orderOverride_.end() ? std::nullopt : std::optional(it->second);
        if (oc.before != oc.after)
            cs.order.push_back(oc);
    }

    std::sort(outOfArea_.begin(), outOfArea_.end());
    outOfArea_.erase(std::unique(outOfArea_.begin(), outOfArea_.end()), outOfArea_.end());
    for (EntityId id : outOfArea_) {
        const Entity* live = d.find(id);
        if (!live)
            continue;
        Entity copy = *live;
        Result<double> ext = validateEntity(d, copy, d.ownerBlock(id));
        if (ext && ext.value() > d.header_.workArea)
            cs.outOfArea.push_back(id);
    }

    d.active_ = nullptr;
    doc_ = nullptr;
    ++d.revision_;
    if (!cs.empty())
        d.notify(cs);
    return cs;
}

void Transaction::rollback() {
    if (!doc_)
        return;
    Document& d = *doc_;

    // Entities added by this transaction.
    for (std::uint64_t v = startNextId_; v < d.allocCursor(); ++v) {
        EntityId id{v};
        if (d.find(id)) {
            d.owner_.erase(id);
            d.removeEntity(id);
        }
    }
    // Pre-existing entities that were modified or erased.
    for (const Entity& snap : beforeSnaps_) {
        if (Entity* live = d.findMutable(snap.common.id))
            *live = snap;
        else
            d.insertEntity(Entity(snap));
    }
    auto restore = [](auto& table, auto& pending) {
        for (auto& [id, rc] : pending.rows)
            replaceRow(table, id, rc.before);
    };
    restore(d.layers_, pLayers_);
    restore(d.levels_, pLevels_);
    restore(d.linetypes_, pLinetypes_);
    restore(d.textStyles_, pTextStyles_);
    restore(d.dimStyles_, pDimStyles_);
    for (auto& [id, rc] : pBlocks_.rows) {
        d.setBlockRow(d.block(id), rc.before ? &*rc.before : nullptr);
        replaceRow<BlockDef, BlockId>(d.blocks_, id, rc.before);
    }
    if (headerBefore_)
        d.header_ = *headerBefore_;
    for (const auto& [id, oc] : order_) {
        if (oc.before)
            d.orderOverride_[id] = *oc.before;
        else
            d.orderOverride_.erase(id);
    }
    d.active_ = nullptr;
    doc_ = nullptr;
}

namespace {
bool customFlag(const Header& h, const char* key, bool fallback) {
    auto it = h.custom.find(key);
    if (it == h.custom.end())
        return fallback;
    return it->second != "0";
}
}  // namespace

BBox2 drawingLimits(const Header& h) {
    BBox2 box;
    auto it = h.custom.find(kLimitsKey);
    if (it == h.custom.end())
        return box;
    double v[4];
    const char* p = it->second.c_str();
    for (double& x : v) {
        char* end = nullptr;
        x = std::strtod(p, &end);
        if (end == p || !std::isfinite(x))
            return BBox2{};
        p = end;
    }
    if (!(v[0] < v[2]) || !(v[1] < v[3]))
        return BBox2{};
    box.min = {v[0], v[1]};
    box.max = {v[2], v[3]};
    return box;
}

void setDrawingLimits(Header& h, Vec2 lowerLeft, Vec2 upperRight) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%.17g %.17g %.17g %.17g", lowerLeft.x, lowerLeft.y,
                  upperRight.x, upperRight.y);
    h.custom[kLimitsKey] = buf;
}

bool quickText(const Header& h) {
    return customFlag(h, kQTextKey, false);
}

namespace {
std::string layerStyleKey(LayerId l) {
    return "tbcad.pstyle.layer:" + std::to_string(l.value);
}
std::string entityStyleKey(EntityId e) {
    return "tbcad.pstyle.ent:" + std::to_string(e.value);
}
std::string customString(const Header& h, const std::string& key) {
    auto it = h.custom.find(key);
    return it == h.custom.end() ? std::string() : it->second;
}
void setCustomString(Header& h, const std::string& key, const std::string& v) {
    if (v.empty())
        h.custom.erase(key);
    else
        h.custom[key] = v;
}
bool isByLayerName(const std::string& n) {
    if (n.size() != 7)
        return false;
    const char* want = "bylayer";
    for (std::size_t i = 0; i < 7; ++i)
        if (std::tolower(static_cast<unsigned char>(n[i])) != want[i])
            return false;
    return true;
}
}  // namespace

bool colorDependentPlotStyles(const Header& h) {
    return customFlag(h, kPStyleModeKey, true);
}

void setColorDependentPlotStyles(Header& h, bool on) {
    if (on)
        h.custom.erase(kPStyleModeKey);
    else
        h.custom[kPStyleModeKey] = "0";
}

std::string layerPlotStyle(const Header& h, LayerId layer) {
    return customString(h, layerStyleKey(layer));
}

void setLayerPlotStyle(Header& h, LayerId layer, const std::string& name) {
    setCustomString(h, layerStyleKey(layer), name);
}

std::string entityPlotStyle(const Header& h, EntityId id) {
    return customString(h, entityStyleKey(id));
}

void setEntityPlotStyle(Header& h, EntityId id, const std::string& name) {
    setCustomString(h, entityStyleKey(id), isByLayerName(name) ? std::string() : name);
}

std::string resolvedPlotStyle(const Header& h, EntityId id, LayerId layer) {
    std::string own = entityPlotStyle(h, id);
    if (!own.empty())
        return own;
    std::string byLayer = layerPlotStyle(h, layer);
    return byLayer.empty() ? "Normal" : byLayer;
}

bool fillMode(const Header& h) {
    return customFlag(h, kFillModeKey, true);
}

int attDisplay(const Header& h) {
    auto it = h.custom.find(kAttDispKey);
    if (it == h.custom.end())
        return 0;
    return it->second == "1" ? 1 : it->second == "2" ? 2 : 0;
}

bool limitsCheck(const Header& h) {
    return customFlag(h, kLimCheckKey, false);
}

double ceLtScale(const Header& h) {
    auto it = h.custom.find(kCeLtScaleKey);
    if (it == h.custom.end())
        return 1.0;
    char* end = nullptr;
    double v = std::strtod(it->second.c_str(), &end);
    return end != it->second.c_str() && std::isfinite(v) && v > 0 ? v : 1.0;
}
bool psLtScale(const Header& h) {
    return customFlag(h, kPsLtScaleKey, true);
}
bool msLtScale(const Header& h) {
    return customFlag(h, kMsLtScaleKey, true);
}
bool plineGen(const Header& h) {
    return customFlag(h, kPlineGenKey, true);
}
bool mirrText(const Header& h) {
    return customFlag(h, kMirrTextKey, false);
}

double currentAnnotationScale(const Header& h) {
    auto it = h.custom.find(kAnnoScaleKey);
    if (it == h.custom.end())
        return 1.0;
    char* end = nullptr;
    double v = std::strtod(it->second.c_str(), &end);
    return end != it->second.c_str() && std::isfinite(v) && v > 0 ? v : 1.0;
}

std::vector<double> annotationScaleList(const Header& h) {
    std::vector<double> out;
    auto it = h.custom.find(kAnnoScaleListKey);
    if (it != h.custom.end()) {
        const std::string& s = it->second;
        std::size_t at = 0;
        while (at <= s.size()) {
            std::size_t comma = s.find(',', at);
            std::string part = s.substr(at, comma == std::string::npos ? comma : comma - at);
            char* end = nullptr;
            double v = std::strtod(part.c_str(), &end);
            if (end != part.c_str() && std::isfinite(v) && v > 0)
                out.push_back(v);
            if (comma == std::string::npos)
                break;
            at = comma + 1;
        }
    }
    if (out.empty())
        out = {1, 2, 5, 10, 20, 25, 50, 75, 100, 200, 250, 500, 1000};
    return out;
}

bool annoAutoScale(const Header& h) {
    return customFlag(h, kAnnoAutoScaleKey, true);
}
bool annoAllVisible(const Header& h) {
    return customFlag(h, kAnnoAllVisibleKey, true);
}
bool selectionAnnoDisplay(const Header& h) {
    return customFlag(h, kSelAnnoDisplayKey, true);
}

bool supportsCurrentAnnotationScale(const Header& h, const EntityCommon& c) {
    if (!c.annotative || c.annoScales.empty())
        return true;
    const double cur = currentAnnotationScale(h);
    return std::find(c.annoScales.begin(), c.annoScales.end(), cur) != c.annoScales.end();
}

// A viewport draws at its own scale. In model space an object is drawn at the current annotation
// scale when it supports it (or lists none); otherwise at the first scale it has.
double annotationFactor(const Header& h, const EntityCommon& c, double override) {
    if (!c.annotative)
        return 1.0;
    if (override > 0)
        return override;
    if (!supportsCurrentAnnotationScale(h, c) && c.annoScales.front() > 0)
        return c.annoScales.front();
    return currentAnnotationScale(h);
}

LayerId commandLayerFor(const Header& h, const Entity& e) {
    if (h.custom.empty())
        return {};
    const char* key = nullptr;
    switch (kind(e)) {
        case EntityKind::Dimension:
        case EntityKind::Leader:
        case EntityKind::MLeader:
            key = kDimLayerKey;
            break;
        case EntityKind::Hatch:
            key = kHatchLayerKey;
            break;
        case EntityKind::Text:
            if (const auto* t = entityAs<TextData>(e); t && t->attdef)
                return {};
            key = kTextLayerKey;
            break;
        case EntityKind::MText:
            key = kTextLayerKey;
            break;
        default:
            return {};
    }
    auto it = h.custom.find(key);
    if (it == h.custom.end() || it->second.empty())
        return {};
    return LayerId{static_cast<std::uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10))};
}

bool annotateNewObjects(const Header& h) {
    auto it = h.custom.find(kAnnoNewKey);
    return it != h.custom.end() && it->second == "1";
}

}  // namespace tbcad
