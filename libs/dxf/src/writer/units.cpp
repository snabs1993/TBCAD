// A drawing that shows metres, centimetres, inches or feet is written in that unit: a copy of the
// document with every length the reader multiplies by the unit size (INSUNITS) divided by it, so
// other programs see 1 unit = 1 m (or cm, in, ft) and TBCAD reads back the same millimetres.
// Layout sheets stay in paper millimetres, as the reader takes them.
#include "units.h"

#include <cctype>
#include <cstdlib>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "tbcad/geom/entity.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/mline/mline.h"

namespace tbcad::dxf::detail {

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i <= s.size()) {
        std::size_t j = s.find(sep, i);
        if (j == std::string::npos)
            j = s.size();
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& v, char sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i)
            out += sep;
        out += v[i];
    }
    return out;
}

std::string num(double v) {
    std::ostringstream o;
    o.precision(17);
    o << v;
    return o.str();
}

std::string scaledNumber(const std::string& text, double s) {
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str())
        return text;
    return num(v * s);
}

// The blocks that hold layout sheets (paper millimetres, never scaled).
std::set<BlockId> sheetBlocks(const Document& doc) {
    std::set<BlockId> out;
    const auto it = doc.header().custom.find("tbcad.layouts");
    if (it == doc.header().custom.end())
        return out;
    for (const std::string& line : split(it->second, '\n')) {
        const std::vector<std::string> f = split(line, '\t');
        if (f.size() >= 8 && f[0] == "L")
            out.insert(
                BlockId{static_cast<std::uint32_t>(std::strtoul(f[7].c_str(), nullptr, 10))});
    }
    return out;
}

// Viewports show model space: their view centre and 1:N (model units per sheet mm) follow it.
std::string scaledLayouts(const std::string& text, double s) {
    std::vector<std::string> lines = split(text, '\n');
    for (std::string& line : lines) {
        std::vector<std::string> f = split(line, '\t');
        if (f.size() >= 10 && f[0] == "V") {
            f[6] = scaledNumber(f[6], s);
            f[7] = scaledNumber(f[7], s);
            f[8] = scaledNumber(f[8], s);
            line = join(f, '\t');
        }
    }
    return join(lines, '\n');
}

// UCS rows (name, origin x, origin y, angle): the origin is a point of the drawing.
std::string scaledUcs(const std::string& text, double s) {
    std::vector<std::string> lines = split(text, '\n');
    for (std::string& line : lines) {
        std::vector<std::string> f = split(line, '\t');
        if (f.size() >= 4) {
            f[1] = scaledNumber(f[1], s);
            f[2] = scaledNumber(f[2], s);
            line = join(f, '\t');
        }
    }
    return join(lines, '\n');
}

// \p settings with their lengths multiplied by s (as the reader's scaleParagraph).
std::string scaleParagraph(const std::string& arg, double s) {
    std::vector<std::string> toks = split(arg, ',');
    for (std::size_t n = 0; n < toks.size(); ++n) {
        std::string& tok = toks[n];
        std::size_t pre = 0;
        if (n == 0 && !tok.empty() && tok[0] == 'x')
            ++pre;
        if (tok.compare(pre, 1, "q") == 0)
            continue;
        if (tok.compare(pre, 2, "sb") == 0 || tok.compare(pre, 2, "sa") == 0)
            pre += 2;
        else if (pre < tok.size() && std::isalpha(static_cast<unsigned char>(tok[pre])))
            ++pre;
        const std::string numText = tok.substr(pre);
        char* end = nullptr;
        const double v = std::strtod(numText.c_str(), &end);
        if (end != numText.c_str() && *end == 0)
            tok = tok.substr(0, pre) + num(v * s);
    }
    return join(toks, ',');
}

// Text markup with its absolute heights (\H<h>;) and paragraph lengths (\p) in drawing units.
std::string scaleMarkup(const std::string& t, double s) {
    if (t.find('\\') == std::string::npos)
        return t;
    std::string out;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c != '\\' || i + 1 >= t.size()) {
            out.push_back(c);
            continue;
        }
        const char k = t[i + 1];
        const std::size_t semi = t.find(';', i + 2);
        if ((k == 'H' || k == 'p') && semi != std::string::npos) {
            const std::string arg = t.substr(i + 2, semi - i - 2);
            out += '\\';
            out += k;
            if (k == 'p') {
                out += scaleParagraph(arg, s);
            } else {
                char* end = nullptr;
                const double v = std::strtod(arg.c_str(), &end);
                out += end != arg.c_str() && *end == 0 ? num(v * s) : arg;
            }
            out += ';';
            i = semi;
            continue;
        }
        out.push_back(c);
        out.push_back(k);
        ++i;
    }
    return out;
}

