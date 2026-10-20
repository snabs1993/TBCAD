// DXF import: DIMENSION. The definition points become DimensionData; the anonymous "*D" block
// that holds the drawn dimension (lines, arrowheads, text) is kept as ProxyData graphics plus
// loose text entities so the dimension displays before the dimension generator exists.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string_view>
#include <tuple>

#include "importer.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dxf/encoding.h"
#include "tbcad/geom/entity.h"

namespace tbcad::dxf::detail {

namespace {

bool isByBlock(std::string_view n) {
    static constexpr std::string_view k = "byblock";
    if (n.size() != k.size())
        return false;
    for (std::size_t i = 0; i < k.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(n[i])) != k[i])
            return false;
    return true;
}

constexpr double kDeg = kPi / 180.0;
constexpr int kMaxInsertDepth = 4;

// Appends the points of the arc from a to b described by `bulge` (excluding a, including b).
void bulgeArc(Vec2 a, Vec2 b, double bulge, std::vector<Vec2>& out) {
    if (std::abs(bulge) < 1e-12 || tol::isZeroLength(b - a)) {
        out.push_back(b);
        return;
    }
    double sweep = 4 * std::atan(bulge);
    Vec2 chord = b - a;
    double c = length(chord);
    double radius = c / (2 * std::sin(std::abs(sweep) / 2));
    Vec2 mid = (a + b) * 0.5;
    double h = radius * std::cos(sweep / 2);
    // For a counter-clockwise arc (bulge > 0) the center is to the left of the chord.
    Vec2 center = mid + perp(normalized(chord)) * (bulge > 0 ? h : -h);
    double start = std::atan2(a.y - center.y, a.x - center.x);
    int steps = std::max(2, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 18))));
    for (int i = 1; i <= steps; ++i) {
        double t = start + sweep * i / steps;
        out.push_back(i == steps
                          ? b
                          : Vec2{center.x + radius * std::cos(t), center.y + radius * std::sin(t)});
    }
}

// Group 52 is the angle of the extension lines from the X axis (DIMEDIT Oblique), 0 when they are
// not tilted. DimensionData::oblique is their lean from the perpendicular of the dimension line,
// whose direction is `dimAngle`. Both radians.
double leanOf(double obliqueAngle, double dimAngle) {
    if (!std::isfinite(obliqueAngle) || obliqueAngle == 0)
        return 0;
    const double lean = std::remainder(obliqueAngle - dimAngle - kPi / 2, kPi);
    return std::abs(lean) < 1e-12 ? 0 : lean;
}

// "\X" in a dimension's text puts what follows under the dimension line: a second line.
std::string dimensionLines(std::string t) {
    for (std::size_t at = t.find("\\X"); at != std::string::npos; at = t.find("\\X", at + 2)) {
        if (at > 0 && t[at - 1] == '\\')
            continue;  // an escaped backslash followed by X
        t[at + 1] = 'P';
    }
    return t;
}

}  // namespace

// The relative height ("\\H0.8x;") that starts a text and so sizes all of it, else 1. Other
// leading codes ("\\A1;", a font) may come first; a height in the middle ("9d{\\H0.8x;b}")
// sizes just the letters after it.
double leadingHeightFactor(std::string_view t) {
    std::size_t i = 0;
    while (i < t.size()) {
        if (t[i] == '{') {
            ++i;
        } else if (t[i] == '\\' && i + 1 < t.size() &&
                   std::string_view("ACcFfQTWpbi").find(t[i + 1]) != std::string_view::npos &&
                   t.find(';', i) != std::string_view::npos) {
            i = t.find(';', i) + 1;
        } else {
            break;
        }
    }
    if (t.substr(i, 2) != "\\H")
        return 1;
    i += 2;
    const std::size_t j = t.find(';', i);
    if (j == std::string_view::npos || j <= i || t[j - 1] != 'x')
        return 1;
    const double v = std::strtod(std::string(t.substr(i, j - 1 - i)).c_str(), nullptr);
    return std::isfinite(v) && v > 0 && v < 1e6 ? v : 1;
}

