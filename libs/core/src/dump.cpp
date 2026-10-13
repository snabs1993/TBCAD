#include "tbcad/core/dump.h"

#include <algorithm>
#include <type_traits>

#include "tbcad/core/units.h"

namespace tbcad {

namespace {

class Out {
public:
    Out& operator<<(const char* s) {
        s_ += s;
        return *this;
    }
    Out& operator<<(const std::string& s) {
        s_ += s;
        return *this;
    }
    Out& operator<<(double v) {
        s_ += formatDouble(v);
        return *this;
    }
    Out& operator<<(bool v) {
        s_ += v ? "1" : "0";
        return *this;
    }
    template <class T, class = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
    Out& operator<<(T v) {
        s_ += std::to_string(v);
        return *this;
    }
    Out& operator<<(const Vec2& v) { return *this << "(" << v.x << "," << v.y << ")"; }
    Out& operator<<(const Color& c) {
        return *this << static_cast<int>(c.kind) << ":" << static_cast<int>(c.r) << "."
                     << static_cast<int>(c.g) << "." << static_cast<int>(c.b);
    }
    template <class Tag, class Rep>
    Out& operator<<(StrongId<Tag, Rep> id) {
        return *this << id.value;
    }
    // Strings are quoted and escaped so line breaks and quotes cannot confuse the dump.
    Out& quoted(const std::string& s) {
        s_ += '"';
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') {
                s_ += '\\';
                s_ += static_cast<char>(c);
            } else if (c < 0x20 || c >= 0x7F) {
                static const char* hex = "0123456789abcdef";
                s_ += "\\x";
                s_ += hex[c >> 4];
                s_ += hex[c & 15];
            } else {
                s_ += static_cast<char>(c);
            }
        }
        s_ += '"';
        return *this;
    }
    std::string& str() { return s_; }

private:
    std::string s_;
};

void polyV(Out& o, const std::vector<PolyVertex>& v) {
    for (const auto& p : v) {
        o << " [" << p.p << " b" << p.bulge << " w" << p.startWidth << "/" << p.endWidth << "]";
    }
}

void splineD(Out& o, const SplineData& d) {
    o << "deg" << d.degree << " closed" << d.closed << " periodic" << d.periodic << " knots";
    for (double k : d.knots)
        o << " " << k;
    o << " ctrl";
    for (const auto& c : d.control)
        o << " " << c;
    o << " w";
    for (double w : d.weights)
        o << " " << w;
    o << " fit";
    for (const auto& c : d.fit)
        o << " " << c;
}

void style(Out& o, const DisplayStyle& st) {
    o << "#" << static_cast<int>(st.color.r) << "." << static_cast<int>(st.color.g) << "."
      << static_cast<int>(st.color.b) << "." << static_cast<int>(st.color.a) << " lw"
      << st.lineweight << " L" << st.layer << " sc" << st.patternScale;
    if (st.pattern) {
        o << " pat";
        for (double d : *st.pattern)
            o << "," << d;
    }
}

void display(Out& o, const DisplayList& g) {
    for (const auto& item : g.items) {
        std::visit(
            [&](const auto& it) {
                using I = std::decay_t<decltype(it)>;
                if constexpr (std::is_same_v<I, DisplayFill>) {
                    o << " fill{";
                    for (const auto& ring : it.rings) {
                        o << "ring";
                        for (const auto& p : ring)
                            o << " " << p;
                        o << ";";
                    }
                    o << "}";
                } else {
                    o << (std::is_same_v<I, DisplayPoints> ? " points{" : " poly{");
                    for (const auto& p : it.pts)
                        o << p << " ";
                    if constexpr (std::is_same_v<I, DisplayPolyline>)
                        o << "closed" << it.closed;
                    o << "}";
                }
                style(o, it.style);
            },
            item);
    }
}