void scaleProxyGraphics(ProxyData& p, double s) {
    for (DisplayItem& it : p.graphics.items)
        std::visit(
            [&](auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, DisplayFill>) {
                    for (auto& r : item.rings)
                        for (Vec2& q : r)
                            q = q * s;
                } else {
                    for (Vec2& q : item.pts)
                        q = q * s;
                }
            },
            it);
}

// One object in drawing units. Inserts keep their scale (the block's content is scaled instead).
Result<void> scaleEntity(Entity& e, double s, double patternUnit) {
    if (auto* ins = entityAs<InsertData>(e)) {
        ins->position = ins->position * s;
        for (Vec2& p : ins->clip)
            p = p * s;
        for (AttribValue& a : ins->attribs)
            a.offset = a.offset * s;
        if (ins->array) {
            ArrayInfo& a = *ins->array;
            a.rowSpacing *= s;
            a.columnSpacing *= s;
            a.center = a.center * s;
            a.reference = a.reference * s;
            for (PolyVertex& v : a.path)
                v.p = v.p * s;
            a.pathSpacing *= s;
        }
        return {};
    }
    if (auto* px = entityAs<ProxyData>(e)) {
        scaleProxyGraphics(*px, s);  // the kept DXF groups stay as they were read
        return {};
    }
    // Simple shapes by hand: transformed() would also tidy them (a full ellipse's start).
    if (auto* l = entityAs<LineData>(e)) {
        l->a = l->a * s;
        l->b = l->b * s;
        return {};
    }
    if (auto* c = entityAs<CircleData>(e)) {
        c->center = c->center * s;
        c->radius *= s;
        return {};
    }
    if (auto* a = entityAs<ArcData>(e)) {
        a->center = a->center * s;
        a->radius *= s;
        return {};
    }
    if (auto* el = entityAs<EllipseData>(e)) {
        el->center = el->center * s;
        el->majorAxis = el->majorAxis * s;
        return {};
    }
    if (auto* pt = entityAs<PointData>(e)) {
        pt->p = pt->p * s;
        return {};
    }
    if (auto* h = entityAs<HatchData>(e)) {  // by hand too: transformed() turns arc angles
        for (HatchLoop& loop : h->loops)
            for (HatchEdge& edge : loop.edges)
                std::visit(
                    [&](auto& ed) {
                        using T = std::decay_t<decltype(ed)>;
                        if constexpr (std::is_same_v<T, HatchLineEdge>) {
                            ed.a = ed.a * s;
                            ed.b = ed.b * s;
                        } else if constexpr (std::is_same_v<T, HatchArcEdge>) {
                            ed.center = ed.center * s;
                            ed.radius *= s;
                        } else if constexpr (std::is_same_v<T, HatchEllipseEdge>) {
                            ed.center = ed.center * s;
                            ed.majorAxis = ed.majorAxis * s;
                        } else if constexpr (std::is_same_v<T, HatchSplineEdge>) {
                            for (Vec2& p : ed.spline.control)
                                p = p * s;
                            for (Vec2& p : ed.spline.fit)
                                p = p * s;
                        } else {
                            for (PolyVertex& v : ed.v) {
                                v.p = v.p * s;
                                v.startWidth *= s;
                                v.endWidth *= s;
                            }
                        }
                    },
                    edge);
        h->patternOrigin = h->patternOrigin * s;
        h->patternScale *= s * patternUnit;  // see Importer::patternK_
        return {};
    }
    if (auto* mt = entityAs<MTextData>(e)) {
        mt->utf8 = scaleMarkup(mt->utf8, s);
        mt->columnGap *= s;
        mt->columnHeight *= s;
    }
    if (auto* ml = entityAs<MLeaderData>(e))
        ml->utf8 = scaleMarkup(ml->utf8, s);
    auto t = geom::transformed(e, Transform2::scale(s, s));
    if (!t)
        return t.error();
    Entity out = std::move(t).value();
    if (auto* ml = entityAs<MlineData>(out))
        ml->scale = entityAs<MlineData>(e)->scale;  // the style's offsets are scaled instead
    if (auto* m = entityAs<MLeaderData>(out)) {
        m->textHeight *= s;
        m->blockScale = entityAs<MLeaderData>(e)->blockScale;
        for (AttribValue& a : m->attribs)
            a.offset = a.offset * s;
    }
    if (auto* tb = entityAs<TableData>(out))
        for (TableCellStyle* cs : {&tb->titleStyle, &tb->headerStyle, &tb->dataStyle})
            cs->textHeight *= s;
    e.data = std::move(out.data);
    return {};
}

}  // namespace