// A dimension can carry its own values for a few style variables (the "DSTYLE" block of its
// extended data), and its text can start with a relative height ("{\\H560x;<>}"). Both make a
// copy of the style named by the dimension; copies with the same values are shared.
DimStyleId Importer::dimStyleWithOverrides(const Record& r, DimStyleId base) {
    std::optional<double> txt, asz, scale, gap, exo, exe, dle, tp, tm;
    std::optional<std::int64_t> tolOn, limOn, altOn;
    std::map<int, double> extra;  // the other overridable variables, by variable number
    // DIMLDRBLK, DIMBLK, DIMBLK1, DIMBLK2: block record handles
    std::map<int, std::string> handles;
    bool inDstyle = false, inAcad = false;
    int var = -1;
    for (const Group& g : r.g) {
        if (g.code == 1001) {
            inAcad = g.str() == "ACAD";
            inDstyle = false;
            var = -1;
            continue;
        }
        if (!inAcad)
            continue;
        if (g.code == 1000) {
            inDstyle = g.str() == "DSTYLE";
            continue;
        }
        if (!inDstyle)
            continue;
        if (g.code == 1070 && var < 0) {
            var = static_cast<int>(g.integer());
            continue;
        }
        if (g.code == 1005 && var >= 0) {
            // Only handles of a known block record (a file converted from DWG has no table).
            std::string h(g.str());
            for (char& c : h)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if ((var == 341 || var == 342 || var == 343 || var == 344) &&
                (h == "0" || blockNameByHandle_.count(h)))
                handles[var] = h;
            else if (var == 340 && styleNameByHandle_.count(std::string(g.str())))
                handles[var] = std::string(g.str());  // DIMTXSTY
            else if ((var == 345 || var == 346 || var == 347) && ltypeNameByHandle_.count(h))
                handles[var] = h;  // DIMLTYPE, DIMLTEX1, DIMLTEX2
            var = -1;
            continue;
        }
        if (g.code == 1040 || g.code == 1070 || g.code == 1071) {
            const double v = g.code == 1040 ? g.number() : static_cast<double>(g.integer());
            if (std::isfinite(v)) {
                switch (var) {
                    case 140:
                        txt = v * k_;
                        break;
                    case 41:
                        asz = v * k_;
                        break;
                    case 40:
                        scale = v;
                        break;
                    case 147:
                        gap = v * k_;
                        break;
                    case 42:
                        exo = v * k_;
                        break;
                    case 44:
                        exe = v * k_;
                        break;
                    case 46:
                        dle = v * k_;
                        break;
                    case 47:
                        tp = v * k_;
                        break;
                    case 48:
                        tm = v * k_;
                        break;
                    case 71:
                        tolOn = static_cast<std::int64_t>(v);
                        break;
                    case 72:
                        limOn = static_cast<std::int64_t>(v);
                        break;
                    case 170:
                        altOn = static_cast<std::int64_t>(v);
                        break;
                    default:
                        extra[var] = v;
                        break;
                }
            }
            var = -1;
        }
    }
    const double factor = leadingHeightFactor(dec(r.raw(1)));
    if (!txt && !asz && !scale && !gap && !exo && !exe && !dle && !tp && !tm && !tolOn && !limOn &&
        !altOn && extra.empty() && handles.empty() && factor == 1)
        return base;
    const DimStyle* b = base ? doc_->dimStyle(base) : nullptr;
    DimStyle ds = b ? *b : DimStyle{};
    if (txt && *txt > 0)
        ds.textHeight = *txt;
    if (asz && *asz > 0)
        ds.arrowSize = *asz;
    if (scale && *scale > 0)
        ds.overallScale = *scale;
    if (gap && *gap >= 0)
        ds.textGap = *gap;
    if (exo && *exo >= 0)
        ds.extLineOffset = *exo;
    if (exe && *exe >= 0)
        ds.extLineExtension = *exe;
    if (dle && *dle >= 0)
        ds.dimLineExtension = *dle;
    ds.textHeight *= factor;
    // Tolerance on/off, limits and the plus and minus values; DIMTOL with equal values is
    // symmetrical, with different ones a deviation, DIMLIM shows the two limits.
    if (tp && *tp >= 0)
        ds.tolUpper = *tp;
    if (tm && *tm >= 0)
        ds.tolLower = *tm;
    if (limOn && *limOn != 0)
        ds.tolerance = DimTolerance::Limits;
    else if (tolOn && *tolOn != 0)
        ds.tolerance =
            ds.tolUpper == ds.tolLower ? DimTolerance::Symmetrical : DimTolerance::Deviation;
    else if ((tolOn && *tolOn == 0) || (limOn && *limOn == 0))
        ds.tolerance = DimTolerance::None;
    if (altOn)
        ds.altUnits = *altOn != 0;
    // Arrow blocks named by handle (DIMBLK, DIMBLK1, DIMBLK2), with DIMSAH as overridden or as
    // the style has it. DIMTSZ, below, still turns the arrows into ticks.
    std::string extraKey;
    if (const auto h = handles.find(340); h != handles.end()) {
        // DIMTXSTY: the text style, and its fixed height when it has one.
        if (const TextStyle* t = doc_->textStyleByName(styleNameByHandle_.at(h->second))) {
            ds.textStyle = t->id;
            if (const auto f = styleHeights_.find(h->second);
                f != styleHeights_.end() && ds.overallScale > 0 && !txt)
                ds.textHeight = f->second / ds.overallScale * factor;
            extraKey += "|txsty=" + std::to_string(t->id.value);
        }
        handles.erase(h);
    }
    for (int code : {345, 346, 347}) {
        const auto h = handles.find(code);
        if (h == handles.end())
            continue;
        std::string name = ltypeNameByHandle_.at(h->second);
        if (isByBlock(name))
            name.clear();
        (code == 345 ? ds.dimLinetype : code == 346 ? ds.ext1Linetype : ds.ext2Linetype) = name;
        extraKey += "|lt" + std::to_string(code) + "=" + name;
        handles.erase(h);
    }
    if (const auto h = handles.find(341); h != handles.end()) {
        ds.leaderArrow = arrowKindOf(h->second, ArrowType::ClosedFilled);
        extraKey += "|ldr=" + std::to_string(static_cast<int>(ds.leaderArrow));
        handles.erase(h);
    }
    if (!handles.empty()) {
        const auto a = dimArrows_.find(ds.name);
        DimArrows arrows = a != dimArrows_.end()
                               ? a->second
                               : DimArrows{ArrowType::ClosedFilled, ArrowType::ClosedFilled,
                                           ArrowType::ClosedFilled};
        const auto sahOv = extra.find(173);
        const bool sah = sahOv != extra.end()
                             ? sahOv->second != 0
                             : ds.arrowType2.has_value() ||
                                   (ds.arrowType == arrows.blk1 && arrows.blk1 != arrows.blk);
        if (const auto h = handles.find(342); h != handles.end())
            arrows.blk = arrowKindOf(h->second, ArrowType::ClosedFilled);
        if (const auto h = handles.find(343); h != handles.end())
            arrows.blk1 = arrowKindOf(h->second, ArrowType::ClosedFilled);
        if (const auto h = handles.find(344); h != handles.end())
            arrows.blk2 = arrowKindOf(h->second, ArrowType::ClosedFilled);
        std::tie(ds.arrowType, ds.arrowType2) = arrows.with(sah);
        extraKey += "|arrow=" + std::to_string(static_cast<int>(ds.arrowType));
        if (ds.arrowType2)
            extraKey += "|arrow2=" + std::to_string(static_cast<int>(*ds.arrowType2));
    }
    // The other variables an override can carry (number format, text rules, colours, spacing).
    for (const auto& [varCode, v] : extra) {
        const auto whole = static_cast<std::int64_t>(v);
        switch (varCode) {
            case 271:  // DIMDEC
                ds.precision = static_cast<int>(std::clamp<std::int64_t>(whole, 0, 16));
                break;
            case 272:  // DIMTDEC
                ds.tolPrecision = static_cast<int>(std::clamp<std::int64_t>(whole, 0, 16));
                break;
            case 144:  // DIMLFAC
                if (v > 0)
                    ds.linearFactor = v;
                break;
            case 290:  // DIMFXLON
                ds.fixedExtLines = whole != 0;
                break;
            case 49:  // DIMFXL
                if (v >= 0)
                    ds.fixedExtLength = v * k_;
                break;
            case 148:  // DIMALTRND
                if (v >= 0)
                    ds.altRoundOff = v;
                break;
            case 146:  // DIMTFAC
                if (v > 0)
                    ds.tolHeightFactor = std::min(v, 10.0);
                break;
            case 142:  // DIMTSZ: oblique strokes of that size instead of arrowheads
                if (v > 0) {
                    ds.arrowType = ArrowType::ArchTick;
                    ds.arrowType2.reset();
                    ds.arrowSize = v * k_;
                }
                break;
            case 145:  // DIMTVP
                ds.textVertPos = std::clamp(v, -100.0, 100.0);
                break;
            case 45:  // DIMRND
                if (v >= 0)
                    ds.roundOff = v * k_;
                break;
            case 73:
                ds.textAlign = whole ? DimTextAlign::Horizontal : DimTextAlign::WithDimLine;
                break;
            case 74:
                ds.textAlignOutside = whole ? DimTextAlign::Horizontal : DimTextAlign::WithDimLine;
                break;
            case 77:
                ds.textPlacement = whole == 4   ? DimTextPlacement::Below
                                   : whole != 0 ? DimTextPlacement::AboveLine
                                                : DimTextPlacement::Centered;
                break;
            case 75:
                ds.suppressExt1 = whole != 0;
                break;
            case 76:
                ds.suppressExt2 = whole != 0;
                break;
            case 281:
                ds.suppressDim1 = whole != 0;
                break;
            case 282:
                ds.suppressDim2 = whole != 0;
                break;
            case 172:
                ds.forceLineInside = whole != 0;
                break;
            case 174:
                ds.textInsideForced = whole != 0;
                break;
            case 173:  // DIMSAH: the separate arrows (DIMBLK1) instead of DIMBLK
                if (!handles.empty())
                    break;  // decided with the overridden blocks below
                if (const auto a = dimArrows_.find(ds.name); a != dimArrows_.end())
                    std::tie(ds.arrowType, ds.arrowType2) = a->second.with(whole != 0);
                else if (whole != 0) {
                    ds.arrowType = ArrowType::ClosedFilled;
                    ds.arrowType2.reset();
                }
                break;
            case 175:
                ds.suppressArrowsOutside = whole != 0;
                break;
            case 78:  // DIMZIN
                ds.suppressLeadingZeros = (whole & 4) != 0;
                ds.suppressTrailingZeros = (whole & 8) != 0;
                break;
            case 280:
                ds.textJustify = whole == 1 || whole == 2 ? static_cast<int>(whole) : 0;
                break;
            case 283:  // DIMTOLJ
                ds.tolJustify = whole >= 0 && whole <= 2 ? static_cast<int>(whole) : 1;
                break;
            case 90:  // DIMARCSYM
                ds.arcSymbol = whole >= 0 && whole <= 2 ? static_cast<int>(whole) : 0;
                break;
            case 50:  // DIMJOGANG
                ds.jogAngle = dimJogAngle(v);
                break;
            case 79:  // DIMAZIN
                ds.angularZeros = static_cast<int>(whole & 3);
                break;
            case 284:  // DIMTZIN
            case 285:  // DIMALTZ
                (var == 284 ? ds.tolZeros : ds.altZeros) =
                    ((whole & 4) != 0 ? 1 : 0) | ((whole & 8) != 0 ? 2 : 0);
                break;
            case 279:
                ds.textMove = whole == 1   ? DimTextMove::Leader
                              : whole == 2 ? DimTextMove::Free
                                           : DimTextMove::WithLine;
                break;
            case 289:
                ds.fit = whole == 0   ? DimFit::BothOutside
                         : whole == 1 ? DimFit::ArrowsFirst
                         : whole == 2 ? DimFit::TextFirst
                                      : DimFit::Best;
                break;
            case 371:  // DIMLWD
            case 372:  // DIMLWE
                if (whole >= -3 && whole <= 211)
                    (var == 371 ? ds.dimLineWeight : ds.extLineWeight) =
                        LineWeight{static_cast<std::int16_t>(whole)};
                break;
            case 176:
            case 177:
            case 178: {
                Color& c = var == 176   ? ds.dimLineColor
                           : var == 177 ? ds.extLineColor
                                        : ds.textColor;
                if (whole == 0)
                    c = Color::byBlock();
                else if (whole == 256)
                    c = Color::byLayer();
                else if (whole >= 1 && whole <= 255)
                    c = Color::index(static_cast<std::uint8_t>(whole));
                break;
            }
            case 275:
                ds.angleFormat = whole == 1   ? DimAngleFormat::Dms
                                 : whole == 2 ? DimAngleFormat::Gradians
                                 : whole == 3 ? DimAngleFormat::Radians
                                              : DimAngleFormat::Degrees;
                break;
            case 277:
                ds.linearFormat = whole == 4   ? DimLinearFormat::Architectural
                                  : whole == 5 ? DimLinearFormat::Fractional
                                               : DimLinearFormat::Decimal;
                break;
            case 179:
                ds.angularPrecision = static_cast<int>(std::clamp<std::int64_t>(whole, -1, 8));
                break;
            case 278:
                ds.commaDecimal = whole == ',';
                break;
            case 43:
                if (v >= 0)
                    ds.baselineSpacing = v * k_;
                break;
            case 141:
                ds.centerMark = v * k_;
                break;
            case 69:
                ds.textFill = static_cast<int>(std::clamp<std::int64_t>(whole, 0, 2));
                break;
            default:
                continue;  // not a variable of ours
        }
        extraKey += "|" + std::to_string(var) + "=" + std::to_string(v);
    }
    if (!txt && !asz && !scale && !gap && !exo && !exe && !dle && !tp && !tm && !tolOn && !limOn &&
        !altOn && extraKey.empty() && factor == 1)
        return base;  // only variables of no use to us
    char key[320];
    std::snprintf(key, sizeof key, "%u|%.9g|%.9g|%.9g|%.9g|%.9g|%.9g|%.9g|%.9g|%d|%.9g|%.9g|%d",
                  base.value, ds.textHeight, ds.arrowSize, ds.overallScale, ds.textGap,
                  ds.extLineOffset, ds.extLineExtension, ds.dimLineExtension, factor,
                  static_cast<int>(ds.tolerance), ds.tolUpper, ds.tolLower, ds.altUnits ? 1 : 0);
    const std::string fullKey = std::string(key) + extraKey;
    auto it = dimOverrideStyles_.find(fullKey);
    if (it != dimOverrideStyles_.end())
        return it->second;
    ds.name = (b ? b->name : std::string("Standard")) + "$" +
              std::to_string(dimOverrideStyles_.size() + 1);
    ds.id = DimStyleId{};
    tbcad::Result<DimStyleId> id = tx_->addDimStyle(std::move(ds));
    if (!id)
        return base;
    dimOverrideStyles_[fullKey] = id.value();
    return id.value();
}