struct DataDumper {
    Out& o;
    void operator()(const LineData& d) { o << "LINE " << d.a << " " << d.b; }
    void operator()(const CircleData& d) { o << "CIRCLE " << d.center << " r" << d.radius; }
    void operator()(const ArcData& d) {
        o << "ARC " << d.center << " r" << d.radius << " " << d.startAngle << " " << d.endAngle;
    }
    void operator()(const PolylineData& d) {
        o << "POLYLINE closed" << d.closed << " cw" << d.constantWidth;
        polyV(o, d.v);
    }
    void operator()(const PointData& d) { o << "POINT " << d.p; }
    void operator()(const EllipseData& d) {
        o << "ELLIPSE " << d.center << " " << d.majorAxis << " ratio" << d.ratio << " "
          << d.startParam << " " << d.endParam;
    }
    void operator()(const SplineData& d) {
        o << "SPLINE ";
        splineD(o, d);
    }
    void operator()(const TextData& d) {
        o << "TEXT " << d.position << " " << d.alignPoint << " h" << d.height << " rot"
          << d.rotation << " wf" << d.widthFactor << " ob" << d.oblique << " style" << d.style
          << " " << static_cast<int>(d.h) << "/" << static_cast<int>(d.v) << " ";
        o.quoted(d.utf8);
        if (d.attdef) {
            o << " ATTDEF tag=";
            o.quoted(d.attdef->tag);
            o << " prompt=";
            o.quoted(d.attdef->prompt);
            o << " default=";
            o.quoted(d.attdef->defaultValue);
            o << " flags" << (d.attdef->invisible ? "I" : "") << (d.attdef->constant ? "C" : "")
              << (d.attdef->verify ? "V" : "") << (d.attdef->preset ? "P" : "");
        }
    }
    void operator()(const MTextData& d) {
        o << "MTEXT " << d.position << " h" << d.height << " w" << d.width << " rot" << d.rotation
          << " att" << static_cast<int>(d.attach) << " style" << d.style << " ";
        o.quoted(d.utf8);
    }
    void operator()(const HatchData& d) {
        o << "HATCH solid" << d.solid << " pat=";
        o.quoted(d.pattern);
        o << " sc" << d.patternScale << " ang" << d.patternAngle << " org" << d.patternOrigin
          << " fill" << d.fill << " src";
        for (EntityId e : d.sourceBoundary)
            o << " " << e;
        for (const auto& loop : d.loops) {
            o << " loop(outer" << loop.outer;
            for (const auto& e : loop.edges) {
                std::visit(
                    [&](const auto& edge) {
                        using E = std::decay_t<decltype(edge)>;
                        if constexpr (std::is_same_v<E, HatchLineEdge>) {
                            o << " L" << edge.a << edge.b;
                        } else if constexpr (std::is_same_v<E, HatchArcEdge>) {
                            o << " A" << edge.center << " r" << edge.radius << " "
                              << edge.startAngle << " " << edge.endAngle << " ccw" << edge.ccw;
                        } else if constexpr (std::is_same_v<E, HatchEllipseEdge>) {
                            o << " E" << edge.center << edge.majorAxis << " ra" << edge.ratio << " "
                              << edge.startParam << " " << edge.endParam << " ccw" << edge.ccw;
                        } else if constexpr (std::is_same_v<E, HatchSplineEdge>) {
                            o << " S{";
                            splineD(o, edge.spline);
                            o << "}";
                        } else {
                            o << " P closed" << edge.closed;
                            polyV(o, edge.v);
                        }
                    },
                    e);
            }
            o << ")";
        }
    }
    void operator()(const InsertData& d) {
        o << "INSERT block" << d.block << " " << d.position << " sc" << d.scale << " rot"
          << d.rotation;
        for (const AttribValue& a : d.attribs) {
            o << " ATTRIB ";
            o.quoted(a.tag);
            o << "=";
            o.quoted(a.value);
        }
    }
    void operator()(const DimensionData& d) {
        o << "DIM kind" << static_cast<int>(d.kind) << " rot" << d.rotation;
        for (const auto& p : d.defPts)
            o << " " << p;
        o << " text" << d.textPos << " user" << d.textPosUser << " ";
        o.quoted(d.textOverride);
        o << " style" << d.style << " m" << d.measurement;
    }
    void operator()(const LeaderData& d) {
        o << "LEADER style" << d.style << " text" << d.textPos << " ";
        o.quoted(d.utf8);
        for (const auto& p : d.vertices)
            o << " " << p;
    }
    void operator()(const ProxyData& d) {
        o << "PROXY ";
        o.quoted(d.originalType);
        o << " raw" << d.raw.size() << ":";
        for (std::uint8_t b : d.raw)
            o << static_cast<int>(b) << ",";
        display(o, d.graphics);
    }
};