int insunitsCode(LengthUnit u) {
    switch (u) {
        case LengthUnit::Inch:
            return 1;
        case LengthUnit::Foot:
            return 2;
        case LengthUnit::Centimeter:
            return 5;
        case LengthUnit::Meter:
            return 6;
        case LengthUnit::Millimeter:
            break;
    }
    return 4;
}

Result<std::unique_ptr<Document>> inDrawingUnits(const Document& doc) {
    const LengthUnit unit = doc.header().units;
    const double s = 1.0 / mmPerUnit(unit);
    // Imperial drawings take their pattern scales in inches (acad.pat), as the reader does.
    const double patternUnit = unit == LengthUnit::Inch || unit == LengthUnit::Foot ? 25.4 : 1;
    auto bytes = io::writeTbcadToMemory(doc);
    if (!bytes)
        return bytes.error();
    io::IoReport ioReport;
    auto copy = io::readTbcad(std::span<const std::uint8_t>(bytes.value()), ioReport);
    if (!copy)
        return copy.error();
    std::unique_ptr<Document> out = std::move(copy).value();
    const std::set<BlockId> sheets = sheetBlocks(*out);

    Transaction tx = out->begin("drawing units");
    std::vector<EntityId> ids;
    out->forEachEntity([&](const Entity& e) {
        if (!sheets.count(out->ownerBlock(e.common.id)))
            ids.push_back(e.common.id);
    });
    std::string failed;
    for (EntityId id : ids) {
        auto r = tx.modify(id, [&](Entity& e) {
            if (auto ok = scaleEntity(e, s, patternUnit); !ok && failed.empty())
                failed = ok.error().message;
        });
        if (!r)
            return r.error();
    }
    if (!failed.empty())
        return makeError(ErrorCode::Generic, "cannot write the drawing in its units: " + failed);

    // Ids first: a modified row replaces the map entry being walked.
    auto keys = [](const auto& rows) {
        std::vector<typename std::decay_t<decltype(rows)>::key_type> v;
        for (const auto& kv : rows)
            v.push_back(kv.first);
        return v;
    };
    for (BlockId id : keys(out->blocks())) {
        if (sheets.count(id))
            continue;
        if (auto r = tx.modifyBlock(id, [&](BlockDef& d) { d.base = d.base * s; }); !r)
            return r.error();
    }
    for (LinetypeId id : keys(out->linetypes())) {
        (void)tx.modifyLinetype(id, [&](Linetype& l) {
            for (double& v : l.pattern)
                v *= s;
            for (LinetypeMark& m : l.marks) {
                m.offset *= s;
                m.scale *= s;
                m.shift = m.shift * s;
            }
        });
    }
    for (TextStyleId id : keys(out->textStyles())) {
        (void)tx.modifyTextStyle(id, [&](TextStyle& t) { t.height *= s; });
    }
    for (DimStyleId id : keys(out->dimStyles())) {
        (void)tx.modifyDimStyle(id, [&](DimStyle& d) {
            for (double* v : {&d.textHeight, &d.textGap, &d.arrowSize, &d.extLineOffset,
                              &d.extLineExtension, &d.dimLineExtension, &d.roundOff, &d.tolUpper,
                              &d.tolLower, &d.baselineSpacing, &d.fixedExtLength, &d.centerMark})
                *v *= s;
            d.altScale /= s;
            // The numbers are already in the drawing's unit: a dimension laid out for the file
            // shows them as they are (DIMLUNIT does not carry the unit; the reader sets it back).
            d.units = LengthUnit::Millimeter;
        });
    }
    for (LevelId id : keys(out->levels())) {
        (void)tx.modifyLevel(id, [&](Level& l) { l.elevation *= s; });
    }
    std::vector<mline::Style> mlineStyles;  // the reader scales MLINESTYLE offsets by the unit
    for (const std::string& name : mline::styleNames(*out))
        if (auto st = mline::findStyle(*out, name)) {
            for (double& v : st->offsets)
                v *= s;
            mlineStyles.push_back(std::move(*st));
        }
    if (auto r = tx.modifyHeader([&](Header& h) {
            for (const mline::Style& st : mlineStyles)
                mline::storeStyle(h, st);
            if (const BBox2 lim = drawingLimits(h); !lim.empty())
                setDrawingLimits(h, lim.min * s, lim.max * s);
            for (const char* key : {"tbcad.ucs", "tbcad.ucs.current"})
                if (auto it = h.custom.find(key); it != h.custom.end())
                    it->second = scaledUcs(it->second, s);
            if (auto it = h.custom.find("tbcad.layouts"); it != h.custom.end())
                it->second = scaledLayouts(it->second, s);
        });
        !r)
        return r.error();
    tx.commit();
    return out;
}

}  // namespace tbcad::dxf::detail