bool Importer::convDimension(const Record& r, std::vector<Entity>& out, BlockId /*owner*/) {
    const std::int64_t flags = r.integer(70);
    DimensionData d;
    bool isLength = true;
    switch (flags & 7) {
        case 0:
            d.kind = DimKind::Linear;
            d.rotation = r.num(50) * kDeg;
            d.defPts = {P(r, 13, 23), P(r, 14, 24), P(r, 10, 20), Vec2{}};
            break;
        case 1:
            d.kind = DimKind::Aligned;
            d.defPts = {P(r, 13, 23), P(r, 14, 24), P(r, 10, 20), Vec2{}};
            break;
        case 2:
            d.kind = DimKind::Angular2Line;
            d.defPts = {P(r, 13, 23), P(r, 14, 24), P(r, 15, 25), P(r, 10, 20)};
            isLength = false;
            break;
        case 3:
            d.kind = DimKind::Diameter;
            d.defPts = {P(r, 10, 20), P(r, 15, 25), Vec2{}, Vec2{}};
            break;
        case 4:
            d.kind = DimKind::Radius;
            d.defPts = {P(r, 10, 20), P(r, 15, 25), Vec2{}, Vec2{}};
            break;
        case 5:
            d.kind = DimKind::Angular3Point;
            d.defPts = {P(r, 13, 23), P(r, 14, 24), P(r, 15, 25), P(r, 10, 20)};
            isLength = false;
            break;
        case 6:
            d.kind = DimKind::Ordinate;
            d.rotation = (flags & 64) ? 0 : kPi / 2;
            d.oblique = r.num(51) * kDeg;
            d.defPts = {P(r, 13, 23), P(r, 14, 24), P(r, 10, 20), Vec2{}};
            break;
        default:
            report_.info(code::kBadEntity,
                         "DIMENSION: unknown dimension type, kept as drawn graphics only", r.line);
            Entity none;
            if (opts_.dimensionGraphics)
                appendDimensionGraphics(r, none, out);
            return true;
    }
    // Extension lines that lean (DIMEDIT Oblique) and text turned by hand (DIMEDIT Rotate,
    // DIMTEDIT Angle; an absolute angle, as AutoCAD writes it).
    if (d.kind == DimKind::Linear || d.kind == DimKind::Aligned) {
        const Vec2 along = d.defPts[1] - d.defPts[0];
        const double dimAngle =
            d.kind == DimKind::Linear ? d.rotation : std::atan2(along.y, along.x);
        d.oblique = leanOf(r.num(52) * kDeg, dimAngle);
    }
    if (const double rot = r.num(53); rot != 0 && std::isfinite(rot)) {
        d.textRotated = true;
        d.textRotation = rot * kDeg;
    }
    d.textPos = P(r, 11, 21);
    // Two-line angular dimensions keep the point on their arc in textPos (see docs/dim.md).
    if ((flags & 7) == 2 && r.has(16))
        d.textPos = P(r, 16, 26);
    d.textPosUser = (flags & 128) != 0;
    std::string text = stripMText(dimensionLines(dec(r.raw(1))));
    if (text != "<>")
        d.textOverride = text;
    d.style = dimStyleWithOverrides(r, dimStyleFor(r.raw(3)));
    d.measurement = r.num(42) * (isLength ? k_ : 1.0);
    if (!(d.measurement > 0) || !std::isfinite(d.measurement)) {
        // Many writers omit the measurement; derive it from the definition points.
        switch (d.kind) {
            case DimKind::Linear: {
                Vec2 dir{std::cos(d.rotation), std::sin(d.rotation)};
                d.measurement = std::abs(dot(d.defPts[1] - d.defPts[0], dir));
                break;
            }
            case DimKind::Aligned:
                d.measurement = distance(d.defPts[0], d.defPts[1]);
                break;
            case DimKind::Radius:
            case DimKind::Diameter:
                d.measurement = distance(d.defPts[0], d.defPts[1]);
                break;
            case DimKind::Ordinate:
                d.measurement = dim::measurement(d);
                break;
            default:
                d.measurement = 0;
                break;
        }
    }
    // Group 11 is where AutoCAD shows the text: its middle point (or the point on the dimension
    // line under text lifted above it), written whether or not the text was moved by hand
    // (AutoCAD's own fit rules and font widths put it there, a radius dimension's text goes where
    // it was picked). Text that is not where TBCAD would put it
    // counts as placed; a file TBCAD wrote keeps its text where TBCAD puts it anyway. A point
    // far from the dimension is not trusted (some writers leave it at the origin).
    const bool textAtPoint = d.kind == DimKind::Linear || d.kind == DimKind::Aligned ||
                             d.kind == DimKind::Radius || d.kind == DimKind::Diameter;
    if (textAtPoint && r.has(11) && !d.textPosUser) {
        const dim::Layout auto_ = dim::layoutDimension(*doc_, d, 0.01, 1);
        const double h = std::max(auto_.text.height, 1e-9);
        BBox2 near;
        const bool radial = d.kind == DimKind::Radius || d.kind == DimKind::Diameter;
        for (int i = 0; i < (radial ? 2 : 3); ++i)
            near.expand(d.defPts[static_cast<std::size_t>(i)]);
        near = near.inflated(std::max(near.width(), near.height()) + 50 * h);
        if (near.contains(d.textPos) &&
            (!auto_.text.valid() || distance(auto_.text.point, d.textPos) > 0.01 * h))
            d.textPosUser = true;
    }
    Entity e;
    e.data = Indirect<DimensionData>(std::move(d));
    if (opts_.dimensionCompare) {
        e.common = commonOf(r);
        compareDimension(r, e);
        e.common = {};
    }
    if (opts_.dimensionGraphics)
        appendDimensionGraphics(r, e, out);
    out.insert(out.begin(), std::move(e));
    return true;
}