void common(Out& o, const EntityCommon& c) {
    o << "e" << c.id << " L" << c.layer << " lt" << c.linetype << " c" << c.color << " lw"
      << static_cast<int>(c.lineweight.v) << " ls" << c.linetypeScale << " vis" << c.visible << " ";
}

}  // namespace

std::string dumpEntity(const Entity& e) {
    Out o;
    common(o, e.common);
    visitEntity(e, DataDumper{o});
    return std::move(o.str());
}

std::string canonicalDump(const Document& doc) {
    Out o;
    const Header& h = doc.header();
    o << "header units" << static_cast<int>(h.units) << " workArea" << h.workArea << " ltScale"
      << h.ltScale << " layer" << h.currentLayer << " textStyle" << h.currentTextStyle
      << " dimStyle" << h.currentDimStyle << " createdBy=";
    o.quoted(h.createdBy);
    o << "\n";
    for (const auto& [k, v] : h.custom) {
        o << "custom ";
        o.quoted(k);
        o << "=";
        o.quoted(v);
        o << "\n";
    }
    for (const auto& [id, r] : doc.layers()) {
        o << "layer " << id << " ";
        o.quoted(r.name);
        o << " parent" << r.parent << " color" << r.color << " lt" << r.linetype << " lw"
          << static_cast<int>(r.lineweight.v) << " on" << r.on << " frozen" << r.frozen << " locked"
          << r.locked << " plot" << r.plot << "\n";
    }
    for (const auto& [id, r] : doc.linetypes()) {
        o << "linetype " << id << " ";
        o.quoted(r.name);
        o << " ";
        o.quoted(r.description);
        for (double d : r.pattern)
            o << " " << d;
        o << "\n";
    }
    for (const auto& [id, r] : doc.textStyles()) {
        o << "textstyle " << id << " ";
        o.quoted(r.name);
        o << " ";
        o.quoted(r.fontFamily);
        o << " ";
        o.quoted(r.fontFile);
        o << " wf" << r.widthFactor << " ob" << r.oblique << "\n";
    }
    for (const auto& [id, r] : doc.dimStyles()) {
        o << "dimstyle " << id << " ";
        o.quoted(r.name);
        o << " th" << r.textHeight << " ts" << r.textStyle << " gap" << r.textGap << " arrow"
          << static_cast<int>(r.arrowType) << "/" << r.arrowSize << " off" << r.extLineOffset
          << " ext" << r.extLineExtension << " dext" << r.dimLineExtension << " place"
          << static_cast<int>(r.textPlacement) << " align" << static_cast<int>(r.textAlign)
          << " units" << static_cast<int>(r.units) << " prec" << r.precision << " pre=";
        o.quoted(r.prefix);
        o << " suf=";
        o.quoted(r.suffix);
        o << " round" << r.roundOff << " scale" << r.overallScale << " slz"
          << r.suppressLeadingZeros << " stz" << r.suppressTrailingZeros << " c" << r.dimLineColor
          << "/" << r.extLineColor << "/" << r.textColor << "\n";
    }
    for (const auto& [id, r] : doc.blocks()) {
        o << "block " << id << " ";
        o.quoted(r.name);
        o << " base" << r.base << " anon" << r.anonymous << " members";
        for (EntityId e : r.entities)
            o << " " << e;
        o << "\n";
    }
    // Entities in id order.
    std::vector<EntityId> ids;
    ids.reserve(doc.entityCount());
    doc.forEachEntity([&](const Entity& e) { ids.push_back(e.common.id); });
    std::sort(ids.begin(), ids.end());
    for (EntityId id : ids) {
        o << dumpEntity(*doc.find(id));
        if (BlockId b = doc.ownerBlock(id))
            o << " owner" << b;
        o << "\n";
    }
    // Draw order of model space, only when it deviates from creation order.
    std::vector<EntityId> order = doc.modelSpace();
    if (!std::is_sorted(order.begin(), order.end())) {
        o << "draworder";
        for (EntityId e : order)
            o << " " << e;
        o << "\n";
    }
    return std::move(o.str());
}

}  // namespace tbcad
