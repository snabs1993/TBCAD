// DXF export: entity records.
#include <algorithm>
#include <cmath>

#include "tbcad/geom/entity.h"
#include "writer_impl.h"

namespace tbcad::dxf::detail {

namespace {

constexpr double kDeg = 180.0 / kPi;

// Plain text of TBCAD markup: line breaks kept as '\n', formatting codes and groups removed.
std::string plainLines(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char k = s[i + 1];
            if (k == '\\' || k == '{' || k == '}') {
                out.push_back(k);
                ++i;
            } else if (k == 'P') {
                out.push_back('\n');
                ++i;
            } else {
                // Formatting code: skip to its ';'.
                std::size_t semi = s.find(';', i);
                i = semi == std::string_view::npos ? s.size() : semi;
            }
        } else if (c == '{' || c == '}' || c == '\r') {
            continue;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// TBCAD markup -> DXF MTEXT markup (before the text is escaped for the file's encoding).
std::string toDxfMarkup(std::string_view s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '\n') {
            out += "\\P";
            ++i;
        } else if (c == '\r') {
            ++i;
        } else if (c == '\\' && i + 1 < s.size()) {
            char k = s[i + 1];
            if ((k == 'b' || k == 'i') && i + 3 < s.size() + 0 &&
                (s[i + 2] == '0' || s[i + 2] == '1') && s[i + 3] == ';') {
                // Gather adjacent bold/italic codes into one font code.
                bool bold = false, italic = false;
                while (i + 3 < s.size() && s[i] == '\\' && (s[i + 1] == 'b' || s[i + 1] == 'i') &&
                       (s[i + 2] == '0' || s[i + 2] == '1') && s[i + 3] == ';') {
                    (s[i + 1] == 'b' ? bold : italic) = s[i + 2] == '1';
                    i += 4;
                }
                out += std::string("\\fArial|b") + (bold ? "1" : "0") + "|i" +
                       (italic ? "1" : "0") + "|c0|p0;";
            } else if (k == 'C') {
                std::size_t semi = s.find(';', i);
                if (semi == std::string_view::npos) {
                    i = s.size();
                    continue;
                }
                int idx = std::atoi(std::string(s.substr(i + 2, semi - i - 2)).c_str());
                out += "\\C" + std::to_string(aciForIndex(idx).aci) + ";";
                i = semi + 1;
            } else {
                out.push_back(c);
                out.push_back(k);
                i += 2;
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

// Splits an already escaped string in pieces of at most `n` characters without cutting a \U+XXXX.
std::vector<std::string> chunks(const std::string& s, std::size_t n) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t len = std::min(n, s.size() - pos);
        if (pos + len < s.size()) {
            // Do not end inside an escape: look back for a backslash in the last 6 characters.
            for (std::size_t back = 1; back <= 6 && back <= len; ++back) {
                if (s[pos + len - back] == '\\') {
                    if (back < 7 && s.compare(pos + len - back, 3, "\\U+") == 0 && back < 7)
                        len -= back;
                    break;
                }
            }
            if (len == 0)
                len = std::min(n, s.size() - pos);
        }
        out.push_back(s.substr(pos, len));
        pos += len;
    }
    if (out.empty())
        out.push_back("");
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Common parts

Props Writer::propsOf(const EntityCommon& c) {
    Props p;
    p.layer = layerNameOf(c.layer);
    p.linetype = linetypeNameOf(c.linetype);
    p.color = c.color;
    p.lineweight = c.lineweight;
    p.ltScale = c.linetypeScale;
    p.visible = c.visible;
    return p;
}

Props Writer::propsOfStyle(const DisplayStyle& st, const std::string& fallbackLayer) {
    Props p;
    p.layer = st.layer ? layerNameOf(st.layer) : fallbackLayer;
    p.trueColor = st.color;
    int hundredths = static_cast<int>(std::lround(st.lineweight * 100.0));
    p.lineweight = LineWeight::hundredths(static_cast<std::int16_t>(validLineweight(hundredths)));
    if (st.pattern) {
        auto it = ltByPattern_.find(st.pattern);
        if (it != ltByPattern_.end()) {
            p.linetype = it->second;
            p.ltScale = st.patternScale;
        }
    }
    return p;
}

void Writer::sub(std::string_view subclass) {
    if (!r12_ && !subclass.empty())
        o_->raw(100, subclass);
}

void Writer::colorGroups(const Props& p) {
    Out& o = *o_;
    if (p.trueColor) {
        AciChoice c = aciForRgba(*p.trueColor);
        o.i(62, c.aci);
        if (!r12_ && c.rgb)
            o.i(420, *c.rgb);
        return;
    }
    switch (p.color.kind) {
        case ColorKind::ByBlock:
            o.i(62, 0);
            break;
        case ColorKind::Index:
        case ColorKind::Rgb: {
            AciChoice c = aciFor(p.color);
            o.i(62, c.aci);
            if (!r12_ && c.rgb)
                o.i(420, *c.rgb);
            break;
        }
        default:
            break;  // ByLayer is the default
    }
}

void Writer::head(std::string_view type, const Props& p, std::uint64_t owner,
                  std::string_view subclass) {
    Out& o = *o_;
    o.zero(type);
    if (!r12_) {
        o.h(5, newHandle());
        o.h(330, owner);
        o.raw(100, "AcDbEntity");
    }
    o.str(8, p.layer);
    if (!p.linetype.empty())
        o.str(6, p.linetype);
    colorGroups(p);
    if (!r12_) {
        if (p.lineweight.isValue())
            o.i(370, validLineweight(p.lineweight.v));
        else if (p.lineweight.isByBlock())
            o.i(370, -2);
        else if (p.lineweight.isDefault())
            o.i(370, -3);
    }
    if (p.ltScale != 1 && p.ltScale > 0)
        o.d(48, p.ltScale);
    if (!p.visible)
        o.i(60, 1);
    sub(subclass);
}

void Writer::skipEntity(std::string_view what, const std::string& why) {
    ++summary_.skipped;
    rep_.warn(wcode::kSkippedEntity, std::string(what) + " not written: " + why);
}

void Writer::downgrade(std::string_view what, const std::string& why) {
    ++summary_.downgraded;
    if (warnedOnce_.insert(std::string(what) + why).second) {
        rep_.warn(wcode::kDowngraded, std::string(what) + " " + why);
    }
}

// ---------------------------------------------------------------------------------------------
// Dispatch

void Writer::writeEntityList(const std::vector<EntityId>& ids, std::uint64_t owner) {
    for (std::size_t i = 0; i < ids.size();) {
        const Entity* e = doc_.find(ids[i]);
        if (!e) {
            ++i;
            continue;
        }
        if (entityAs<DimensionData>(*e)) {
            i += writeDimension(ids, i, owner);
            continue;
        }
        writeEntity(*e, owner);
        ++i;
    }
}

void Writer::writeEntity(const Entity& e, std::uint64_t owner) {
    Props p = propsOf(e.common);
    switch (kind(e)) {
        case EntityKind::Line:
            writeLine(p, *entityAs<LineData>(e), owner);
            break;
        case EntityKind::Circle:
            writeCircle(p, *entityAs<CircleData>(e), owner);
            break;
        case EntityKind::Arc:
            writeArc(p, *entityAs<ArcData>(e), owner);
            break;
        case EntityKind::Polyline:
            writePolyline(p, *entityAs<PolylineData>(e), owner);
            break;
        case EntityKind::Point:
            writePoint(p, *entityAs<PointData>(e), owner);
            break;
        case EntityKind::Ellipse:
            writeEllipse(p, *entityAs<EllipseData>(e), e, owner);
            break;
        case EntityKind::Spline:
            writeSpline(p, *entityAs<SplineData>(e), e, owner);
            break;
        case EntityKind::Text:
            writeText(p, *entityAs<TextData>(e), owner);
            break;
        case EntityKind::MText:
            writeMText(p, *entityAs<MTextData>(e), owner);
            break;
        case EntityKind::Hatch: {
            const HatchData& h = *entityAs<HatchData>(e);
            if (p.color.kind == ColorKind::ByLayer && h.fill.kind != ColorKind::ByLayer)
                p.color = h.fill;
            writeHatch(p, h, owner);
            break;
        }
        case EntityKind::Insert:
            writeInsert(p, *entityAs<InsertData>(e), owner);
            break;
        case EntityKind::Dimension: {
            std::vector<EntityId> one{e.common.id};
            writeDimension(one, 0, owner);
            break;
        }
        case EntityKind::Leader:
            writeLeader(p, *entityAs<LeaderData>(e), owner);
            break;
        case EntityKind::Proxy:
            writeProxy(p, *entityAs<ProxyData>(e), owner);
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// Simple geometry

void Writer::writeLine(const Props& p, const LineData& d, std::uint64_t owner) {
    head("LINE", p, owner, "AcDbLine");
    o_->pt(10, d.a);
    o_->pt(11, d.b);
    count("LINE");
}

void Writer::writePoint(const Props& p, const PointData& d, std::uint64_t owner) {
    head("POINT", p, owner, "AcDbPoint");
    o_->pt(10, d.p);
    count("POINT");
}

void Writer::writeCircle(const Props& p, const CircleData& d, std::uint64_t owner) {
    head("CIRCLE", p, owner, "AcDbCircle");
    o_->pt(10, d.center);
    o_->d(40, d.radius);
    count("CIRCLE");
}

void Writer::writeArc(const Props& p, const ArcData& d, std::uint64_t owner) {
    head("ARC", p, owner, "AcDbCircle");
    o_->pt(10, d.center);
    o_->d(40, d.radius);
    sub("AcDbArc");
    o_->d(50, d.startAngle * kDeg);
    o_->d(51, d.endAngle * kDeg);
    count("ARC");
}

void Writer::writeVertexPolyline(const Props& p, const std::vector<PolyVertex>& v, bool closed,
                                 double constantWidth, std::uint64_t owner) {
    // R12 has no LWPOLYLINE: a POLYLINE followed by VERTEX records and SEQEND.
    head("POLYLINE", p, owner, "AcDb2dPolyline");
    Out& o = *o_;
    o.i(66, 1);
    o.pt(10, {0, 0});
    if (constantWidth > 0) {
        o.d(40, constantWidth);
        o.d(41, constantWidth);
    }
    o.i(70, closed ? 1 : 0);
    for (const PolyVertex& pv : v) {
        o.zero("VERTEX");
        o.str(8, p.layer);
        o.pt(10, pv.p);
        if (pv.startWidth != 0 || pv.endWidth != 0) {
            o.d(40, pv.startWidth);
            o.d(41, pv.endWidth);
        }
        if (pv.bulge != 0)
            o.d(42, pv.bulge);
    }
    o.zero("SEQEND");
    o.str(8, p.layer);
    count("POLYLINE");
}

void Writer::writePolyline(const Props& p, const PolylineData& d, std::uint64_t owner) {
    if (d.v.size() < 2) {
        skipEntity("LWPOLYLINE", "fewer than two vertices");
        return;
    }
    if (r12_) {
        writeVertexPolyline(p, d.v, d.closed, d.constantWidth, owner);
        return;
    }
    head("LWPOLYLINE", p, owner, "AcDbPolyline");
    Out& o = *o_;
    o.i(90, static_cast<long long>(d.v.size()));
    o.i(70, d.closed ? 1 : 0);
    if (d.constantWidth != 0)
        o.d(43, d.constantWidth);
    for (const PolyVertex& v : d.v) {
        o.pt2(10, v.p);
        if (v.startWidth != 0 || v.endWidth != 0) {
            o.d(40, v.startWidth);
            o.d(41, v.endWidth);
        }
        if (v.bulge != 0)
            o.d(42, v.bulge);
    }
    count("LWPOLYLINE");
}

void Writer::writeCurveFallback(const Entity& e, const Props& p, std::uint64_t owner) {
    geom::DisplayContext ctx;
    ctx.chordTolMm = 0.01;
    DisplayList list;
    geom::buildDisplay(doc_, e, ctx, list);
    for (const DisplayItem& item : list.items) {
        if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
            std::vector<PolyVertex> v;
            for (Vec2 pt : pl->pts) {
                PolyVertex pv;
                pv.p = pt;
                v.push_back(pv);
            }
            if (v.size() >= 2)
                writeVertexPolyline(p, v, pl->closed, 0, owner);
        }
    }
}

void Writer::writeEllipse(const Props& p, const EllipseData& d, const Entity& e,
                          std::uint64_t owner) {
    if (r12_) {
        downgrade("ELLIPSE", "has no R12 form; written as a polyline");
        writeCurveFallback(e, p, owner);
        return;
    }
    head("ELLIPSE", p, owner, "AcDbEllipse");
    Out& o = *o_;
    o.pt(10, d.center);
    o.pt(11, d.majorAxis);
    o.d(210, 0);
    o.d(220, 0);
    o.d(230, 1);
    o.d(40, d.ratio);
    double sweep = d.endParam - d.startParam;
    if (sweep >= kTwoPi - 1e-9 || sweep <= 0) {
        o.d(41, 0);
        o.d(42, kTwoPi);
    } else {
        double s = normalizeAngle(d.startParam);
        o.d(41, s);
        o.d(42, s + sweep > kTwoPi ? s + sweep - kTwoPi : s + sweep);
    }
    count("ELLIPSE");
}

void Writer::writeSpline(const Props& p, const SplineData& d, const Entity& e,
                         std::uint64_t owner) {
    if (r12_) {
        downgrade("SPLINE", "has no R12 form; written as a polyline");
        writeCurveFallback(e, p, owner);
        return;
    }
    if (d.control.empty() && d.fit.size() < 2) {
        skipEntity("SPLINE", "no control or fit points");
        return;
    }
    head("SPLINE", p, owner, "AcDbSpline");
    Out& o = *o_;
    o.d(210, 0);
    o.d(220, 0);
    o.d(230, 1);

    std::vector<double> knots = d.knots;
    bool haveControl = !d.control.empty();
    if (haveControl && knots.size() != d.control.size() + static_cast<std::size_t>(d.degree) + 1) {
        // Clamped uniform knot vector.
        std::size_t n = d.control.size();
        std::size_t deg = static_cast<std::size_t>(std::max(d.degree, 1));
        knots.clear();
        std::size_t interior = n > deg ? n - deg - 1 : 0;
        for (std::size_t i = 0; i <= deg; ++i)
            knots.push_back(0);
        for (std::size_t i = 1; i <= interior; ++i)
            knots.push_back(static_cast<double>(i));
        for (std::size_t i = 0; i <= deg; ++i)
            knots.push_back(static_cast<double>(interior + 1));
    }
    bool rational = haveControl && d.weights.size() == d.control.size();
    int flags = (d.closed ? 1 : 0) | (d.periodic ? 2 : 0) | (rational ? 4 : 0) | 8;
    o.i(70, flags);
    o.i(71, d.degree);
    o.i(72, haveControl ? static_cast<long long>(knots.size()) : 0);
    o.i(73, static_cast<long long>(d.control.size()));
    o.i(74, static_cast<long long>(d.fit.size()));
    o.d(42, 1e-7);
    o.d(43, 1e-7);
    o.d(44, 1e-10);
    if (haveControl) {
        for (double k : knots)
            o.d(40, k);
        if (rational) {
            for (double w : d.weights)
                o.d(41, w);
        }
        for (Vec2 c : d.control)
            o.pt(10, c);
    }
    for (Vec2 f : d.fit)
        o.pt(11, f);
    count("SPLINE");
}

// ---------------------------------------------------------------------------------------------
// Text

void Writer::writeText(const Props& p, const TextData& d, std::uint64_t owner) {
    writeTextRecord(p, d, owner, d.attdef ? "ATTDEF" : "TEXT", d.utf8);
}

// TEXT, ATTDEF (inside a block) and ATTRIB (after an INSERT) share the text groups; only the
// record type, the string in group 1 and the attribute subclass differ.
void Writer::writeTextRecord(const Props& p, const TextData& d, std::uint64_t owner,
                             std::string_view type, const std::string& shown) {
    head(type, p, owner, "AcDbText");
    Out& o = *o_;
    o.pt(10, d.position);
    o.d(40, d.height);
    o.str(1, type == "ATTDEF" ? d.attdef->defaultValue : shown);
    if (d.rotation != 0)
        o.d(50, d.rotation * kDeg);
    if (d.widthFactor != 1)
        o.d(41, d.widthFactor);
    if (d.oblique != 0)
        o.d(51, d.oblique * kDeg);
    o.str(7, textStyleNameOf(d.style));
    bool aligned = d.h != HAlign::Left || d.v != VAlign::Baseline;
    if (d.h != HAlign::Left)
        o.i(72, static_cast<int>(d.h));
    if (aligned)
        o.pt(11, d.alignPoint);
    if (type == "TEXT") {
        sub("AcDbText");
        if (d.v != VAlign::Baseline)
            o.i(73, static_cast<int>(d.v));
    } else {
        const AttDefInfo& a = *d.attdef;
        sub(type == "ATTDEF" ? "AcDbAttributeDefinition" : "AcDbAttribute");
        if (type == "ATTDEF")
            o.str(3, a.prompt);
        o.str(2, a.tag);
        o.i(70,
            (a.invisible ? 1 : 0) | (a.constant ? 2 : 0) | (a.verify ? 4 : 0) | (a.preset ? 8 : 0));
        if (d.v != VAlign::Baseline)
            o.i(74, static_cast<int>(d.v));
    }
    count(std::string(type));
}

void Writer::writeMText(const Props& p, const MTextData& d, std::uint64_t owner) {
    if (r12_) {
        downgrade("MTEXT", "has no R12 form; written as one TEXT per line");
        std::string plain = plainLines(d.utf8);
        std::vector<std::string> lines;
        std::size_t pos = 0;
        while (true) {
            std::size_t nl = plain.find('\n', pos);
            lines.push_back(plain.substr(pos, nl == std::string::npos ? nl : nl - pos));
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
        }
        double step = 1.5 * d.height;
        Vec2 down{std::sin(d.rotation), -std::cos(d.rotation)};
        int col = static_cast<int>(d.attach) % 3;  // 0 left, 1 center, 2 right
        int row = static_cast<int>(d.attach) / 3;  // 0 top, 1 middle, 2 bottom
        double n = static_cast<double>(lines.size());
        for (std::size_t k = 0; k < lines.size(); ++k) {
            double shift = row == 0 ? -static_cast<double>(k) * step
                                    : (row == 1 ? ((n - 1) / 2 - static_cast<double>(k)) * step
                                                : (n - 1 - static_cast<double>(k)) * step);
            Vec2 at = d.position + down * (-shift);
            head("TEXT", p, owner, "AcDbText");
            Out& o = *o_;
            o.pt(10, at);
            o.d(40, d.height);
            o.str(1, lines[k]);
            if (d.rotation != 0)
                o.d(50, d.rotation * kDeg);
            o.str(7, textStyleNameOf(d.style));
            o.i(72, col);
            o.pt(11, at);
            o.i(73, row == 0 ? 3 : (row == 1 ? 2 : 1));
            count("TEXT");
        }
        return;
    }
    head("MTEXT", p, owner, "AcDbMText");
    Out& o = *o_;
    o.pt(10, d.position);
    o.d(40, d.height);
    o.d(41, d.width);
    o.i(71, static_cast<int>(d.attach) + 1);
    o.i(72, 1);
    std::vector<std::string> parts = chunks(encodeText(toDxfMarkup(d.utf8), utf8_), 250);
    for (std::size_t i = 0; i + 1 < parts.size(); ++i)
        o.raw(3, parts[i]);
    o.raw(1, parts.back());
    o.str(7, textStyleNameOf(d.style));
    if (d.rotation != 0)
        o.pt(11, {std::cos(d.rotation), std::sin(d.rotation)});
    count("MTEXT");
}

// ---------------------------------------------------------------------------------------------
// Inserts, leaders, proxies, solids

void Writer::writeInsert(const Props& p, const InsertData& d, std::uint64_t owner) {
    std::string name = blockNameOf(d.block);
    if (name.empty()) {
        skipEntity("INSERT", "its block definition does not exist");
        return;
    }
    // Non-constant attributes of the block become ATTRIB records after the INSERT.
    std::vector<const TextData*> defs;
    if (const BlockDef* bd = doc_.block(d.block))
        for (EntityId id : bd->entities)
            if (const Entity* e = doc_.find(id))
                if (const auto* t = entityAs<TextData>(*e); t && t->attdef && !t->attdef->constant)
                    defs.push_back(t);
    head("INSERT", p, owner, "AcDbBlockReference");
    Out& o = *o_;
    if (!defs.empty())
        o.i(66, 1);
    o.str(2, name);
    o.pt(10, d.position);
    o.d(41, d.scale.x);
    o.d(42, d.scale.y);
    o.d(43, 1);
    if (d.rotation != 0)
        o.d(50, d.rotation * kDeg);
    count("INSERT");
    if (defs.empty())
        return;
    const BlockDef* bd = doc_.block(d.block);
    const double c = std::cos(d.rotation), sn = std::sin(d.rotation);
    for (const TextData* def : defs) {
        // The definition's placement, carried through the insert transform. A non-uniform scale
        // cannot stretch text, so the height follows the Y scale.
        Vec2 rel = def->position - bd->base;
        rel = {rel.x * d.scale.x, rel.y * d.scale.y};
        TextData t = *def;
        t.position = d.position + Vec2{rel.x * c - rel.y * sn, rel.x * sn + rel.y * c};
        Vec2 al = def->alignPoint - bd->base;
        al = {al.x * d.scale.x, al.y * d.scale.y};
        t.alignPoint = d.position + Vec2{al.x * c - al.y * sn, al.x * sn + al.y * c};
        t.height = def->height * std::abs(d.scale.y);
        t.rotation = def->rotation + d.rotation;
        std::string value;
        bool found = false;
        for (const AttribValue& v : d.attribs)
            if (v.tag == def->attdef->tag) {
                value = v.value;
                found = true;
                break;
            }
        if (!found)
            value = def->attdef->defaultValue;
        writeTextRecord(p, t, owner, "ATTRIB", value);
    }
    o.zero("SEQEND");
    o.str(8, p.layer);
}

void Writer::writeLeader(const Props& p, const LeaderData& d, std::uint64_t owner) {
    if (d.vertices.size() < 2) {
        skipEntity("LEADER", "fewer than two vertices");
        return;
    }
    if (r12_) {
        downgrade("LEADER", "has no R12 form; written as a polyline");
        std::vector<PolyVertex> v;
        for (Vec2 pt : d.vertices) {
            PolyVertex pv;
            pv.p = pt;
            v.push_back(pv);
        }
        writeVertexPolyline(p, v, false, 0, owner);
    } else {
        head("LEADER", p, owner, "AcDbLeader");
        Out& o = *o_;
        o.str(3, dimStyleNameOf(d.style));
        o.i(71, 1);
        o.i(72, 0);
        o.i(73, 3);  // no annotation: the text is a separate entity
        o.i(74, 1);
        o.i(75, 0);
        o.d(40, 0);
        o.d(41, 0);
        o.i(76, static_cast<long long>(d.vertices.size()));
        for (Vec2 pt : d.vertices)
            o.pt(10, pt);
        o.d(210, 0);
        o.d(220, 0);
        o.d(230, 1);
        o.pt(211, {1, 0});
        o.pt(212, {0, 0});
        o.pt(213, {0, 0});
        count("LEADER");
    }
    if (!d.utf8.empty()) {
        MTextData text;
        text.position = d.textPos;
        const DimStyle* ds = doc_.dimStyle(d.style ? d.style : doc_.header().currentDimStyle);
        text.height = ds ? ds->textHeight : 2.5;
        text.attach = Attachment::MiddleLeft;
        text.utf8 = d.utf8;
        writeMText(p, text, owner);
    }
}

void Writer::writeProxy(const Props& p, const ProxyData& d, std::uint64_t owner) {
    if (d.originalType == "ACAD_PROXY_ENTITY" && !d.raw.empty() && !r12_) {
        head("ACAD_PROXY_ENTITY", p, owner, "AcDbProxyEntity");
        Out& o = *o_;
        o.i(90, 498);
        o.i(91, 500);
        o.i(93, 0);
        o.i(95, 0);
        o.i(70, 0);
        static const char* digits = "0123456789ABCDEF";
        for (std::size_t pos = 0; pos < d.raw.size(); pos += 127) {
            std::string hex;
            for (std::size_t i = pos; i < std::min(pos + 127, d.raw.size()); ++i) {
                hex.push_back(digits[d.raw[i] >> 4]);
                hex.push_back(digits[d.raw[i] & 15]);
            }
            o.raw(310, hex);
        }
        count("ACAD_PROXY_ENTITY");
        return;
    }
    if (!d.graphics.items.empty()) {
        writeDisplayList(d.graphics, p.layer, owner);
        return;
    }
    skipEntity("PROXY", "unknown object \"" + d.originalType + "\" has no graphics to write");
}

void Writer::writeSolid(const Props& p, const std::vector<Vec2>& ring, std::uint64_t owner) {
    head("SOLID", p, owner, "AcDbTrace");
    Out& o = *o_;
    Vec2 p1 = ring[0], p2 = ring[1], p3 = ring.size() == 4 ? ring[3] : ring[2],
         p4 = ring.size() == 4 ? ring[2] : ring[2];
    o.pt(10, p1);
    o.pt(11, p2);
    o.pt(12, p3);
    o.pt(13, p4);
    count("SOLID");
}

void Writer::writeDisplayList(const DisplayList& list, const std::string& fallbackLayer,
                              std::uint64_t owner) {
    for (const DisplayItem& item : list.items) {
        if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
            if (pl->pts.size() < 2)
                continue;
            Props p = propsOfStyle(pl->style, fallbackLayer);
            std::vector<PolyVertex> v;
            v.reserve(pl->pts.size());
            for (Vec2 pt : pl->pts) {
                PolyVertex pv;
                pv.p = pt;
                v.push_back(pv);
            }
            PolylineData data;
            data.v = std::move(v);
            data.closed = pl->closed;
            writePolyline(p, data, owner);
        } else if (const auto* fill = std::get_if<DisplayFill>(&item)) {
            Props p = propsOfStyle(fill->style, fallbackLayer);
            // A ring may repeat its first point at the end; DXF closes the loop by itself.
            std::vector<std::vector<Vec2>> rings = fill->rings;
            for (auto& ring : rings) {
                if (ring.size() > 3 && ring.front() == ring.back())
                    ring.pop_back();
            }
            if (rings.size() == 1 && (rings[0].size() == 3 || rings[0].size() == 4)) {
                writeSolid(p, rings[0], owner);
            } else if (!rings.empty()) {
                if (r12_) {
                    downgrade("HATCH", "has no R12 form; the filled area is written as outlines");
                    for (const auto& ring : rings) {
                        std::vector<PolyVertex> v;
                        for (Vec2 pt : ring) {
                            PolyVertex pv;
                            pv.p = pt;
                            v.push_back(pv);
                        }
                        if (v.size() >= 2)
                            writeVertexPolyline(p, v, true, 0, owner);
                    }
                } else {
                    writeSolidHatch(p, rings, owner);
                }
            }
        } else if (const auto* pts = std::get_if<DisplayPoints>(&item)) {
            Props p = propsOfStyle(pts->style, fallbackLayer);
            for (Vec2 pt : pts->pts)
                writePoint(p, PointData{pt}, owner);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Dimensions

std::size_t Writer::writeDimension(const std::vector<EntityId>& ids, std::size_t index,
                                   std::uint64_t owner) {
    const Entity* e = doc_.find(ids[index]);
    const DimensionData& d = *entityAs<DimensionData>(*e);
    if (d.kind == DimKind::ArcLength) {
        skipEntity("DIMENSION", "arc length dimensions are not supported yet");
        return 1;
    }
    Props p = propsOf(e->common);

    // Imported dimensions are followed by their drawn graphics (a DIMENSION proxy) and the loose
    // text of the dimension. They belong into the dimension's anonymous block.
    // TODO(W5-5): once DimensionData has its own display generator, build the *D block from it
    // instead of from these imported graphics.
    const ProxyData* proxy = nullptr;
    std::vector<const Entity*> texts;
    std::size_t j = index + 1;
    if (j < ids.size()) {
        const Entity* n = doc_.find(ids[j]);
        const ProxyData* pd = n ? entityAs<ProxyData>(*n) : nullptr;
        if (pd && pd->originalType == "DIMENSION") {
            proxy = pd;
            ++j;
        }
    }
    if (proxy) {
        BBox2 box;
        for (Vec2 pt : d.defPts)
            box.expand(pt);
        box.expand(d.textPos);
        for (const DisplayItem& item : proxy->graphics.items) {
            if (const auto* pl = std::get_if<DisplayPolyline>(&item)) {
                for (Vec2 pt : pl->pts)
                    box.expand(pt);
            } else if (const auto* fl = std::get_if<DisplayFill>(&item)) {
                for (const auto& ring : fl->rings) {
                    for (Vec2 pt : ring)
                        box.expand(pt);
                }
            }
        }
        while (j < ids.size() && texts.size() < 8) {
            const Entity* n = doc_.find(ids[j]);
            if (!n || n->common.layer != e->common.layer)
                break;
            Vec2 at;
            double h = 0;
            if (const auto* t = entityAs<TextData>(*n)) {
                at = t->position;
                h = t->height;
            } else if (const auto* m = entityAs<MTextData>(*n)) {
                at = m->position;
                h = m->height;
            } else {
                break;
            }
            if (!box.inflated(3 * std::max(h, 1.0)).contains(at))
                break;
            texts.push_back(n);
            ++j;
        }
    }

    DimBlock block;
    do {
        block.name = "*D" + std::to_string(++dimCounter_);
    } while (blockSet_.has(block.name));
    blockSet_.add(block.name);
    block.rec = newHandle();
    {
        Out bo(utf8_, r12_);
        Out* saved = o_;
        o_ = &bo;
        if (proxy)
            writeDisplayList(proxy->graphics, p.layer, block.rec);
        for (const Entity* t : texts)
            writeEntity(*t, block.rec);
        o_ = saved;
        block.body = std::move(bo.buf);
    }
    if (!proxy) {
        ++summary_.downgraded;
        rep_.warn(wcode::kNoDimensionGraphics,
                  "a dimension has no drawn graphics; its block is empty, so programs that do not "
                  "regenerate dimensions will show nothing (the dimension generator is W5-5)");
    }
    std::string blockName = block.name;
    dimBlocks_.push_back(std::move(block));

    int type = 0;
    switch (d.kind) {
        case DimKind::Linear:
            type = 0;
            break;
        case DimKind::Aligned:
            type = 1;
            break;
        case DimKind::Angular2Line:
            type = 2;
            break;
        case DimKind::Diameter:
            type = 3;
            break;
        case DimKind::Radius:
            type = 4;
            break;
        case DimKind::Angular3Point:
            type = 5;
            break;
        case DimKind::ArcLength:
            break;
    }
    if (!r12_)
        type |= 32;
    if (d.textPosUser)
        type |= 128;

    const char* subclass = "AcDbAlignedDimension";
    switch (d.kind) {
        case DimKind::Radius:
            subclass = "AcDbRadialDimension";
            break;
        case DimKind::Diameter:
            subclass = "AcDbDiametricDimension";
            break;
        case DimKind::Angular2Line:
            subclass = "AcDb2LineAngularDimension";
            break;
        case DimKind::Angular3Point:
            subclass = "AcDb3PointAngularDimension";
            break;
        default:
            break;
    }

    head("DIMENSION", p, owner, "AcDbDimension");
    Out& o = *o_;
    o.str(2, blockName);
    switch (d.kind) {
        case DimKind::Linear:
        case DimKind::Aligned:
            o.pt(10, d.defPts[2]);
            break;
        case DimKind::Radius:
        case DimKind::Diameter:
            o.pt(10, d.defPts[0]);
            break;
        default:
            o.pt(10, d.defPts[3]);
            break;
    }
    o.pt(11, d.textPos);
    o.i(70, type);
    o.i(71, 5);
    if (!d.textOverride.empty())
        o.str(1, d.textOverride);
    o.str(3, dimStyleNameOf(d.style));
    o.d(42, d.measurement);
    sub(subclass);
    switch (d.kind) {
        case DimKind::Linear:
        case DimKind::Aligned:
            o.pt(13, d.defPts[0]);
            o.pt(14, d.defPts[1]);
            if (d.kind == DimKind::Linear) {
                o.d(50, d.rotation * kDeg);
                sub("AcDbRotatedDimension");
            }
            break;
        case DimKind::Radius:
        case DimKind::Diameter:
            o.pt(15, d.defPts[1]);
            o.d(40, 0);
            break;
        case DimKind::Angular2Line:
            o.pt(13, d.defPts[0]);
            o.pt(14, d.defPts[1]);
            o.pt(15, d.defPts[2]);
            o.pt(16, d.defPts[3]);
            break;
        case DimKind::Angular3Point:
            o.pt(13, d.defPts[0]);
            o.pt(14, d.defPts[1]);
            o.pt(15, d.defPts[2]);
            break;
        case DimKind::ArcLength:
            break;
    }
    count("DIMENSION");
    return j - index;
}

}  // namespace tbcad::dxf::detail