// ARC_DIMENSION: the length of an arc, from its centre, the two ends and a point on the
// dimension arc. The measured arc runs counter-clockwise from the first end to the second.
bool Importer::convArcDimension(const Record& r, std::vector<Entity>& out) {
    if (!r.has(13) || !r.has(14) || !r.has(15)) {
        bad(r, "ARC_DIMENSION", "definition points missing; skipped");
        return false;
    }
    DimensionData d;
    d.kind = DimKind::ArcLength;
    d.defPts = {P(r, 15, 25), P(r, 13, 23), P(r, 14, 24), P(r, 10, 20)};
    d.textPos = P(r, 11, 21);
    d.textPosUser = (r.integer(70) & 128) != 0;
    const std::string text = stripMText(dimensionLines(dec(r.raw(1))));
    if (text != "<>")
        d.textOverride = text;
    d.style = dimStyleWithOverrides(r, dimStyleFor(r.raw(3)));
    d.measurement = dim::measurement(d);
    Entity e;
    e.data = Indirect<DimensionData>(std::move(d));
    out.push_back(std::move(e));
    return true;
}

const BlockDef* Importer::dimensionBlock(const Record& r) {
    std::string name = dec(r.raw(2));
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    const auto it = blockByFold_.find(name);
    const BlockDef* def = it == blockByFold_.end() ? nullptr : doc_->block(it->second);
    return def && !def->entities.empty() ? def : nullptr;
}

void Importer::dimensionBlockEntities(const BlockDef& def, std::vector<Entity>& shapes,
                                      std::vector<Entity>& texts) {
    struct Walker {
        Importer& self;
        std::vector<Entity>& shapes;
        std::vector<Entity>& texts;
        void run(const BlockDef& block, const std::optional<Transform2>& t, int depth) {
            for (EntityId id : block.entities) {
                const Entity* e = self.doc_->find(id);
                if (!e)
                    continue;
                if (const auto* ins = entityAs<InsertData>(*e)) {
                    const BlockDef* nested = self.doc_->block(ins->block);
                    if (!nested || depth >= kMaxInsertDepth)
                        continue;
                    Transform2 local =
                        compose(Transform2::translate(ins->position),
                                compose(Transform2::rotate(ins->rotation),
                                        compose(Transform2::scale(ins->scale.x, ins->scale.y),
                                                Transform2::translate(-nested->base))));
                    run(*nested, t ? compose(*t, local) : local, depth + 1);
                    continue;
                }
                Entity c = *e;
                if (t) {
                    tbcad::Result<Entity> moved = geom::transformed(c, *t);
                    if (!moved)
                        continue;
                    c = std::move(moved.value());
                }
                EntityKind k = kind(c);
                (k == EntityKind::Text || k == EntityKind::MText ? texts : shapes)
                    .push_back(std::move(c));
            }
        }
    };
    Walker{*this, shapes, texts}.run(def, std::nullopt, 0);
}

void Importer::appendDimensionGraphics(const Record& r, const Entity& /*dim*/,
                                       std::vector<Entity>& out) {
    const BlockDef* def = dimensionBlock(r);
    if (!def) {
        report_.info(code::kNoDimensionBlock,
                     "DIMENSION: the drawn dimension block \"" + dec(r.raw(2)) +
                         "\" is missing; no graphics kept",
                     r.line);
        return;
    }

    const EntityCommon dc = commonOf(r);
    Entity styleHolder;
    styleHolder.common = dc;
    styleHolder.data = PointData{};
    geom::DisplayContext ctx;
    ctx.chordTolMm = 0.01;
    DisplayStyle dimStyle = geom::resolveStyle(*doc_, styleHolder, ctx);
    ctx.byBlockColor = dimStyle.color;
    ctx.byBlockLineweightMm = dimStyle.lineweight;
    ctx.byBlockPattern = dimStyle.pattern;

    std::vector<Entity> shapes, texts;
    dimensionBlockEntities(*def, shapes, texts);
    // The block is drawn at the dimension's group 12 point (zero unless the dimension was copied
    // or moved without its block being made again).
    if (const Vec2 shift = r.has(12) ? P(r, 12, 22) : Vec2{}; shift.x != 0 || shift.y != 0) {
        const Transform2 move = Transform2::translate(shift);
        for (std::vector<Entity>* list : {&shapes, &texts})
            for (Entity& e : *list)
                if (Result<Entity> m = geom::transformed(e, move); m.ok())
                    e = std::move(m.value());
    }

    ProxyData proxy;
    proxy.originalType = "DIMENSION";
    for (Entity& s : shapes) {
        s.common.layer = dc.layer;
        s.common.level = dc.level;
        if (s.common.color.kind == ColorKind::ByBlock)
            s.common.color = dc.color;
        if (const auto* h = entityAs<HatchData>(s)) {
            DisplayFill fill;
            fill.style = geom::resolveStyle(*doc_, s, ctx);
            for (const HatchLoop& loop : h->loops) {
                std::vector<Vec2> ring;
                for (const HatchEdge& edge : loop.edges) {
                    if (const auto* pe = std::get_if<HatchPolylineEdge>(&edge)) {
                        for (std::size_t i = 0; i < pe->v.size(); ++i) {
                            if (i == 0)
                                ring.push_back(pe->v[0].p);
                            std::size_t j = i + 1;
                            if (j < pe->v.size())
                                bulgeArc(pe->v[i].p, pe->v[j].p, pe->v[i].bulge, ring);
                            else if (pe->closed && pe->v.size() > 1)
                                bulgeArc(pe->v[i].p, pe->v[0].p, pe->v[i].bulge, ring);
                        }
                    } else if (const auto* le = std::get_if<HatchLineEdge>(&edge)) {
                        if (ring.empty())
                            ring.push_back(le->a);
                        ring.push_back(le->b);
                    }
                }
                if (ring.size() >= 3)
                    fill.rings.push_back(std::move(ring));
            }
            if (!fill.rings.empty())
                proxy.graphics.items.emplace_back(std::move(fill));
        } else {
            geom::buildDisplay(*doc_, s, ctx, proxy.graphics);
        }
    }
    if (!proxy.graphics.items.empty()) {
        Entity pe;
        pe.common = dc;
        pe.data = Indirect<ProxyData>(std::move(proxy));
        out.push_back(std::move(pe));
    }
    for (Entity& t : texts) {
        t.common.layer = dc.layer;
        t.common.level = dc.level;
        if (t.common.color.kind == ColorKind::ByBlock)
            t.common.color = dc.color;
        out.push_back(std::move(t));
    }
}

}  // namespace tbcad::dxf::detail
