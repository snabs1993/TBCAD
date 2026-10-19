// DXF import: file structure, header, tables, layers and blocks.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <unordered_set>

#include "importer.h"
#include "keep.h"
#include "tbcad/blocks/array.h"
#include "tbcad/dim/dim.h"

namespace tbcad::dxf::detail {

namespace {

constexpr int kMaxSubLayerDepth = 8;

std::string fold(std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

bool isLayoutBlock(const std::string& foldedName) {
    return foldedName == "*model_space" || foldedName == "$model_space" ||
           foldedName.rfind("*paper_space", 0) == 0 || foldedName.rfind("$paper_space", 0) == 0;
}

struct UnitInfo {
    const char* name;
    double mm;
};

// Index = $INSUNITS code.
constexpr UnitInfo kUnits[] = {
    {"unitless", 1},
    {"inches", 25.4},
    {"feet", 304.8},
    {"miles", 1609344.0},
    {"millimeters", 1},
    {"centimeters", 10},
    {"meters", 1000},
    {"kilometers", 1e6},
    {"microinches", 25.4e-6},
    {"mils", 0.0254},
    {"yards", 914.4},
    {"angstroms", 1e-7},
    {"nanometers", 1e-6},
    {"micrometers", 1e-3},
    {"decimeters", 100},
    {"decameters", 1e4},
    {"hectometers", 1e5},
    {"gigameters", 1e12},
    {"astronomical units", 1.495978707e14},
    {"light years", 9.4607304725808e18},
    {"parsecs", 3.0856775814914e19},
};
constexpr int kUnitCount = static_cast<int>(sizeof(kUnits) / sizeof(kUnits[0]));

const char* versionName(const std::string& v) {
    struct V {
        const char* code;
        const char* name;
    };
    static constexpr V kVersions[] = {
        {"AC1006", "R10"},   {"AC1009", "R12"},   {"AC1012", "R13"},   {"AC1014", "R14"},
        {"AC1015", "R2000"}, {"AC1018", "R2004"}, {"AC1021", "R2007"}, {"AC1024", "R2010"},
        {"AC1027", "R2013"}, {"AC1032", "R2018"},
    };
    for (const V& x : kVersions) {
        if (v == x.code)
            return x.name;
    }
    return "";
}

// "AC1009" -> 1009; 0 when not of that shape.
int versionNumber(const std::string& v) {
    if (v.size() != 6 || v.compare(0, 2, "AC") != 0)
        return 0;
    return std::atoi(v.c_str() + 2);
}

std::vector<std::string> splitOn(const std::string& name, const std::string& sep) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (true) {
        std::size_t hit = name.find(sep, pos);
        if (hit == std::string::npos) {
            parts.push_back(name.substr(pos));
            break;
        }
        parts.push_back(name.substr(pos, hit - pos));
        pos = hit + sep.size();
    }
    // Empty segments (leading, trailing or doubled separators) are part of the name.
    std::vector<std::string> out;
    std::string carry;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) {
            carry += sep;
        } else {
            out.push_back(carry + parts[i]);
            carry.clear();
        }
    }
    if (!carry.empty()) {
        if (out.empty())
            out.push_back(carry);
        else
            out.back() += carry;
    }
    return out;
}

std::string joinFirst(const std::vector<std::string>& segs, std::size_t n, const std::string& sep) {
    std::string r;
    for (std::size_t i = 0; i < n; ++i) {
        if (i)
            r += sep;
        r += segs[i];
    }
    return r;
}

}  // namespace

std::string Importer::dec(std::string_view raw) const {
    bool plain = true;
    for (char c : raw) {
        if (static_cast<unsigned char>(c) >= 0x80 || c == '\\') {
            plain = false;
            break;
        }
    }
    if (plain)
        return std::string(raw);
    return decodeText(raw, enc_);
}

// ---------------------------------------------------------------------------------------------
// Header and units

void Importer::setUnits(int insunits, bool assumed) {
    if (opts_.insunits) {
        insunits = *opts_.insunits;
        assumed = false;
    }
    if (insunits <= 0 || insunits >= kUnitCount) {
        if (insunits >= kUnitCount) {
            report_.warn(code::kUnitsAssumed,
                         "unknown $INSUNITS value " + std::to_string(insunits) + "; assuming mm");
        }
        insunits = 4;
        assumed = true;
    }
    sum_.insunits = assumed ? 0 : insunits;
    sum_.unitsAssumed = assumed;
    sum_.unitsName = assumed ? "millimeters" : kUnits[insunits].name;
    k_ = kUnits[insunits].mm;
    sum_.unitScale = k_;
    displayUnit_ = LengthUnit::Millimeter;
    if (!assumed) {
        switch (insunits) {
            case 1:
                displayUnit_ = LengthUnit::Inch;
                break;
            case 2:
                displayUnit_ = LengthUnit::Foot;
                break;
            case 5:
                displayUnit_ = LengthUnit::Centimeter;
                break;
            case 6:
                displayUnit_ = LengthUnit::Meter;
                break;
            default:
                break;
        }
    }
    if (assumed) {
        report_.info(code::kUnitsAssumed,
                     "the file does not state its drawing units; assuming millimeters");
    } else if (k_ != 1) {
        report_.info(code::kUnitsScaled, std::string("drawing units are ") + kUnits[insunits].name +
                                             "; every length was multiplied by " +
                                             formatDouble(k_) + " to convert to millimeters");
    }
}

void Importer::parseHeader(const Record& rec) {
    std::string var;
    int insunits = 0;
    int unitMarker = 0;
    std::string markerVar;
    bool haveUnits = false;
    double ltscale = 1, celtscale = 1;
    std::int64_t psltscale = -1, msltscale = -1, plinegen = -1, fillmode = -1, attmode = -1,
                 qtextmode = -1, mirrtext = -1, pstylemode = -1;
    std::string clayer, ucsName;
    double ucsOx = 0, ucsOy = 0, ucsXx = 1, ucsXy = 0;
    bool haveUcs = false;
    double limMin[2] = {0, 0}, limMax[2] = {0, 0};
    bool haveLimits = false;
    for (const Group& g : rec.g) {
        if (g.code == 9) {
            var = std::string(g.str());
            continue;
        }
        if (var == "$ACADVER" && g.code == 1) {
            acadVer_ = std::string(g.str());
        } else if (var == "$INSUNITS" && g.code == 70) {
            insunits = static_cast<int>(g.integer());
            haveUnits = true;
        } else if (var.compare(0, 6, "$USERI") == 0 && var.size() == 7 && g.code == 70 &&
                   g.integer() > 8100 &&
                   g.integer() <= 8106) {
            // TBCAD's own note of the unit it shows lengths in (all lengths are mm in the file)
            unitMarker = static_cast<int>(g.integer()) - 8100;
            markerVar = var;
        } else if (var == "$LTSCALE" && g.code == 40) {
            ltscale = g.number();
        } else if (var == "$CELTSCALE" && g.code == 40) {
            celtscale = g.number();
        } else if (var == "$PSLTSCALE" && g.code == 70) {
            psltscale = g.integer();
        } else if (var == "$MSLTSCALE" && g.code == 70) {
            msltscale = g.integer();
        } else if (var == "$QTEXTMODE" && g.code == 70) {
            qtextmode = g.integer();
        } else if (var == "$PSTYLEMODE" && g.code == 70) {
            pstylemode = g.integer();
        } else if (var == "$FILLMODE" && g.code == 70) {
            fillmode = g.integer();
        } else if (var == "$ATTMODE" && g.code == 70) {
            attmode = g.integer();
        } else if (var == "$PLINEGEN" && g.code == 70) {
            plinegen = g.integer();
        } else if (var == "$MIRRTEXT" && g.code == 70) {
            mirrtext = g.integer();
        } else if ((var == "$LIMMIN" || var == "$LIMMAX") && (g.code == 10 || g.code == 20)) {
            (var == "$LIMMIN" ? limMin : limMax)[g.code == 10 ? 0 : 1] = g.number();
            haveLimits = true;
        } else if (var == "$UCSNAME" && g.code == 2) {
            ucsName = dec(std::string(g.str()));
        } else if (var == "$UCSORG" && (g.code == 10 || g.code == 20)) {
            (g.code == 10 ? ucsOx : ucsOy) = g.number();
            haveUcs = true;
        } else if (var == "$UCSXDIR" && (g.code == 10 || g.code == 20)) {
            (g.code == 10 ? ucsXx : ucsXy) = g.number();
        } else if (var == "$CLAYER" && g.code == 8) {
            clayer = std::string(g.str());
        }
    }
    setUnits(insunits, !haveUnits);
    if (unitMarker != 0 && k_ == 1) {
        displayUnit_ = unitMarker == 1   ? LengthUnit::Inch
                       : unitMarker == 2 ? LengthUnit::Foot
                       : unitMarker == 5 ? LengthUnit::Centimeter
                       : unitMarker == 6 ? LengthUnit::Meter
                                         : LengthUnit::Millimeter;
    }

    // Variables the writer does not derive from the drawing are kept as they were read.
    std::vector<std::pair<std::string, std::string>> kept;
    {
        std::string name, packed;
        bool usable = true;
        auto flush = [&] {
            if (!name.empty() && usable && !packed.empty() &&
                !(name.compare(0, 6, "$USERI") == 0 && unitMarker != 0 && name.size() == 7 &&
                  markerVar == name) &&
                (!ownedHeaderVar(name) || overridableHeaderVar(name)))
                kept.emplace_back(name, packed);
            name.clear();
            packed.clear();
            usable = true;
        };
        for (const Group& g : rec.g) {
            if (g.code == 9) {
                flush();
                name = std::string(g.str());
                continue;
            }
            if (name.empty())
                continue;
            std::string text;
            if (handleCode(g.code) ||
                !keptValue(g, [&](std::string_view v) { return dec(v); }, text))
                usable = false;
            packGroup(packed, g.code, text);
        }
        flush();
    }
    (void)tx_->modifyHeader([&](Header& h) {
        for (auto& [n, v] : kept)
            h.custom[std::string(kKeptVarPrefix) + n] = std::move(v);
        if (std::isfinite(ltscale) && ltscale > 0)
            h.ltScale = ltscale;
        if (std::isfinite(celtscale) && celtscale > 0 && celtscale != 1)
            h.custom[kCeLtScaleKey] = std::to_string(celtscale);
        if (psltscale >= 0)
            h.custom[kPsLtScaleKey] = psltscale ? "1" : "0";
        if (msltscale >= 0)
            h.custom[kMsLtScaleKey] = msltscale ? "1" : "0";
        if (qtextmode > 0)
            h.custom[kQTextKey] = "1";
        if (pstylemode == 0)
            setColorDependentPlotStyles(h, false);
        if (fillmode == 0)
            h.custom[kFillModeKey] = "0";
        if (attmode == 0)
            h.custom[kAttDispKey] = "2";
        else if (attmode == 2)
            h.custom[kAttDispKey] = "1";
        if (mirrtext >= 0)
            h.custom[kMirrTextKey] = mirrtext ? "1" : "0";
        if (plinegen >= 0)
            h.custom[kPlineGenKey] = plinegen ? "1" : "0";
        if (haveLimits && std::isfinite(limMin[0] + limMin[1] + limMax[0] + limMax[1]) &&
            limMin[0] < limMax[0] && limMin[1] < limMax[1] &&
            !(limMin[0] == 0 && limMin[1] == 0 &&  // the defaults of other writers: not a choice
              ((limMax[0] == 420 && limMax[1] == 297) || (limMax[0] == 12 && limMax[1] == 9))))
            setDrawingLimits(h, {limMin[0], limMin[1]}, {limMax[0], limMax[1]});
        h.createdBy = "tbcad-convert";
        h.custom["dxf.acadver"] = acadVer_;
        if (!clayer.empty())
            h.custom["dxf.clayer"] = dec(clayer);
        const double ang = std::atan2(ucsXy, ucsXx);
        if (haveUcs && (ucsOx != 0 || ucsOy != 0 || ang != 0)) {
            for (char& c : ucsName)
                if (c == '\t' || c == ',' || c == '\r' || c == '\n')
                    c = '_';
            char buf[160];
            std::snprintf(buf, sizeof buf, "\t%.17g\t%.17g\t%.17g", ucsOx * k_, ucsOy * k_, ang);
            h.custom["tbcad.ucs.current"] = ucsName + buf;
        }
    });
}

// ---------------------------------------------------------------------------------------------
// Tables

void Importer::skipSection(RecordReader& rr) {
    Record r;
    while (rr.next(r)) {
        if (r.type == "ENDSEC")
            return;
        if (r.type == "SECTION" || r.type == "EOF") {
            rr.pushBack(std::move(r));
            return;
        }
    }
}

// CLASSES: kept as read, except the classes the writer adds itself for the images it writes.
void Importer::processClasses(RecordReader& rr) {
    std::string kept;
    Record r;
    while (rr.next(r)) {
        if (r.type == "ENDSEC")
            break;
        if (r.type == "SECTION" || r.type == "EOF") {
            rr.pushBack(std::move(r));
            break;
        }
        if (r.type != "CLASS")
            continue;
        const std::string_view cls = r.raw(1);
        if (cls == "IMAGE" || cls == "IMAGEDEF" || cls == "IMAGEDEF_REACTOR" || cls == "PDFUNDERLAY" ||
            cls == "PDFDEFINITION")
            continue;
        packGroup(kept, 0, "CLASS");
        for (const Group& g : r.g) {
            std::string text;
            if (keptValue(g, [&](std::string_view v) { return dec(v); }, text))
                packGroup(kept, g.code, text);
        }
    }
    if (!kept.empty())
        (void)tx_->modifyHeader([&](Header& h) { h.custom[kKeptClassesKey] = std::move(kept); });
}

// OBJECTS: only our annotation-scale note is read (the current scale and the scale list); the
// SCALE objects other programs write are not needed because the note carries the same list.
namespace {
std::string upperHandle(std::string_view h) {
    std::string s(h);
    for (char& c : s)
        if (c >= 'a' && c <= 'f')
            c = static_cast<char>(c - 'a' + 'A');
    return s;
}

}  // namespace

// A named page setup (PLOTSETTINGS). Layouts carry the same data inside their own record; only the
// named ones are collected here.
void Importer::readPlotSettings(const Record& r) {
    const std::string name = dec(r.raw(1));
    if (name.empty() || name.front() == '*')
        return;
    sum_.plotSetups.push_back(plotSetupOf(r, name));
}

PlotSetupRecord Importer::plotSetupOf(const Record& r, const std::string& name) {
    PlotSetupRecord p;
    p.name = name;
    p.paperSize = dec(r.raw(4));
    p.styleSheet = dec(r.raw(7));
    const int flags = static_cast<int>(r.integer(70, 0));
    // Group 72 is the unit of the paper: inches, millimetres or pixels (taken as millimetres).
    const double unit = r.integer(72, 1) == 0 ? 25.4 : 1.0;
    p.paperWidthMm = r.num(44) * unit;
    p.paperHeightMm = r.num(45) * unit;
    p.rotation = static_cast<int>(r.integer(73, 0));
    p.plotType = static_cast<int>(r.integer(74, 1));
    p.scaleType = static_cast<int>(r.integer(75, 0));
    p.scaleNum = r.num(142, 1);
    p.scaleDen = r.num(143, 1);
    p.centered = (flags & 4) != 0;
    p.lineweights = (flags & 128) != 0;
    p.originX = r.num(46) * unit;
    p.originY = r.num(47) * unit;
    p.windowMinX = r.num(48);
    p.windowMinY = r.num(49);
    p.windowMaxX = r.num(140);
    p.windowMaxY = r.num(141);
    return p;
}

void Importer::processObjects(RecordReader& rr) {
    Record r;
    while (rr.next(r)) {
        if (r.type == "ENDSEC")
            return;
        if (r.type == "SECTION" || r.type == "EOF") {
            rr.pushBack(std::move(r));
            return;
        }
        storeObject(r);
        if (r.type == "LAYOUT") {
            readLayoutObject(r);
            continue;
        }
        if (r.type == "PLOTSETTINGS") {
            readPlotSettings(r);
            continue;
        }
        if (r.type == "RASTERVARIABLES") {
            // 70 is "frame shown"; only an off frame says something (shown and plotted or not
            // is the IMAGEFRAME value, which the file does not keep).
            if (r.has(70) && r.integer(70) == 0)
                imageFrame_ = 0;
        }
        if (r.type == "WIPEOUTVARIABLES" && r.has(70) && r.integer(70) >= 0 && r.integer(70) <= 2) {
            const std::string mode = std::to_string(r.integer(70));
            (void)tx_->modifyHeader([&](Header& h) { h.custom[kWipeoutFrameKey] = mode; });
        }
        if (r.type == "PDFDEFINITION") {
            PdfDef def;
            def.file = dec(r.raw(1));
            const std::string pg(r.raw(2));
            // The page is named "1", or "Page 3" in some files.
            std::size_t i = pg.find_first_of("0123456789");
            if (i != std::string::npos)
                def.page = std::max(1, std::atoi(pg.c_str() + i));
            pdfDefs_[std::string(r.raw(5))] = std::move(def);
            continue;
        }
        if (r.type == "IMAGEDEF") {
            imageDefs_[std::string(r.raw(5))] = dec(r.raw(1));
            continue;
        }
        if (r.type == "SCALE") {
            const double paper = r.num(140, 0), drawing = r.num(141, 0);
            if (std::isfinite(paper) && std::isfinite(drawing) && paper > 0 && drawing > 0)
                scaleFactors_[std::string(r.raw(5))] = drawing / paper;
            continue;
        }
        {  // AcDbObjectContextData: the first 340 after the marker is the scale it is for
            bool inCtx = false;
            for (const Group& cg : r.g) {
                if (cg.code == 100 && cg.str() == "AcDbObjectContextData")
                    inCtx = true;
                else if (inCtx && cg.code == 340) {
                    contextScale_[std::string(r.raw(5))] = std::string(cg.str());
                    break;
                }
            }
        }
        if (r.type == "DICTIONARY" || r.type == "ACDBDICTIONARYWDFLT") {
            auto& entries = dicts_[std::string(r.raw(5))];
            std::string key;
            for (const Group& dg : r.g) {
                if (dg.code == 3)
                    key = dec(dg.str());
                else if ((dg.code == 350 || dg.code == 360) && !key.empty()) {
                    entries.emplace_back(key, std::string(dg.str()));
                    // The plot style dictionary names its style objects.
                    if (r.type == "ACDBDICTIONARYWDFLT" && dg.code == 350)
                        plotStyleNames_[upperHandle(dg.str())] = key;
                    key.clear();
                }
            }
            continue;
        }
        if (r.type == "XRECORD" && !r.raw(5).empty()) {
            // Everything after the AcDbXrecord marker and its cloning flag, for entity
            // dictionaries.
            std::vector<DataItem> items;
            bool after = false, skippedFlag = false;
            for (const Group& xg : r.g) {
                if (xg.code == 100 && xg.str() == "AcDbXrecord") {
                    after = true;
                    continue;
                }
                if (!after || items.size() >= 4096)
                    continue;
                if (!skippedFlag && xg.code == 280) {
                    skippedFlag = true;
                    continue;
                }
                skippedFlag = true;
                if (xg.code < 0 || xg.code > 369 + 1000)
                    continue;
                DataItem it;
                it.code = xg.code;
                switch (dataKindOf(xg.code)) {
                    case DataKind::Text:
                        it.text = dec(xg.str());
                        break;
                    case DataKind::Integer:
                        it.integer = xg.integer();
                        break;
                    case DataKind::Real:
                        it.real = std::isfinite(xg.number()) ? xg.number() : 0;
                        break;
                }
                items.push_back(std::move(it));
            }
            xrecItems_[std::string(r.raw(5))] = std::move(items);
        }
        if (r.type == "XRECORD") {
            bool afterMarker = false;
            for (const Group& xg : r.g) {
                if (xg.code == 100 && xg.str() == "AcDbXrecord") {
                    afterMarker = true;
                    continue;
                }
                if (afterMarker && (xg.code == 90 || xg.code == 91 || xg.code == 70 ||
                                    xg.code == 1070 || xg.code == 1071)) {
                    xrecNumbers_[std::string(r.raw(5))] = xg.integer();
                    break;
                }
            }
        }
        if (r.type != "XRECORD")
            continue;
        const Group* g = r.find(1);
        if (!g)
            continue;
        constexpr const char* kSketchConstraints = "tbcad.constraints";
        constexpr const char* kSketchParameters = "tbcad.parameters";
        const std::string text = dec(g->str());
        if (text == kSketchConstraints || text == kSketchParameters) {
            std::string body;
            bool first = true;
            for (const Group& xg : r.g) {
                if (xg.code != 1)
                    continue;
                if (first) {
                    first = false;
                    continue;
                }
                body += dec(xg.str());
                body += '\n';
            }
            if (!body.empty() && body.size() < (8u << 20))
                (void)tx_->modifyHeader([&](Header& h) { h.custom[text] = body; });
            continue;
        }
        if (text == "tbcad.settings") {
            std::string key, value;
            std::map<std::string, std::string> found;
            auto finish = [&] {
                if (key.empty())
                    return;
                std::string out;
                for (std::size_t i = 0; i < value.size(); ++i) {
                    if (value[i] == '\\' && i + 1 < value.size()) {
                        const char n = value[++i];
                        out.push_back(n == 'n' ? '\n' : n == 'r' ? '\r' : n);
                    } else {
                        out.push_back(value[i]);
                    }
                }
                found[key] = std::move(out);
                key.clear();
                value.clear();
            };
            bool first = true;
            for (const Group& xg : r.g) {
                if (xg.code != 1)
                    continue;
                if (first) {
                    first = false;
                    continue;
                }
                const std::string line = dec(xg.str());
                if (line.empty())
                    continue;
                if (line[0] == 'K') {
                    finish();
                    key = line.substr(1);
                } else if (line[0] == 'V' && !key.empty() && value.size() < (8u << 20)) {
                    value += line.substr(1);
                }
            }
            finish();
            (void)tx_->modifyHeader([&](Header& h) {
                for (auto& [k, v] : found)
                    if (k.rfind("tbcad.", 0) == 0)
                        h.custom.emplace(k, std::move(v));  // a setting read elsewhere wins
            });
            continue;
        }
        if (text.rfind("current=", 0) != 0)
            continue;
        const std::size_t semi = text.find(";list=");
        if (semi == std::string::npos)
            continue;
        const std::string cur = text.substr(8, semi - 8);
        const std::string list = text.substr(semi + 6);
        char* end = nullptr;
        const double c = std::strtod(cur.c_str(), &end);
        (void)tx_->modifyHeader([&](Header& h) {
            if (end != cur.c_str() && std::isfinite(c) && c > 0)
                h.custom[kAnnoScaleKey] = cur;
            if (!list.empty() && list.size() < 2000)
                h.custom[kAnnoScaleListKey] = list;
        });
    }
}

// ---------------------------------------------------------------------------------------------
// Objects that are kept as they are
//
// The OBJECTS section holds more than TBCAD models: materials, visual styles, multileader and
// table styles, page setups, application dictionaries and data. Everything that hangs off the
// root dictionary under a name the writer does not make itself is kept: the records, with their
// handles replaced by "@N" tokens (numbered in order of appearance; "@root" is the root
// dictionary) that the writer turns into new handles. A reference to something that is not kept
// (an entity, a table record, a layout) becomes the null handle.

namespace {}  // namespace

void Importer::storeObject(const Record& r) {
    const std::string_view h = r.raw(5);
    if (h.empty() || objStore_.size() >= 400000 || objStoreBytes_ > (96u << 20))
        return;
    ObjRec rec;
    rec.type = std::string(r.type);
    rec.g.reserve(r.g.size());
    for (const Group& g : r.g) {
        std::string text;
        if (!keptValue(g, [&](std::string_view v) { return dec(v); }, text))
            continue;
        objStoreBytes_ += text.size() + 16;
        rec.g.emplace_back(g.code, std::move(text));
    }
    if (rec.type == "DICTIONARY" && objRoot_.empty()) {
        const std::string_view owner = r.raw(330);
        if (owner.empty() || owner == "0")
            objRoot_ = upperHandle(h);
    }
    objStore_[upperHandle(h)] = std::move(rec);
}

// The handle of each entity in the source file, for the writer to give back.
void Importer::keepHandles() {
    if (srcHandles_.empty())
        return;
    std::sort(srcHandles_.begin(), srcHandles_.end());
    std::unordered_set<std::uint64_t> seen;
    HandlePairs kept;
    kept.reserve(srcHandles_.size());
    for (const auto& [id, handle] : srcHandles_)
        if (seen.insert(handle).second)
            kept.emplace_back(id, handle);
    std::string packed = packHandles(kept);
    (void)tx_->modifyHeader([&](Header& h) { h.custom[kKeptHandlesKey] = std::move(packed); });
}

// Layers and objects that name a plot style (an .stb table): the name goes into the drawing
// header, see layerPlotStyle(). "Normal" is the default and is not stored.
void Importer::applyPlotStyles() {
    if (plotStyleNames_.empty())
        return;
    auto nameOf = [&](const std::string& handle) {
        auto it = plotStyleNames_.find(upperHandle(handle));
        return it == plotStyleNames_.end() || it->second == "Normal" ? std::string() : it->second;
    };
    std::unordered_map<std::uint64_t, std::uint64_t> entityOfHandle;
    for (const auto& [id, handle] : srcHandles_)
        entityOfHandle.emplace(handle, id);
    (void)tx_->modifyHeader([&](Header& h) {
        for (const LayerRow& row : layerRows_) {
            if (row.plotStyleHandle.empty())
                continue;
            auto lit = layerByFold_.find(fold(row.name));
            const std::string name = nameOf(row.plotStyleHandle);
            if (lit != layerByFold_.end() && !name.empty())
                setLayerPlotStyle(h, lit->second, name);
        }
        for (const auto& [handle, style] : entityPlotStyles_) {
            auto eit = entityOfHandle.find(handle);
            const std::string name = nameOf(style);
            if (eit != entityOfHandle.end() && !name.empty())
                setEntityPlotStyle(h, EntityId{eit->second}, name);
        }
    });
}

// GROUP objects under ACAD_GROUP become the drawing's groups (GROUP / UNGROUP). Members are found
// by the handle their entity had in the file.
void Importer::importGroups() {
    if (objRoot_.empty() || !objStore_.count(objRoot_))
        return;
    std::string dictHandle;
    {
        std::string name;
        for (const auto& [c, v] : objStore_.at(objRoot_).g) {
            if (c == 3)
                name = v;
            else if ((c == 350 || c == 360) && name == "ACAD_GROUP")
                dictHandle = upperHandle(v);
        }
    }
    auto dit = objStore_.find(dictHandle);
    if (dit == objStore_.end())
        return;
    std::unordered_map<std::uint64_t, std::uint64_t> entityOfHandle;
    for (const auto& [id, handle] : srcHandles_)
        entityOfHandle.emplace(handle, id);
    std::vector<tbcad::Group> groups;
    std::string descriptions;  // lines "name TAB text"
    std::string name;
    for (const auto& [c, v] : dit->second.g) {
        if (c == 3) {
            name = v;
            continue;
        }
        if ((c != 350 && c != 360) || name.empty())
            continue;
        auto git = objStore_.find(upperHandle(v));
        if (git == objStore_.end() || git->second.type != "GROUP")
            continue;
        tbcad::Group grp;
        grp.name = name;
        for (const auto& [gc, gv] : git->second.g) {
            if (gc == 71)
                grp.selectable = gv != "0";
            else if (gc == 300 && !gv.empty() && gv.find_first_of("\t\n") == std::string::npos &&
                     name.find_first_of("\t\n") == std::string::npos)
                descriptions += name + '\t' + gv + '\n';
            else if (gc == 340) {
                auto eit = entityOfHandle.find(std::strtoull(gv.c_str(), nullptr, 16));
                if (eit != entityOfHandle.end())
                    grp.members.push_back(EntityId{eit->second});
            }
        }
        name.clear();
        if (!grp.members.empty())
            groups.push_back(std::move(grp));
    }
    if (!groups.empty())
        (void)writeGroups(*tx_, groups);
    if (!groups.empty() && !descriptions.empty())
        (void)tx_->modifyHeader(
            [&](Header& h) { h.custom[kKeptGroupDescKey] = std::move(descriptions); });
}

// SORTENTSTABLE objects give the draw order of model space and of blocks: pairs of an entity
// handle (331) and a sort handle (5, after the object's own); an entity that is not listed sorts
// by its own handle. Without a table the order is the order of the handles, which is the order
// the entities came in the file.
void Importer::importDrawOrder() {
    bool any = false;
    for (const auto& [h, rec] : objStore_)
        if (rec.type == "SORTENTSTABLE") {
            any = true;
            break;
        }
    if (!any)
        return;
    std::unordered_map<std::uint64_t, std::uint64_t> entityOfHandle, handleOf;
    for (const auto& [id, handle] : srcHandles_) {
        entityOfHandle.emplace(handle, id);
        handleOf.emplace(id, handle);
    }
    std::unordered_map<std::uint64_t, std::uint64_t> sortKey;  // entity id -> sort handle
    for (const auto& [h, rec] : objStore_) {
        if (rec.type != "SORTENTSTABLE")
            continue;
        std::uint64_t entity = 0;
        bool haveEntity = false, ownHandle = true;
        for (const auto& [c, v] : rec.g) {
            if (c == 331) {
                auto it = entityOfHandle.find(std::strtoull(v.c_str(), nullptr, 16));
                haveEntity = it != entityOfHandle.end();
                if (haveEntity)
                    entity = it->second;
            } else if (c == 5) {
                if (ownHandle) {  // the first 5 is the handle of the table itself
                    ownHandle = false;
                    continue;
                }
                if (haveEntity)
                    sortKey[entity] = std::strtoull(v.c_str(), nullptr, 16);
                haveEntity = false;
            }
        }
    }
    if (sortKey.empty())
        return;
    // Containers touched: model space (block 0) and the blocks of the listed entities.
    std::set<std::uint64_t> containers;
    for (const auto& [id, key] : sortKey)
        containers.insert(doc_->ownerBlock(EntityId{id}).value);
    auto keyOf = [&](EntityId id, std::uint64_t& last) {
        if (auto it = sortKey.find(id.value); it != sortKey.end())
            return last = it->second;
        if (auto it = handleOf.find(id.value); it != handleOf.end())
            return last = it->second;
        return last;  // made by the importer: stays after the one before it
    };
    for (std::uint64_t container : containers) {
        std::vector<EntityId> cur;
        if (container == 0)
            cur = doc_->modelSpace();
        else if (const BlockDef* def = doc_->block(BlockId{static_cast<std::uint32_t>(container)}))
            cur = def->entities;
        std::uint64_t last = 0;
        std::vector<std::pair<std::uint64_t, EntityId>> keyed;
        keyed.reserve(cur.size());
        for (EntityId id : cur)
            keyed.emplace_back(keyOf(id, last), id);
        std::stable_sort(keyed.begin(), keyed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<EntityId> want;
        want.reserve(keyed.size());
        for (const auto& [k, id] : keyed)
            want.push_back(id);
        if (want != cur)
            (void)tx_->setDrawOrder(want, BlockId{static_cast<std::uint32_t>(container)});
    }
}

namespace {
// An MLINESTYLE object (or its record) as a style; offsets are scaled by k to millimetres.
std::optional<mline::Style> mlineStyleOf(const std::vector<std::pair<int, std::string>>& g,
                                         double k) {
    mline::Style st;
    st.offsets.clear();
    bool named = false;
    int flags = 0, fillColor = 256;
    std::vector<int> colors;
    std::vector<std::string> linetypes;
    for (const auto& [c, v] : g) {
        if (c == 2 && !named) {
            st.name = v;
            named = true;
        } else if (c == 3) {
            st.description = v;
        } else if (c == 70) {
            flags = std::atoi(v.c_str());
        } else if (c == 49) {
            st.offsets.push_back(std::strtod(v.c_str(), nullptr) * k);
            colors.push_back(256);
            linetypes.emplace_back();
        } else if (c == 62) {
            const int aci = std::atoi(v.c_str());
            if (colors.empty())
                fillColor = aci;  // before the first element: the fill colour
            else
                colors.back() = aci >= 0 && aci <= 256 ? aci : 256;
        } else if (c == 6 && !linetypes.empty()) {
            std::string lt = v;
            std::string low = lt;
            for (char& ch : low)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            linetypes.back() = low == "bylayer" || low == "byblock" ? std::string() : lt;
        }
    }
    if (!named || st.name.empty() || st.offsets.empty())
        return std::nullopt;
    st.startCap = (flags & 16) != 0;
    st.endCap = (flags & 256) != 0;
    st.startArc = (flags & 64) ? 1 : (flags & 32) ? 2 : 0;
    st.endArc = (flags & 1024) ? 1 : (flags & 512) ? 2 : 0;
    st.joints = (flags & 2) != 0;
    st.fill = (flags & 1) ? (fillColor >= 0 && fillColor <= 256 ? fillColor : 256) : -1;
    if (std::any_of(colors.begin(), colors.end(), [](int c) { return c != 256; }))
        st.colors = std::move(colors);
    if (std::any_of(linetypes.begin(), linetypes.end(),
                    [](const std::string& l) { return !l.empty(); }))
        st.linetypes = std::move(linetypes);
    return st;
}
}  // namespace

// The styles in the file's MLINESTYLE objects become the drawing's multiline styles.
void Importer::importMlineStyles() {
    std::vector<mline::Style> styles;
    for (const auto& [h, rec] : objStore_) {
        if (rec.type != "MLINESTYLE")
            continue;
        if (auto st = mlineStyleOf(rec.g, k_))
            styles.push_back(std::move(*st));
    }
    for (const auto& [key, st] : mlStyles_)
        styles.push_back(st);
    // The built-in Standard style needs no entry of its own.
    const mline::Style builtin = mline::standardStyle();
    std::erase_if(styles, [&](const mline::Style& st) {
        std::string up = st.name;
        for (char& c : up)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return up == "STANDARD" && st.offsets == builtin.offsets &&
               st.startCap == builtin.startCap && st.endCap == builtin.endCap &&
               st.description == builtin.description;
    });
    if (styles.empty())
        return;
    (void)tx_->modifyHeader([&](Header& hd) {
        for (const mline::Style& st : styles)
            mline::storeStyle(hd, st);
    });
}

// Reads the MLINESTYLE records of the file ahead of the entities (they come later in the file),
// so a multiline can be checked against its style.
void Importer::loadMlineStyles() {
    if (mlStylesLoaded_)
        return;
    mlStylesLoaded_ = true;
    Report scratch;
    std::optional<Lexer> lx;
    if (groups_) {
        lx.emplace(Lexer::fromBuffer(*groups_, scratch));
    } else {
        Result<Lexer> opened = Lexer::open(bytes_, scratch);
        if (!opened)
            return;
        lx.emplace(std::move(opened.value()));
    }
    RecordReader rr(*lx);
    Record rec;
    while (rr.next(rec)) {
        if (rec.type != "MLINESTYLE")
            continue;
        std::vector<std::pair<int, std::string>> g;
        for (const Group& gr : rec.g) {
            std::string text;
            if (keptValue(gr, [&](std::string_view v) { return dec(v); }, text))
                g.emplace_back(gr.code, std::move(text));
        }
        if (auto st = mlineStyleOf(g, k_))
            mlStyles_.emplace(fold(st->name), std::move(*st));
    }
}

void Importer::keepObjects() {
    if (objRoot_.empty() || !objStore_.count(objRoot_))
        return;
    // Names the writer writes itself, and objects that only make sense next to entities or
    // layouts of the source file.
    static const std::set<std::string> ownedNames = {
        "ACAD_GROUP",      "ACAD_IMAGE_DICT",    "ACAD_PDFDEFINITIONS",    "ACAD_IMAGE_VARS",  "ACAD_LAYOUT",
        "ACAD_MLINESTYLE", "ACAD_PLOTSTYLENAME", "ACAD_SCALELIST",   "TBCAD_ANNOSCALE",
        "ACAD_SORTENTS",   "TBCAD_CONSTRAINTS",  "TBCAD_PARAMETERS", "ACAD_WIPEOUT_VARS",
        "TBCAD_SETTINGS"};
    static const std::set<std::string> neverKept = {
        "LAYOUT",   "IMAGEDEF",        "IMAGEDEF_REACTOR", "PDFDEFINITION", "SORTENTSTABLE", "GROUP",
        "DIMASSOC", "IMAGEDEFREACTOR", "ACDBPLACEHOLDER",  "MLINESTYLE"};

    const ObjRec& root = objStore_.at(objRoot_);
    std::vector<std::pair<std::string, std::string>> entries;  // (name, handle)
    {
        std::string name;
        for (const auto& [c, v] : root.g) {
            if (c == 3)
                name = v;
            else if ((c == 350 || c == 360) && !name.empty()) {
                entries.emplace_back(name, upperHandle(v));
                name.clear();
            }
        }
    }

    // The layer table's own dictionary: its entries are kept as root entries with a marker.
    if (auto lit = objStore_.find(layerTableDict_); lit != objStore_.end()) {
        std::string name;
        for (const auto& [c, v] : lit->second.g) {
            if (c == 3)
                name = v;
            else if ((c == 350 || c == 360) && !name.empty()) {
                entries.emplace_back("%LAYERTABLE%" + name, upperHandle(v));
                name.clear();
            }
        }
    }

    // Entities keep their handles in the saved file, so a pointer to one can stay a pointer.
    std::unordered_set<std::uint64_t> entityHandles;
    for (const auto& pr : srcHandles_)
        entityHandles.insert(pr.second);

    std::unordered_map<std::string, std::size_t> token;  // source handle -> index
    std::vector<std::string> order;                      // in token order
    auto admit = [&](const std::string& h) -> bool {
        auto it = objStore_.find(h);
        if (it == objStore_.end() || neverKept.count(it->second.type) || h == objRoot_)
            return false;
        if (token.emplace(h, order.size()).second)
            order.push_back(h);
        return true;
    };

    std::string rootEntries;
    for (const auto& [name, h] : entries) {
        if (ownedNames.count(name) || name.find_first_of("\t\n") != std::string::npos || !admit(h))
            continue;
        rootEntries += name + "\t@" + std::to_string(token[h]) + "\n";
    }
    if (order.empty())
        return;

    std::string packed;
    for (std::size_t i = 0; i < order.size(); ++i) {  // `order` grows as references are found
        const ObjRec& rec = objStore_.at(order[i]);
        packGroup(packed, 0, rec.type);
        int skipDepth = 0;
        for (const auto& [c, v] : rec.g) {
            if (c == 102) {  // {ACAD_REACTORS ... } and {ACAD_XDICTIONARY ... }: not kept
                if (!v.empty() && v[0] == '{')
                    ++skipDepth;
                else if (skipDepth > 0)
                    --skipDepth;
                continue;
            }
            if (skipDepth > 0)
                continue;
            if (c == 5) {
                packGroup(packed, 5, "@" + std::to_string(i));
            } else if (c == 330) {
                const std::string h = upperHandle(v);
                packGroup(packed, 330,
                          h == objRoot_    ? "@root"
                          : token.count(h) ? "@" + std::to_string(token[h])
                                           : "@root");
            } else if (handleCode(c)) {
                const std::string h = upperHandle(v);
                if (!h.empty() && h != "0" && admit(h))
                    packGroup(packed, c, "@" + std::to_string(token[h]));
                else if (!h.empty() && entityHandles.count(std::strtoull(h.c_str(), nullptr, 16)))
                    packGroup(packed, c, "#" + h);
                else if (auto sit = styleNameByHandle_.find(v); sit != styleNameByHandle_.end())
                    packGroup(packed, c, "$S:" + sit->second);  // a text style, by name
                else
                    packGroup(packed, c, "0");
            } else {
                packGroup(packed, c, v);
            }
        }
    }
    (void)tx_->modifyHeader([&](Header& h) {
        h.custom[kKeptObjectsKey] = std::move(packed);
        h.custom[kKeptObjectRootKey] = std::move(rootEntries);
    });
}

void Importer::applyLayerAlphaXrecords() {
    for (const auto& [layerName, dictHandle] : layerXdicts_) {
        auto d = dicts_.find(dictHandle);
        if (d == dicts_.end())
            continue;
        for (const auto& [key, handle] : d->second) {
            if (key != "ADSK_XREC_LAYER_ALPHA")
                continue;
            auto x = xrecNumbers_.find(handle);
            auto l = layerByFold_.find(fold(layerName));
            if (x == xrecNumbers_.end() || l == layerByFold_.end())
                continue;
            // Same encoding as group 440; a bare 0..255 is taken as the alpha byte itself.
            std::int64_t v = x->second;
            if (v == 0)
                continue;
            if ((v & 0xFF000000) == 0)
                v = 0x02000000 | (v & 0xFF);
            const int t = transparencyFromGroup(v);
            if (t < 0)
                continue;
            (void)tx_->modifyLayer(l->second,
                                   [&](Layer& layer) { layer.transparency = std::uint8_t(t); });
        }
    }
}

void Importer::processTables(RecordReader& rr) {
    Record r;
    while (rr.next(r)) {
        if (r.type == "ENDSEC")
            break;
        if (r.type == "SECTION" || r.type == "EOF") {
            report_.warn(code::kMissingSection, "TABLES section is not closed by ENDSEC", r.line);
            rr.pushBack(std::move(r));
            break;
        }
        if (r.type == "TABLE") {
            if (r.raw(2) == "LAYER") {  // its extension dictionary holds the layer filters
                bool inX = false;
                for (const Group& g : r.g) {
                    if (g.code == 102)
                        inX = g.str() == "{ACAD_XDICTIONARY";
                    else if (inX && g.code == 360)
                        layerTableDict_ = upperHandle(g.str());
                }
            }
        } else if (r.type == "LAYER")
            readLayerRow(r);
        else if (r.type == "LTYPE")
            readLinetype(r);
        else if (r.type == "STYLE")
            readStyle(r);
        else if (r.type == "DIMSTYLE")
            readDimStyle(r);
        else if (r.type == "APPID")
            readAppId(r);
        else if (r.type == "UCS")
            readUcs(r);
    }
    createLayers();
    createLevels();
    // Text in a linetype names its style by handle; now the styles are known.
    for (LinetypeId id : marksAwaitingStyles_) {
        (void)tx_->modifyLinetype(id, [&](Linetype& lt) {
            for (LinetypeMark& m : lt.marks) {
                if (m.style.empty() || m.style[0] != '\x01')
                    continue;
                const std::string handle = m.style.substr(1);
                if (m.kind == LinetypeMark::Kind::Shape) {
                    auto sit = shapeFileByHandle_.find(handle);
                    m.style = sit == shapeFileByHandle_.end() ? std::string() : sit->second;
                    continue;
                }
                auto it = styleNameByHandle_.find(handle);
                m.style = it == styleNameByHandle_.end() ? std::string() : it->second;
            }
        });
    }
    marksAwaitingStyles_.clear();
}

// A named coordinate system (2D: the origin and the angle of the X axis). Kept in the drawing's UCS
// table, Header::custom["tbcad.ucs"].
void Importer::readUcs(const Record& r) {
    std::string name = dec(r.raw(2));
    for (char& c : name)
        if (c == '\t' || c == ',' || c == '\r' || c == '\n')
            c = '_';
    if (name.empty() || name.size() > 31)
        return;
    const double a = std::atan2(r.num(21), r.num(11));
    char buf[160];
    std::snprintf(buf, sizeof buf, "\t%.17g\t%.17g\t%.17g\n", r.num(10) * k_, r.num(20) * k_,
                  r.has(11) ? a : 0.0);
    const std::string row = name + buf;
    (void)tx_->modifyHeader([&](Header& h) { h.custom["tbcad.ucs"] += row; });
}

// TBCAD level table: XDATA on the TBCAD application record (written by the exporter).
void Importer::readAppId(const Record& r) {
    bool inOurs = false;
    for (const Group& g : r.g) {
        if (g.code == 1001) {
            inOurs = g.str() == "TBCAD";
            continue;
        }
        if (!inOurs)
            continue;
        if (g.code == 1000) {
            std::string v = dec(g.str());
            if (v.rfind("levels-current=", 0) == 0)
                levelCurrent_ = v.substr(15);
            else if (v.rfind("levels-view=", 0) == 0)
                levelView_ = std::atoi(v.c_str() + 12);
            else if (v.rfind("levels-row=", 0) == 0)
                levelRows_.push_back({v.substr(11), 0, 1});
        } else if (g.code == 1040 && !levelRows_.empty()) {
            double e = g.number();
            levelRows_.back().elevation = std::isfinite(e) ? e : 0;
        } else if (g.code == 1070 && !levelRows_.empty()) {
            levelRows_.back().flags = static_cast<int>(g.integer());
        }
    }
}

void Importer::createLevels() {
    if (levelsCreated_)
        return;
    levelsCreated_ = true;
    auto key = [](const std::string& n) { return fold(n); };
    levelByFold_[key(doc_->level(kLevelDefault)->name)] = kLevelDefault;
    bool first = true;
    for (const LevelRow& row : levelRows_) {
        if (row.name.empty())
            continue;
        Level l;
        l.name = row.name;
        l.elevation = row.elevation * k_;
        l.visible = (row.flags & 1) != 0;
        l.locked = (row.flags & 2) != 0;
        if (first) {
            // The first row is the exporter's first level: it keeps the default level's id.
            first = false;
            (void)tx_->modifyLevel(kLevelDefault, [&](Level& d) { d = l; });
            levelByFold_.clear();
            levelByFold_[key(row.name)] = kLevelDefault;
            continue;
        }
        tbcad::Result<LevelId> id = tx_->addLevel(std::move(l));
        if (id)
            levelByFold_[key(row.name)] = id.value();
    }
    if (!levelCurrent_.empty()) {
        auto it = levelByFold_.find(key(levelCurrent_));
        if (it != levelByFold_.end()) {
            LevelId id = it->second;
            LevelView view = levelView_ >= 0 && levelView_ <= 2 ? static_cast<LevelView>(levelView_)
                                                                : LevelView::DimOthers;
            (void)tx_->modifyHeader([&](Header& h) {
                h.currentLevel = id;
                h.levelView = view;
            });
        }
    }
}

// The level named by the entity's TBCAD XDATA ("level=<name>"); the default level otherwise.
LevelId Importer::levelFor(const Record& r) {
    bool inOurs = false;
    for (const Group& g : r.g) {
        if (g.code == 1001) {
            inOurs = g.str() == "TBCAD";
            continue;
        }
        if (inOurs && g.code == 1000) {
            std::string v = dec(g.str());
            if (v.rfind("level=", 0) == 0) {
                auto it = levelByFold_.find(fold(v.substr(6)));
                if (it != levelByFold_.end())
                    return it->second;
            }
        }
    }
    return kLevelDefault;
}

void Importer::readLinetype(const Record& r) {
    std::string name = dec(r.raw(2));
    std::string f = fold(name);
    if (f.empty() || f == "continuous" || f == "bylayer" || f == "byblock") {
        if (f == "continuous")
            ltypeByRaw_[fold(r.raw(2))] = kLinetypeContinuous;
        return;
    }
    Linetype lt;
    lt.name = name;
    lt.description = dec(r.raw(3));
    double total = 0;
    // Text and shape elements ride on a dash element (group 74): they sit where that element ends.
    LinetypeMark mark;
    bool haveMark = false;
    bool lastZero = false;
    auto closeMark = [&] {
        if (haveMark) {
            mark.offset = total;
            lt.marks.push_back(std::move(mark));
        }
        mark = LinetypeMark{};
        haveMark = false;
    };
    for (const Group& g : r.g) {
        switch (g.code) {
            case 49: {
                closeMark();
                double v = g.number() * k_;
                if (std::isfinite(v)) {
                    lt.pattern.push_back(v);
                    total += std::abs(v);
                    lastZero = v == 0;
                }
                break;
            }
            case 74: {
                const std::int64_t f = g.integer();
                if (f & 6) {
                    haveMark = true;
                    // An element of its own has no length: its 49 was a zero that is no dot.
                    if (lastZero && !lt.pattern.empty())
                        lt.pattern.pop_back();
                    lastZero = false;
                    mark.kind = (f & 4) ? LinetypeMark::Kind::Shape : LinetypeMark::Kind::Text;
                    mark.rotationKind = (f & 8)   ? LinetypeMark::Rotation::Upright
                                        : (f & 1) ? LinetypeMark::Rotation::Absolute
                                                  : LinetypeMark::Rotation::Relative;
                }
                break;
            }
            case 9:
                if (haveMark)
                    mark.text = dec(g.str());
                break;
            case 75:  // a shape is named by its number in the shape file: "#132"
                if (haveMark && mark.kind == LinetypeMark::Kind::Shape && g.integer() > 0 &&
                    mark.text.empty())
                    mark.text = "#" + std::to_string(g.integer());
                break;
            case 340:
                if (haveMark)
                    mark.style = "\x01" + std::string(g.str());
                break;
            case 46:
                if (haveMark)
                    mark.scale = g.number() * k_;
                break;
            case 50:
                if (haveMark)
                    mark.rotation = g.number() * kPi / 180.0;
                break;
            case 44:
                if (haveMark)
                    mark.shift.x = g.number() * k_;
                break;
            case 45:
                if (haveMark)
                    mark.shift.y = g.number() * k_;
                break;
            default:
                break;
        }
    }
    closeMark();
    if (!(total > 0)) {
        lt.pattern.clear();
        lt.marks.clear();
    }
    for (LinetypeMark& m : lt.marks)
        if (!std::isfinite(m.scale) || !std::isfinite(m.rotation))
            m = LinetypeMark{};
    const bool styled = std::any_of(lt.marks.begin(), lt.marks.end(), [](const LinetypeMark& m) {
        return !m.style.empty() && m.style[0] == '\x01';
    });
    tbcad::Result<LinetypeId> id = tx_->addLinetype(std::move(lt));
    if (!id) {
        report_.warn(code::kRejected, "linetype \"" + name + "\": " + id.error().message, r.line);
        return;
    }
    if (styled)
        marksAwaitingStyles_.push_back(id.value());
    ltypeByRaw_[fold(r.raw(2))] = id.value();
}

namespace {
bool endsWithNoCase(const std::string& s, std::string_view ext) {
    if (s.size() < ext.size())
        return false;
    for (std::size_t i = 0; i < ext.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - ext.size() + i])) != ext[i])
            return false;
    return true;
}
}  // namespace

namespace {
constexpr double kShxStandInWidth = 1.0 / 0.92;
}

void Importer::readStyle(const Record& r) {
    std::string name = dec(r.raw(2));
    if (name.empty())
        return;
    // Shape files (flag bit 0) are not text styles; linetype shapes name them.
    if (r.integer(70) & 1) {
        if (r.has(5))
            shapeFileByHandle_[std::string(r.raw(5))] = dec(r.raw(3));
        return;
    }
    if (r.has(5) && r.num(40) > 0)
        styleHeights_[std::string(r.raw(5))] = r.num(40) * k_;
    if (r.has(5))
        styleNameByHandle_[std::string(r.raw(5))] = name;
    TextStyle ts;
    ts.name = name;
    ts.fontFile = dec(r.raw(3));
    // DWG files often name a shape font without its extension ("romans", "simplex").
    bool bareShx = false;
    if (!ts.fontFile.empty() && ts.fontFile.find('.') == std::string::npos) {
        // "txt" is left out: TBCAD writes it for a style with no font file, and it must read back
        // as none.
        static constexpr std::string_view kBare[] = {
            "romans",   "romanc", "romand", "romant",  "simplex", "complex", "monotxt", "isocp",
            "isocpeur", "iso",    "italic", "italicc", "scripts", "gothice", "greeks"};
        std::string low = ts.fontFile;
        for (char& c : low)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (std::string_view k : kBare)
            bareShx = bareShx || low == k;
        if (bareShx)
            ts.fontFile += ".shx";
    }
    if (endsWithNoCase(ts.fontFile, ".shx")) {
        // The style keeps the shape font's name: the real font is used when the file is found on
        // this machine, else TBCAD's own stroke font stands in.
        std::string shx = ts.fontFile;
        std::string big = dec(r.raw(4));
        ts.shxFile = shx;
        ts.bigFontFile = big;
        ts.fontFile.clear();
        ts.fontFamily = "TBCAD Stroke";
        report_.info(code::kShxFont,
                     "text style \"" + name + "\": SHX font \"" + shx +
                         "\" is drawn with the real font when its file is found, else with the "
                         "TBCAD Stroke font" +
                         (big.empty() ? std::string() : " (big font \"" + big + "\" is not read)"),
                     r.line);
    }
    // AcadAnnotative extended data: "AnnotativeData", { 1070 1 (on) 1070 0 }.
    {
        bool inAnno = false, seenData = false;
        for (const Group& g : r.g) {
            if (g.code == 1001)
                inAnno = g.str() == "AcadAnnotative";
            else if (inAnno && g.code == 1000 && g.str() == "AnnotativeData")
                seenData = true;
            else if (inAnno && seenData && g.code == 1070) {
                ts.annotative = g.integer() == 1;
                break;
            }
        }
    }
    double wf = r.num(41, 1);
    ts.widthFactor = (std::isfinite(wf) && wf > 0) ? wf : 1;
    const double rawWidth = ts.widthFactor;
    // The stroke font that stands in for an SHX font sets narrower than SHX does: a title block
    // in ROMANS measured 0.92 of the SHX width in the owner's plot.
    if (!ts.fontFamily.empty() && ts.fontFamily == "TBCAD Stroke")
        ts.widthFactor *= kShxStandInWidth;
    ts.height = r.num(40, 0);
    if (!std::isfinite(ts.height) || ts.height < 0)
        ts.height = 0;
    ts.oblique = r.num(50, 0) * kPi / 180.0;
    if (!std::isfinite(ts.oblique))
        ts.oblique = 0;
    ts.backward = (r.integer(71, 0) & 2) != 0;
    ts.upsideDown = (r.integer(71, 0) & 4) != 0;
    if (const TextStyle* old = doc_->textStyleByName(name)) {
        TextStyleId id = old->id;
        (void)tx_->modifyTextStyle(id, [&](TextStyle& t) {
            t.fontFile = ts.fontFile;
            t.fontFamily = ts.fontFamily;
            t.widthFactor = ts.widthFactor;
            t.oblique = ts.oblique;
            t.backward = ts.backward;
            t.upsideDown = ts.upsideDown;
            t.height = ts.height;
            t.shxFile = ts.shxFile;
            t.bigFontFile = ts.bigFontFile;
            t.annotative = ts.annotative;
        });
        styleRawWidth_[id.value] = rawWidth;
        return;
    }
    tbcad::Result<TextStyleId> id = tx_->addTextStyle(std::move(ts));
    if (!id)
        report_.warn(code::kRejected, "text style \"" + name + "\": " + id.error().message, r.line);
    else
        styleRawWidth_[id.value().value] = rawWidth;
}

void Importer::readDimStyle(const Record& r) {
    std::string name = dec(r.raw(2));
    if (name.empty())
        return;
    DimStyle ds;
    ds.name = name;
    ds.units = displayUnit_;
    auto positive = [&](int code, double scale, double& out) {
        if (const Group* g = r.find(code)) {
            double v = g->number() * scale;
            if (std::isfinite(v) && v > 0)
                out = v;
        }
    };
    auto nonNegative = [&](int code, double scale, double& out) {
        if (const Group* g = r.find(code)) {
            double v = g->number() * scale;
            if (std::isfinite(v) && v >= 0)
                out = v;
        }
    };
    positive(140, k_, ds.textHeight);    // DIMTXT
    positive(41, k_, ds.arrowSize);      // DIMASZ
    positive(40, 1.0, ds.overallScale);  // DIMSCALE
    // A text style with a fixed height decides the dimension text height: DIMTXT and DIMSCALE
    // do not apply to it. The style keeps the product the dimension generator multiplies.
    // DIMPOST: text around the measurement, "<>" marks where it goes; without "<>" the whole
    // text follows it. Alternate-unit forms ("[...]") are not supported and leave both empty.
    if (r.has(3)) {
        const std::string post = dec(r.raw(3));
        const std::size_t at = post.find("<>");
        if (post.find('[') == std::string::npos) {
            if (at == std::string::npos) {
                ds.suffix = post;
            } else if (post.find("<>", at + 2) == std::string::npos) {
                ds.prefix = post.substr(0, at);
                ds.suffix = post.substr(at + 2);
            }
        }
    }
    nonNegative(45, k_, ds.roundOff);  // DIMRND
    // Tolerances: DIMTOL with equal plus and minus values reads as symmetrical, with different
    // ones as a deviation; DIMLIM shows the two limits.
    nonNegative(47, k_, ds.tolUpper);  // DIMTP
    nonNegative(48, k_, ds.tolLower);  // DIMTM
    if (r.has(72) && r.find(72)->integer() != 0)
        ds.tolerance = DimTolerance::Limits;
    else if (r.has(71) && r.find(71)->integer() != 0)
        ds.tolerance =
            ds.tolUpper == ds.tolLower ? DimTolerance::Symmetrical : DimTolerance::Deviation;
    if (const Group* g = r.find(272)) {  // DIMTDEC
        const std::int64_t p = g->integer();
        if (p >= 0 && p <= 16)
            ds.tolPrecision = static_cast<int>(p);
    }
    // Alternate units: the value is the measurement times DIMALTF, shown as plain millimetres of
    // that number; DIMAPOST wraps it ("[]" marks the number).
    if (r.has(170) && r.find(170)->integer() != 0) {
        ds.altUnits = true;
        ds.altUnit = LengthUnit::Millimeter;
        double f = 25.4;
        if (const Group* g = r.find(143); g && std::isfinite(g->number()) && g->number() > 0)
            f = g->number();
        ds.altScale = f / k_;
        if (const Group* g = r.find(171)) {  // DIMALTD
            const std::int64_t p = g->integer();
            if (p >= 0 && p <= 16)
                ds.altPrecision = static_cast<int>(p);
        }
        if (r.has(4)) {
            const std::string post = dec(r.raw(4));
            const std::size_t at = post.find("[]");
            if (at == std::string::npos) {
                ds.altSuffix = post;
            } else {
                ds.altPrefix = post.substr(0, at);
                ds.altSuffix = post.substr(at + 2);
            }
        }
    }
    // Text orientation: inside (73) or outside (74) text kept horizontal; above the line (77).
    auto flag = [&](int code, bool& out) {
        if (const Group* g = r.find(code))
            out = g->integer() != 0;
    };
    if (r.has(73))
        ds.textAlign =
            r.find(73)->integer() != 0 ? DimTextAlign::Horizontal : DimTextAlign::WithDimLine;
    if (r.has(74))
        ds.textAlignOutside =
            r.find(74)->integer() != 0 ? DimTextAlign::Horizontal : DimTextAlign::WithDimLine;
    flag(75, ds.suppressExt1);            // DIMSE1
    flag(76, ds.suppressExt2);            // DIMSE2
    flag(172, ds.forceLineInside);        // DIMTOFL
    flag(174, ds.textInsideForced);       // DIMTIX
    flag(175, ds.suppressArrowsOutside);  // DIMSOXD
    flag(281, ds.suppressDim1);           // DIMSD1
    flag(282, ds.suppressDim2);           // DIMSD2
    if (const Group* g =
            r.find(280))  // DIMJUST (3 and 4, over the extension line, read as centred)
        ds.textJustify =
            g->integer() == 1 || g->integer() == 2 ? static_cast<int>(g->integer()) : 0;
    if (r.has(77))
        ds.textPlacement = r.find(77)->integer() == 4   ? DimTextPlacement::Below
                           : r.find(77)->integer() != 0 ? DimTextPlacement::AboveLine
                                                        : DimTextPlacement::Centered;
    if (const Group* g = r.find(144); g && std::isfinite(g->number()) && g->number() > 0)
        ds.linearFactor = g->number();  // DIMLFAC
    if (const Group* g = r.find(146); g && std::isfinite(g->number()) && g->number() > 0)
        ds.tolHeightFactor = std::min(g->number(), 10.0);  // DIMTFAC
    if (const Group* g = r.find(279)) {  // DIMTMOVE
        const std::int64_t t = g->integer();
        ds.textMove = t == 1   ? DimTextMove::Leader
                      : t == 2 ? DimTextMove::Free
                               : DimTextMove::WithLine;
    }
    if (const Group* g = r.find(289)) {  // DIMATFIT
        const std::int64_t t = g->integer();
        ds.fit = t == 0   ? DimFit::BothOutside
                 : t == 1 ? DimFit::ArrowsFirst
                 : t == 2 ? DimFit::TextFirst
                          : DimFit::Best;
    }
    if (const Group* g = r.find(78)) {  // DIMZIN
        const std::int64_t z = g->integer();
        ds.suppressLeadingZeros = (z & 4) != 0;
        ds.suppressTrailingZeros = (z & 8) != 0;
    }
    auto dimColor = [&](int code, Color& out) {
        if (const Group* g = r.find(code)) {
            const std::int64_t c = g->integer();
            if (c == 0)
                out = Color::byBlock();
            else if (c == 256)
                out = Color::byLayer();
            else if (c >= 1 && c <= 255)
                out = Color::index(static_cast<std::uint8_t>(c));
        }
    };
    dimColor(176, ds.dimLineColor);
    dimColor(177, ds.extLineColor);
    dimColor(178, ds.textColor);
    if (r.has(340)) {
        const auto fixed = styleHeights_.find(std::string(r.raw(340)));
        if (fixed != styleHeights_.end() && ds.overallScale > 0)
            ds.textHeight = fixed->second / ds.overallScale;
    }
    // DIMBLK: the name of a built-in arrow block (a file that stores a handle there matches none).
    {
        std::string arrow = std::string(r.raw(342));
        if (arrow.empty())
            arrow = std::string(r.raw(343));
        for (char& c : arrow)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (arrow == "_ARCHTICK" || arrow == "_OBLIQUE")
            ds.arrowType = ArrowType::ArchTick;
        else if (arrow == "_DOT" || arrow == "_DOTSMALL" || arrow == "_DOTBLANK")
            ds.arrowType = ArrowType::Dot;
        else if (arrow == "_OPEN30" || arrow == "_OPEN")
            ds.arrowType = ArrowType::Open30;
        else if (arrow == "_NONE")
            ds.arrowType = ArrowType::None;
    }
    for (const Group& x : r.g) {  // the arrow kind TBCAD writes as extended data
        if (x.code != 1000)
            continue;
        const std::string_view v = x.str();
        if (v.rfind("arrow=", 0) == 0) {
            const int a = std::atoi(std::string(v.substr(6)).c_str());
            if (a >= 0 && a <= static_cast<int>(ArrowType::None))
                ds.arrowType = static_cast<ArrowType>(a);
        }
    }
    if (const Group* g = r.find(147); g && std::isfinite(g->number())) {  // DIMGAP
        ds.textGap = std::abs(g->number()) * k_;
        ds.textFrame = g->number() < 0;  // a negative gap draws a box round the text
    }
    if (const Group* g = r.find(69))  // DIMTFILL
        ds.textFill = static_cast<int>(std::clamp<std::int64_t>(g->integer(), 0, 2));
    {
        // DIMTFILLCLR is the second group 70 of the record; the first is the table flags.
        int seen = 0;
        for (const Group& g : r.g) {
            if (g.code != 70 || ++seen < 2)
                continue;
            const std::int64_t c = g.integer();
            ds.textFillColor = c >= 1 && c <= 255 ? static_cast<int>(c) : 0;
            break;
        }
    }
    nonNegative(42, k_, ds.extLineOffset);                              // DIMEXO
    nonNegative(44, k_, ds.extLineExtension);                           // DIMEXE
    nonNegative(46, k_, ds.dimLineExtension);                           // DIMDLE
    nonNegative(43, k_, ds.baselineSpacing);                            // DIMDLI
    if (const Group* g = r.find(141); g && std::isfinite(g->number()))  // DIMCEN
        ds.centerMark = g->number() * k_;
    if (const Group* g = r.find(275)) {  // DIMAUNIT
        const std::int64_t u = g->integer();
        ds.angleFormat = u == 1   ? DimAngleFormat::Dms
                         : u == 2 ? DimAngleFormat::Gradians
                         : u == 3 ? DimAngleFormat::Radians
                                  : DimAngleFormat::Degrees;
    }
    if (const Group* g = r.find(277)) {  // DIMLUNIT: 4 architectural, 5 fractional
        const std::int64_t u = g->integer();
        ds.linearFormat = u == 4   ? DimLinearFormat::Architectural
                          : u == 5 ? DimLinearFormat::Fractional
                                   : DimLinearFormat::Decimal;
    }
    if (const Group* g = r.find(179))  // DIMADEC: -1 means the same as DIMDEC
        ds.angularPrecision = static_cast<int>(std::clamp<std::int64_t>(g->integer(), -1, 8));
    if (const Group* g = r.find(278))  // DIMDSEP: the character code of the decimal mark
        ds.commaDecimal = g->integer() == ',';
    if (const Group* g = r.find(271)) {  // DIMDEC
        ds.precision = static_cast<int>(std::clamp<std::int64_t>(g->integer(), 0, 16));
    }
    if (const DimStyle* old = doc_->dimStyleByName(name)) {
        DimStyleId id = old->id;
        (void)tx_->modifyDimStyle(id, [&](DimStyle& d) {
            DimStyleId keep = d.id;
            std::string keepName = d.name;
            d = ds;
            d.id = keep;
            d.name = keepName;
        });
        return;
    }
    tbcad::Result<DimStyleId> id = tx_->addDimStyle(std::move(ds));
    if (!id)
        report_.warn(code::kRejected, "dimension style \"" + name + "\": " + id.error().message,
                     r.line);
}

void Importer::readLayerRow(const Record& r) {
    LayerRow row;
    row.name = dec(r.raw(2));
    row.line = r.line;
    row.flags = static_cast<int>(r.integer(70));
    row.color = static_cast<int>(r.integer(62, 7));
    if (const Group* g = r.find(420)) {
        std::int64_t v = g->integer();
        row.trueColor = Color::rgb(static_cast<std::uint8_t>((v >> 16) & 0xFF),
                                   static_cast<std::uint8_t>((v >> 8) & 0xFF),
                                   static_cast<std::uint8_t>(v & 0xFF));
    }
    row.linetype = std::string(r.raw(6));
    row.plot = r.integer(290, 1) != 0;
    row.plotStyleHandle = std::string(r.raw(390));
    row.lineweight = static_cast<int>(r.integer(370, -3));
    if (const Group* g = r.find(440)) {
        const int t = transparencyFromGroup(g->integer());
        row.transparency = t > 0 ? t : 0;  // a layer is never ByLayer / ByBlock
    }
    for (std::size_t i = 0; i + 1 < r.g.size(); ++i)
        if (r.g[i].code == 102 && r.g[i].str() == "{ACAD_XDICTIONARY" && r.g[i + 1].code == 360)
            layerXdicts_.emplace_back(row.name, std::string(r.g[i + 1].str()));
    // TBCAD APPID XDATA: "parent=<DXF name of the parent layer>", "name=<own name>", flags.
    bool inOurs = false;
    bool inStandard = false;
    int standardStrings = 0;
    for (const Group& g : r.g) {
        if (g.code == 1001) {
            inOurs = g.str() == "TBCAD";
            inStandard = g.str() == "AcAecLayerStandard";
            standardStrings = 0;
            continue;
        }
        // AutoCAD keeps the layer description as the second string of this application's XDATA.
        if (inStandard && g.code == 1000 && ++standardStrings == 2)
            row.description = dec(g.str());
        if (!inOurs)
            continue;
        if (g.code == 1000) {
            std::string v = dec(g.str());
            if (v.rfind("parent=", 0) == 0)
                row.xParent = v.substr(7);
            else if (v.rfind("name=", 0) == 0)
                row.xName = v.substr(5);
        } else if (g.code == 1070) {
            row.xFlags = static_cast<int>(g.integer());
        }
    }
    if (row.name.empty()) {
        report_.warn(code::kLayerRejected, "layer without a name skipped", r.line);
        return;
    }
    // Entities refer to layers by raw name bytes; remember them next to the decoded name.
    layerRawNames_.push_back(std::string(r.raw(2)));
    row.handle = std::strtoull(std::string(r.raw(5)).c_str(), nullptr, 16);
    layerRows_.push_back(std::move(row));
}

void Importer::applyLayerRow(Layer& l, const LayerRow& row, bool hasParent) {
    int aci = std::abs(row.color);
    if (row.trueColor)
        l.color = *row.trueColor;
    else if (aci >= 1 && aci <= 255)
        l.color = mapAci(aci);
    else
        l.color = Color::index(7);
    l.on = row.color >= 0;
    l.frozen = (row.flags & 1) != 0;
    l.locked = (row.flags & 4) != 0;
    l.plot = row.plot;
    l.transparency = static_cast<std::uint8_t>(row.transparency);
    l.description = row.description;

    l.linetype = kLinetypeContinuous;
    if (!row.linetype.empty()) {
        auto it = ltypeByRaw_.find(fold(row.linetype));
        if (it != ltypeByRaw_.end())
            l.linetype = it->second;
    }
    if (row.lineweight >= 0)
        l.lineweight =
            LineWeight::hundredths(static_cast<std::int16_t>(std::min(row.lineweight, 211)));
    else
        l.lineweight = LineWeight::defaultWeight();

    if (hasParent) {
        if (row.xFlags & 1)
            l.color = Color::byParent();
        if (row.xFlags & 2)
            l.linetype = kLinetypeByParent;
        if (row.xFlags & 4)
            l.lineweight = LineWeight::byParent();
    }
}

void Importer::createLayers() {
    if (layersCreated_)
        return;
    layersCreated_ = true;
    createLayerRows(layerRows_);
    sum_.layers = doc_->layers().size();
    // Layer flag 2: frozen in new viewports (VPLAYER New freeze).
    std::string newFrozen;
    for (const LayerRow& row : layerRows_) {
        if (!(row.flags & 2))
            continue;
        const auto it = layerByFold_.find(fold(row.name));
        if (it != layerByFold_.end())
            newFrozen += (newFrozen.empty() ? "" : ",") + std::to_string(it->second.value);
    }
    if (!newFrozen.empty())
        (void)tx_->modifyHeader([&](Header& h) { h.custom["tbcad.vpnewfrozen"] = newFrozen; });
}

void Importer::createLayerRows(const std::vector<LayerRow>& rows) {
    const std::size_t n = rows.size();
    std::unordered_map<std::string, std::size_t> index;  // folded decoded name -> row
    for (std::size_t i = 0; i < n; ++i)
        index.emplace(fold(rows[i].name), i);

    std::vector<int> state(n, 0);  // 0 new, 1 in progress, 2 done
    std::vector<LayerId> ids(n);

    auto registerName = [&](std::size_t i, LayerId id) {
        layerByFold_[fold(rows[i].name)] = id;
        layerByRaw_[fold(layerRawNames_[i])] = id;
    };

    int depth = 0;  // recursion guard against hostile parent chains
    std::function<LayerId(std::size_t)> make = [&](std::size_t i) -> LayerId {
        if (state[i] == 2)
            return ids[i];
        if (state[i] == 1 || depth >= 64)
            return LayerId{};  // cycle or too deep
        state[i] = 1;
        ++depth;
        const LayerRow& row = rows[i];
        LayerId result{};

        if (fold(row.name) == "0") {
            Layer base = *doc_->layer(kLayerZero);
            applyLayerRow(base, row, false);
            (void)tx_->modifyLayer(kLayerZero, [&](Layer& l) {
                l.color = base.color;
                l.linetype = base.linetype;
                l.lineweight = base.lineweight;
                l.on = base.on;
                l.frozen = base.frozen;
                l.locked = base.locked;
                l.plot = base.plot;
            });
            result = kLayerZero;
        } else {
            LayerId parent{};
            std::string leaf = row.name;
            if (row.xParent) {
                auto it = index.find(fold(*row.xParent));
                if (it != index.end() && it->second != i) {
                    parent = make(it->second);
                }
                if (parent && parent != kLayerZero) {
                    leaf = row.xName.value_or(row.name);
                } else {
                    parent = LayerId{};
                    report_.warn(code::kSubLayerParent,
                                 "layer \"" + row.name + "\" names parent \"" + *row.xParent +
                                     "\" which is not usable; imported as a top-level layer",
                                 row.line);
                }
            } else if (!opts_.splitLayers.empty()) {
                std::vector<std::string> segs = splitOn(row.name, opts_.splitLayers);
                if (segs.size() > 1) {
                    if (segs.size() > static_cast<std::size_t>(kMaxSubLayerDepth)) {
                        std::string tail = segs[kMaxSubLayerDepth - 1];
                        for (std::size_t s = kMaxSubLayerDepth; s < segs.size(); ++s)
                            tail += opts_.splitLayers + segs[s];
                        segs.resize(kMaxSubLayerDepth);
                        segs.back() = tail;
                    }
                    LayerId cur{};
                    bool ok = true;
                    for (std::size_t s = 0; s + 1 < segs.size() && ok; ++s) {
                        std::string full = joinFirst(segs, s + 1, opts_.splitLayers);
                        auto it = index.find(fold(full));
                        LayerId next{};
                        if (it != index.end() && it->second != i) {
                            next = make(it->second);
                        } else {
                            const Layer* existing = doc_->layerByName(cur, segs[s]);
                            if (existing) {
                                next = existing->id;
                            } else {
                                Layer ph;
                                ph.name = segs[s];
                                ph.parent = cur;
                                if (cur) {
                                    ph.color = Color::byParent();
                                    ph.linetype = kLinetypeByParent;
                                    ph.lineweight = LineWeight::byParent();
                                }
                                tbcad::Result<LayerId> made = tx_->addLayer(std::move(ph));
                                if (made)
                                    next = made.value();
                            }
                        }
                        if (!next || next == kLayerZero) {
                            ok = false;
                        } else {
                            cur = next;
                        }
                    }
                    if (ok) {
                        parent = cur;
                        leaf = segs.back();
                    }
                }
            }

            Layer l;
            l.name = leaf;
            l.parent = parent;
            applyLayerRow(l, row, static_cast<bool>(parent));
            tbcad::Result<LayerId> made = tx_->addLayer(l);
            if (!made && parent) {
                // Fall back to a flat layer with the full DXF name.
                Layer flat;
                flat.name = row.name;
                applyLayerRow(flat, row, false);
                made = tx_->addLayer(flat);
                if (made) {
                    report_.warn(
                        code::kLayerRejected,
                        "layer \"" + row.name + "\" could not become a sub-layer; kept flat",
                        row.line);
                }
            }
            if (made) {
                result = made.value();
            } else {
                report_.warn(code::kLayerRejected,
                             "layer \"" + row.name + "\": " + made.error().message +
                                 "; its entities go to layer 0",
                             row.line);
                result = kLayerZero;
            }
        }
        --depth;
        ids[i] = result;
        state[i] = 2;
        registerName(i, result);
        return result;
    };
    for (std::size_t i = 0; i < n; ++i)
        make(i);
}

// ---------------------------------------------------------------------------------------------
// Blocks

BlockId Importer::blockFor(std::string_view dxfName) {
    std::string name = dec(dxfName);
    if (name.empty())
        name = "unnamed";
    std::string f = fold(name);
    auto it = blockByFold_.find(f);
    if (it != blockByFold_.end())
        return it->second;
    BlockDef def;
    def.name = name;
    def.anonymous = name[0] == '*';
    tbcad::Result<BlockId> id = tx_->addBlock(std::move(def));
    if (!id) {
        report_.warn(code::kRejected, "block \"" + name + "\": " + id.error().message);
        return BlockId{};
    }
    blockByFold_[f] = id.value();
    return id.value();
}

std::size_t Importer::sheetFor(const std::string& foldedName) {
    for (std::size_t i = 0; i < sheets_.size(); ++i)
        if (sheets_[i].foldedName == foldedName)
            return i;
    Sheet s;
    s.foldedName = foldedName;
    BlockDef def;
    def.name = "*LAYOUT-" + std::to_string(sheets_.size() + 1);
    def.anonymous = true;
    if (auto id = tx_->addBlock(std::move(def)))
        s.content = id.value();
    sheets_.push_back(std::move(s));
    return sheets_.size() - 1;
}

void Importer::processBlocks(RecordReader& rr) {
    createLayers();
    Record r;
    while (rr.next(r)) {
        if (r.type == "ENDSEC")
            break;
        if (r.type == "SECTION" || r.type == "EOF") {
            report_.warn(code::kMissingSection, "BLOCKS section is not closed by ENDSEC", r.line);
            rr.pushBack(std::move(r));
            break;
        }
        if (r.type == "BLOCK") {
            std::string_view rawName = r.raw(2);
            if (rawName.empty())
                rawName = r.raw(3);
            std::string name = dec(rawName);
            std::string f = fold(name);
            inBlock_ = true;
            curBlock_ = BlockId{};
            skipBlock_ = isLayoutBlock(f);
            curSheet_ = -1;
            if (f.rfind("*paper_space", 0) == 0 || f.rfind("$paper_space", 0) == 0) {
                // A layout's sheet: its objects are read into a hidden block, in paper
                // millimetres (the drawing's unit scale does not apply to the sheet).
                const std::size_t si = sheetFor(f);
                if (sheets_[si].content) {
                    curSheet_ = static_cast<int>(si);
                    curBlock_ = sheets_[si].content;
                    skipBlock_ = true;  // no named block is created for it
                    sheetBlock_ = true;
                }
            }
            // The drawn dimension (*D<n>) is regenerated from the definition points; its parts
            // are kept only when the caller asks for the graphics.
            if (!opts_.dimensionGraphics && f.size() > 2 && f[0] == '*' && f[1] == 'd')
                skipBlock_ = true;
            // Table graphics of ours (*TB<n>) are regenerated from the table kept in extended
            // data. An AutoCAD table's block (*T<n>) is kept, for its TABLE to insert; those left
            // unused are dropped in finish().
            if (f.size() > 3 && f[0] == '*' && f[1] == 't' && f[2] == 'b')
                skipBlock_ = true;
            if (f.size() > 2 && f[0] == '*' && f[1] == 'm')
                skipBlock_ = true;
            if (!skipBlock_) {
                BlockId id = blockFor(rawName);
                if (!id) {
                    skipBlock_ = true;
                } else if (blockDefined_.count(id)) {
                    report_.warn(code::kDuplicateBlock,
                                 "block \"" + name + "\" is defined twice; the second is ignored",
                                 r.line);
                    skipBlock_ = true;
                } else {
                    blockDefined_.insert(id);
                    curBlock_ = id;
                    Vec2 base = P(r, 10, 20);
                    // TBCAD XDATA: the block is an external reference (its content is the
                    // snapshot).
                    std::string xrefPath;
                    std::int64_t xrefStamp = 0;
                    bool xrefOverlay = false;
                    bool inOurs = false;
                    for (const Group& g : r.g) {
                        if (g.code == 1001) {
                            inOurs = g.str() == "TBCAD";
                        } else if (inOurs && g.code == 1000) {
                            std::string v = dec(g.str());
                            if (v.rfind("xref=", 0) == 0)
                                xrefPath = v.substr(5);
                            else if (v.rfind("stamp=", 0) == 0)
                                xrefStamp = std::strtoll(v.c_str() + 6, nullptr, 10);
                        } else if (inOurs && g.code == 1070) {
                            xrefOverlay = g.integer() != 0;
                        }
                    }
                    (void)tx_->modifyBlock(id, [&](BlockDef& b) {
                        b.base = base;
                        b.anonymous = (r.integer(70) & 1) != 0 || name[0] == '*';
                        if (!xrefPath.empty()) {
                            b.xrefPath = xrefPath;
                            b.xrefStamp = xrefStamp;
                            b.xrefOverlay = xrefOverlay;
                        }
                    });
                    ++sum_.blocks;
                    if ((r.integer(70) & 4) && xrefPath.empty()) {
                        // An external reference to another drawing: keep the path so the XREF
                        // commands can load it (the block stays empty until then).
                        const std::string stored = dec(r.raw(1));
                        if (!stored.empty()) {
                            const bool overlay = (r.integer(70) & 8) != 0;
                            (void)tx_->modifyBlock(id, [&](BlockDef& b) {
                                b.xrefPath = stored;
                                b.xrefOverlay = overlay;
                            });
                        }
                        report_.warn(
                            code::kXref,
                            "external reference \"" + name + "\"" +
                                (stored.empty() ? std::string() : " (file " + stored + ")") +
                                " is not loaded; the block is empty until the file is "
                                "found (XREF > Reload)",
                            r.line);
                    }
                }
            }
        } else if (r.type == "ENDBLK") {
            inBlock_ = false;
            skipBlock_ = false;
            sheetBlock_ = false;
            curSheet_ = -1;
            curBlock_ = BlockId{};
        } else if (inBlock_ && sheetBlock_) {
            handleEntity(r, rr, curBlock_);
        } else if (inBlock_ && !skipBlock_) {
            handleEntity(r, rr, curBlock_);
        }
    }
    inBlock_ = false;
    skipBlock_ = false;
    sheetBlock_ = false;
    curSheet_ = -1;
    curBlock_ = BlockId{};
}

void Importer::processEntities(RecordReader& rr) {
    createLayers();
    Record r;
    while (rr.next(r)) {
        if (r.type == "ENDSEC")
            break;
        if (r.type == "SECTION" || r.type == "EOF") {
            report_.warn(code::kMissingSection, "ENTITIES section is not closed by ENDSEC", r.line);
            rr.pushBack(std::move(r));
            break;
        }
        // An object with a named plot style (group 380 = 3) points at the style object.
        if (r.integer(380, 0) == 3 && !r.raw(390).empty() && !r.raw(5).empty())
            entityPlotStyles_.emplace_back(
                std::strtoull(std::string(r.raw(5)).c_str(), nullptr, 16), std::string(r.raw(390)));
        handleEntity(r, rr, BlockId{});
    }
}

// ---------------------------------------------------------------------------------------------

// A VIEWPORT on a layout sheet: where it sits on the paper and which model area it shows.
void Importer::readViewport(const Record& r, BlockId sheetBlock) {
    for (Sheet& sh : sheets_) {
        if (sh.content != sheetBlock)
            continue;
        const double id = r.num(69, 0);
        const double w = r.num(40), h = r.num(41);
        if (id == 1 || !(w > 0) || !(h > 0))
            return;  // the sheet's own overall viewport, or a degenerate one
        SheetViewport v;
        const double cx = r.num(10), cy = r.num(20);
        v.min = {cx - w / 2, cy - h / 2};
        v.max = {cx + w / 2, cy + h / 2};
        // The view centre is in display coordinates relative to the view target.
        v.center = {(r.num(17) + r.num(12)) * k_, (r.num(27) + r.num(22)) * k_};
        const double viewHeight = r.num(45) * k_;
        v.scaleN = viewHeight > 0 ? viewHeight / h : 1;
        v.on = r.integer(68, 1) != 0;
        v.layer = dec(r.raw(8));
        v.id = static_cast<int>(sh.viewports.size()) + 1;
        // Status flags (group 90): 16384 zoom locking, 65536 non-rectangular clipping, 131072 off.
        const long long status = r.integer(90, 0);
        if (status & 65536)
            v.clipHandle = std::string(r.raw(340));
        if (status & 16384)
            v.locked = true;
        if (status & 131072)
            v.on = false;
        // Frozen layers as AutoCAD writes them: one layer record handle per group 331.
        for (const Group& g : r.g) {
            if (g.code != 331)
                continue;
            const std::uint64_t hnd = std::strtoull(std::string(g.str()).c_str(), nullptr, 16);
            for (const LayerRow& row : layerRows_)
                if (hnd != 0 && row.handle == hnd &&
                    std::find(v.frozen.begin(), v.frozen.end(), row.name) == v.frozen.end())
                    v.frozen.push_back(row.name);
        }
        bool ours = false;
        for (const Group& g : r.g) {
            if (g.code == 1001) {
                ours = g.str() == "TBCAD";
            } else if (ours && g.code == 1000) {
                const std::string t = dec(g.str());
                if (t.rfind("frozen=", 0) == 0) {
                    if (std::find(v.frozen.begin(), v.frozen.end(), t.substr(7)) == v.frozen.end())
                        v.frozen.push_back(t.substr(7));
                } else if (t.rfind("vlevel=", 0) == 0) {
                    v.level = t.substr(7);
                } else if (t == "locked=1") {
                    v.locked = true;
                } else if (t == "border=0" || t == "border=1") {
                    v.border = t == "border=1" ? 1 : 0;
                } else if (t.rfind("clip=", 0) == 0) {
                    // x:y pairs joined by ';' (our own files keep the outline here)
                    std::size_t pos = 5;
                    while (pos < t.size()) {
                        std::size_t semi = t.find(';', pos);
                        if (semi == std::string::npos)
                            semi = t.size();
                        const std::string pt = t.substr(pos, semi - pos);
                        const std::size_t colon = pt.find(':');
                        if (colon != std::string::npos)
                            v.clip.push_back({std::strtod(pt.substr(0, colon).c_str(), nullptr),
                                              std::strtod(pt.substr(colon + 1).c_str(), nullptr)});
                        pos = semi + 1;
                    }
                    if (v.clip.size() < 3)
                        v.clip.clear();
                }
            }
        }
        sh.viewports.push_back(v);
        return;
    }
}

void Importer::readLayoutObject(const Record& r) {
    LayoutRow row;
    // The page setup part of a LAYOUT also has a group 1 (the setup name): the layout's own name
    // is the first one after the AcDbLayout marker.
    bool inLayout = false;
    for (const Group& g : r.g) {
        if (g.code == 100 && g.str() == "AcDbLayout")
            inLayout = true;
        else if (inLayout && g.code == 1 && row.name.empty())
            row.name = dec(g.str());
    }
    if (row.name.empty())
        row.name = dec(r.raw(1));
    row.tab = static_cast<int>(r.integer(71, 0));
    row.width = r.num(44);
    row.height = r.num(45);
    row.rotation = static_cast<int>(r.integer(73, 0));
    row.styleSheet = dec(r.raw(7));
    if (r.has(14) && r.has(24) && r.has(15) && r.has(25)) {
        row.hasExtents = true;
        row.extMin = {r.num(14), r.num(24)};
        row.extMax = {r.num(15), r.num(25)};
    }
    if (row.name.empty())
        return;
    sum_.layoutSetups.push_back(plotSetupOf(r, row.name));
    // The page setup (the AcDbPlotSettings part: device, paper, plot style table, scale, origin)
    // is kept as read, to be written back with the layout while the paper size is unchanged.
    {
        std::string packed;
        bool in = false;
        for (const Group& g : r.g) {
            if (g.code == 100) {
                in = g.str() == "AcDbPlotSettings";
                if (in)
                    continue;
            }
            std::string text;
            if (!in || g.code == 102 || handleCode(g.code) ||
                !keptValue(g, [&](std::string_view v) { return dec(v); }, text))
                continue;
            packGroup(packed, g.code, text);
        }
        if (!packed.empty())
            (void)tx_->modifyHeader(
                [&](Header& h) { h.custom[std::string(kKeptPlotSetPrefix) + row.name] = packed; });
    }
    layoutRows_.push_back(std::move(row));
}

// Builds the layout table (Header::custom["tbcad.layouts"], format in libs/layout/layout.h) from
// the sheets read, naming them from the LAYOUT objects in tab order.
void Importer::finishLayouts() {
    // An empty sheet (the default *Paper_Space of a file with no layouts) is not a layout.
    for (std::size_t i = sheets_.size(); i-- > 0;) {
        const BlockDef* b = doc_->block(sheets_[i].content);
        if (sheets_[i].viewports.empty() && (!b || b->entities.empty())) {
            if (sheets_[i].content)
                (void)tx_->eraseBlock(sheets_[i].content);
            sheets_.erase(sheets_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    if (sheets_.empty())
        return;
    // Sheets in creation order: "*Paper_Space", "*Paper_Space0", "*Paper_Space1", ...
    std::vector<std::size_t> order(sheets_.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    auto rank = [&](const Sheet& s) {
        const std::string suffix =
            s.foldedName.substr(std::min<std::size_t>(12, s.foldedName.size()));
        return suffix.empty() ? -1 : std::atoi(suffix.c_str());
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return rank(sheets_[a]) < rank(sheets_[b]);
    });
    std::vector<LayoutRow> rows;
    for (LayoutRow& l : layoutRows_) {
        const std::string f = fold(l.name);
        if (f != "model")
            rows.push_back(l);
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const LayoutRow& a, const LayoutRow& b) { return a.tab < b.tab; });
    auto num = [](double v) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.17g", v);
        return std::string(buf);
    };
    std::string text, setups;
    std::unordered_set<std::string> used;
    int uid = 0;
    for (std::size_t n = 0; n < order.size(); ++n) {
        Sheet& sh = sheets_[order[n]];
        std::string name = n < rows.size() ? rows[n].name : "Layout" + std::to_string(n + 1);
        if (name.size() > 64)
            name.resize(64);
        for (char& c : name)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == '\t')
                c = ' ';
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        while (!name.empty() && name.front() == ' ')
            name.erase(name.begin());
        if (name.empty())
            name = "Layout" + std::to_string(n + 1);
        while (!used.insert(fold(name)).second)
            name += "_";
        double w = n < rows.size() ? rows[n].width : 0, h = n < rows.size() ? rows[n].height : 0;
        if (n < rows.size() && (rows[n].rotation == 1 || rows[n].rotation == 3))
            std::swap(w, h);
        if (!(w > 0) || !(h > 0)) {
            w = 420;
            h = 297;
            for (const SheetViewport& v : sh.viewports) {
                while (v.max.x > w - 5)
                    w *= 1.5;
                while (v.max.y > h - 5)
                    h *= 1.5;
            }
        }
        for (SheetViewport& v : sh.viewports) {
            // AutoCAD keeps the outline as a separate closed polyline the viewport points to.
            if (v.clip.empty() && !v.clipHandle.empty()) {
                const auto ring = sheetRings_.find(v.clipHandle);
                if (ring != sheetRings_.end() && ring->second.size() >= 3)
                    v.clip = ring->second;
            }
        }
        // Paper space coordinates are absolute: the sheet's paper rectangle can sit anywhere. Move
        // the content so the paper's lower left corner is the origin of the sheet.
        if (n < rows.size() && rows[n].hasExtents &&
            std::abs(rows[n].extMax.x - rows[n].extMin.x - w) < 1.0 &&
            std::abs(rows[n].extMax.y - rows[n].extMin.y - h) < 1.0 &&
            (std::abs(rows[n].extMin.x) > 1e-6 || std::abs(rows[n].extMin.y) > 1e-6)) {
            const Vec2 shift{-rows[n].extMin.x, -rows[n].extMin.y};
            if (const BlockDef* b = doc_->block(sh.content)) {
                const std::vector<EntityId> ids(b->entities.begin(), b->entities.end());
                const Transform2 move = Transform2::translate(shift);
                for (EntityId id : ids)
                    (void)tx_->modify(id, [&](Entity& e) {
                        tbcad::Result<Entity> moved = geom::transformed(e, move);
                        if (moved)
                            e = std::move(moved.value());
                    });
            }
            for (SheetViewport& v : sh.viewports) {
                v.min = v.min + shift;
                v.max = v.max + shift;
                for (Vec2& p : v.clip)
                    p = p + shift;
            }
        }
        ++uid;
        // The plot style table the layout names becomes a page setup of the same name, so PLOT
        // finds the table (the tables themselves are files the user keeps beside the program).
        std::string setupName;
        if (n < rows.size()) {
            std::string sheet = rows[n].styleSheet;
            const std::size_t slash = sheet.find_last_of("/\\");
            if (slash != std::string::npos)
                sheet.erase(0, slash + 1);
            if (!sheet.empty() && fold(sheet) != "none" &&
                sheet.find_first_of("\t\n=") == std::string::npos) {
                setupName = name;
                setups += "S\t" + setupName + "\tstyle=" + sheet + "\n";
            }
        }
        text += "L\t" + std::to_string(uid) + "\t" + name + "\tCustom\t" + (w > h ? "1" : "0") +
                "\t" + num(w) + "\t" + num(h) + "\t" + std::to_string(sh.content.value) + "\t" +
                setupName + "\n";
        for (const SheetViewport& v : sh.viewports) {
            // The sheet's own overall viewport covers the whole paper: not a window on the model.
            if (v.max.x - v.min.x >= 0.95 * w && v.max.y - v.min.y >= 0.95 * h)
                continue;
            // The border is the viewport object itself: it prints with its layer, so a layer that
            // is off, frozen or set not to plot (Defpoints) leaves the sheet without a frame.
            bool border = true;
            for (const LayerRow& row : layerRows_)
                if (fold(row.name) == fold(v.layer))
                    border = row.plot && !(row.flags & 1) && row.color >= 0;
            if (v.border >= 0)
                border = v.border == 1;
            std::string clipText;
            for (std::size_t i = 0; i < v.clip.size(); ++i)
                clipText += (i ? ";" : "") + num(v.clip[i].x) + ":" + num(v.clip[i].y);
            std::string levelId = "0";
            if (!v.level.empty())
                if (auto lt = levelByFold_.find(fold(v.level)); lt != levelByFold_.end())
                    levelId = std::to_string(lt->second.value);
            std::string frozenIds;
            for (const std::string& ln : v.frozen) {
                const auto it = layerByFold_.find(fold(ln));
                if (it != layerByFold_.end())
                    frozenIds += (frozenIds.empty() ? "" : ",") + std::to_string(it->second.value);
            }
            text += "V\t" + std::to_string(v.id) + "\t" + num(v.min.x) + "\t" + num(v.min.y) +
                    "\t" + num(v.max.x) + "\t" + num(v.max.y) + "\t" + num(v.center.x) + "\t" +
                    num(v.center.y) + "\t" + num(v.scaleN) + "\t" + (v.on ? "1" : "0") + "\t" +
                    frozenIds + "\t" + levelId + "\t" + (v.locked ? "1" : "0") + "\t" + clipText +
                    "\t" + (border ? "1" : "0") + "\n";
        }
    }
    (void)tx_->modifyHeader([&](Header& h) {
        h.custom["tbcad.layouts"] = text;
        if (!setups.empty())
            h.custom["tbcad.pagesetups"] = setups;
    });
    sum_.layouts = sheets_.size();
}

void Importer::finish() {
    (void)tx_->modifyHeader([&](Header& h) { h.units = displayUnit_; });
    finishLayouts();
    resolveImages();
    // Anonymous table blocks that no TABLE inserted (erasing fails while an INSERT uses one).
    for (const auto& [f, id] : blockByFold_)
        if (f.size() > 2 && f[0] == '*' && f[1] == 't' && blockDefined_.count(id))
            (void)tx_->eraseBlock(id);
    for (const auto& [f, id] : blockByFold_) {
        if (!blockDefined_.count(id)) {
            const BlockDef* b = doc_->block(id);
            report_.warn(code::kMissingBlock,
                         "block \"" + (b ? b->name : f) + "\" is inserted but never defined");
        }
    }
    for (const auto& [type, info] : skippedTypes_) {
        report_.warn(code::kSkippedEntity,
                     std::to_string(info.first) + " " + type + " entit" +
                         (info.first == 1 ? "y" : "ies") + " skipped (not supported)",
                     info.second);
    }
    if (paperSpace_ > 0) {
        report_.warn(code::kPaperSpace,
                     std::to_string(paperSpace_) + " paper space entities skipped");
    }
    if (projected_ > 0) {
        report_.info(code::kExtrusion3d,
                     std::to_string(projected_) +
                         " circles, arcs, ellipses or polylines lie in tilted 3D planes; they are "
                         "drawn as seen from above",
                     firstProjectedLine_);
    }
    if (acadTables_ > 0) {
        report_.info(code::kTableCells,
                     std::to_string(acadTables_) +
                         " AutoCAD table(s) read as editable tables; cell borders, colours and "
                         "fills follow the TBCAD table style");
    }
    if (flattened_ > 0) {
        report_.info(code::kFlattened,
                     std::to_string(flattened_) + " entities had a non-zero Z; Z was dropped",
                     firstFlattenedLine_);
    }
    if (mirroredText_ > 0) {
        report_.info(code::kMirroredText,
                     std::to_string(mirroredText_) +
                         " text entities use a -Z extrusion; they are placed correctly but drawn "
                         "upright, not mirrored",
                     firstMirroredLine_);
    }
    if (!mtextDropped_.empty()) {
        std::string msg = "MTEXT formatting codes dropped:";
        for (const auto& [letter, count] : mtextDropped_) {
            msg += " \\";
            msg.push_back(letter);
            msg += "x" + std::to_string(count);
        }
        report_.info(code::kMTextMarkup, msg);
    }
}

Result<ImportResult> Importer::run() {
    HeaderHints hints;
    if (groups_) {
        // (a scratch report: the main pass reports the problems of the groups once)
        Report scratch;
        Lexer headerLexer = Lexer::fromBuffer(*groups_, scratch);
        hints = scanHeaderHints(headerLexer);
    } else {
        hints = scanHeaderHints(bytes_);
    }
    acadVer_ = hints.acadVer;
    int vnum = versionNumber(acadVer_);
    if (vnum != 0 && vnum < 1009) {
        return makeError(ErrorCode::Unsupported, "unsupported DXF version " + acadVer_ + " (" +
                                                     versionName(acadVer_) +
                                                     "); TBCAD reads AC1009 (R12) and later");
    }
    sum_.acadVer = acadVer_;
    sum_.version = versionName(acadVer_);

    std::optional<Lexer> lexerHolder;
    if (groups_) {
        lexerHolder.emplace(Lexer::fromBuffer(*groups_, report_));
    } else {
        Result<Lexer> opened = Lexer::open(bytes_, report_);
        if (!opened)
            return opened.error();
        lexerHolder.emplace(std::move(opened.value()));
    }
    Lexer& lexer = *lexerHolder;
    sum_.binary = lexer.isBinary();

    std::size_t guessesBefore = report_.countCode(code::kEncodingGuess);
    if (opts_.encoding) {
        enc_ = *opts_.encoding;
    } else {
        // (all the non-ASCII bytes of a buffer are in its text values)
        const std::span<const std::byte> seen =
            groups_ ? std::span<const std::byte>(
                          reinterpret_cast<const std::byte*>(groups_->arena().data()),
                          groups_->arena().size())
                    : bytes_;
        enc_ = detectEncoding(seen, hints.acadVer, hints.dwgCodepage, report_);
    }
    sum_.encoding = enc_;
    sum_.encodingGuessed = report_.countCode(code::kEncodingGuess) > guessesBefore;

    doc_ = std::make_unique<Document>();
    tx_.emplace(doc_->begin("import dxf"));
    layerByFold_["0"] = kLayerZero;
    layerByRaw_["0"] = kLayerZero;

    RecordReader rr(lexer);
    // A big file is split into records on a second thread while this one builds the drawing (a
    // small one is not worth the thread; asking for threads in the options forces it).
    constexpr std::size_t kReadAheadBytes = std::size_t{1} << 20;
    const bool readAhead =
        !groups_ && (opts_.maxThreads ? opts_.maxThreads >= 2
                                      : bytes_.size() >= kReadAheadBytes &&
                                            std::thread::hardware_concurrency() >= 2);
    if (readAhead)
        rr.readAhead(report_);
    Record rec;
    bool sawSection = false;
    while (rr.next(rec)) {
        if (rec.type == "EOF")
            break;
        if (rec.type != "SECTION")
            continue;  // stray record between sections
        sawSection = true;
        std::string name = dec(rec.raw(2));
        if (name == "HEADER") {
            parseHeader(rec);
            continue;
        }
        if (sum_.unitsName.empty())
            setUnits(0, true);
        if (name == "TABLES")
            processTables(rr);
        else if (name == "BLOCKS")
            processBlocks(rr);
        else if (name == "ENTITIES")
            processEntities(rr);
        else if (name == "OBJECTS")
            processObjects(rr);
        else if (name == "CLASSES")
            processClasses(rr);
        else
            skipSection(rr);
    }
    if (sum_.unitsName.empty())
        setUnits(0, true);
    if (!sawSection) {
        return makeError(ErrorCode::Parse, "no SECTION found: this does not look like a DXF file");
    }
    createLayers();
    applyLayerAlphaXrecords();
    resolveExtDictionaries();
    applyPlotStyles();
    keepHandles();
    importGroups();
    importDrawOrder();
    importMlineStyles();
    keepObjects();
    finish();

    // Current layer from the header, when it exists.
    {
        auto it = doc_->header().custom.find("dxf.clayer");
        if (it != doc_->header().custom.end()) {
            auto lit = layerByFold_.find(fold(it->second));
            if (lit != layerByFold_.end()) {
                LayerId id = lit->second;
                (void)tx_->modifyHeader([&](Header& h) { h.currentLayer = id; });
            }
        }
    }

    ChangeSet cs = tx_->commit();
    tx_.reset();

    // Dimensions draw themselves: put the text where our generator puts it (the position the
    // other program stored is only a hint) so that re-exporting gives the same file again. The
    // measurement the file stored is kept (or derived when it stored none).
    {
        std::vector<EntityId> dims;
        doc_->forEachEntity([&](const Entity& e) {
            if (entityAs<DimensionData>(e))
                dims.push_back(e.common.id);
        });
        if (!dims.empty()) {
            Transaction tx = doc_->begin("dimension text");
            for (EntityId id : dims) {
                (void)tx.modify(id, [&](Entity& e) {
                    DimensionData& d = *entityAs<DimensionData>(e);
                    const double measured = d.measurement;
                    dim::refresh(*doc_, d);
                    if (measured > 0)
                        d.measurement = measured;
                });
            }
            tx.commit();
        }
    }
    // Associative arrays are written as one INSERT per item with a TBCAD_ARRAY note; fuse the
    // items of each array back into a single array insert.
    {
        struct Item {
            long long index = 0;
            EntityId id{};
        };
        struct Group {
            long long count = 0;
            std::vector<Item> items;
            std::string info;
            Vec2 dpos{};
            double drot = 0;
            bool hasInfo = false;
        };
        std::map<long long, Group> groups;
        doc_->forEachEntity([&](const Entity& e) {
            if (!entityAs<InsertData>(e))
                return;
            for (const XdataApp& app : e.common.ext.get().xdata) {
                if (app.app != "TBCAD_ARRAY")
                    continue;
                long long id = -1, n = 0, i = -1;
                std::string info;
                Vec2 dpos{};
                double drot = 0;
                bool hasInfo = false;
                for (const DataItem& it : app.items) {
                    if (it.code != 1000)
                        continue;
                    if (it.text.rfind("a=", 0) == 0) {
                        (void)std::sscanf(it.text.c_str(), "a=%lld;n=%lld;i=%lld", &id, &n, &i);
                    } else if (it.text.rfind("dpos=", 0) == 0) {
                        (void)std::sscanf(it.text.c_str(), "dpos=%lf,%lf;drot=%lf", &dpos.x,
                                          &dpos.y, &drot);
                    } else if (it.text.rfind("info=", 0) == 0) {
                        info = it.text.substr(5);
                        hasInfo = true;
                    }
                }
                if (id < 0 || n < 1 || i < 0)
                    continue;
                Group& g = groups[id];
                g.count = n;
                g.items.push_back({i, e.common.id});
                if (hasInfo) {
                    g.info = std::move(info);
                    g.dpos = dpos * k_;
                    g.drot = drot;
                    g.hasInfo = true;
                }
            }
        });
        if (!groups.empty()) {
            Transaction tx = doc_->begin("arrays");
            for (auto& [gid, g] : groups) {
                std::sort(g.items.begin(), g.items.end(),
                          [](const Item& a, const Item& b) { return a.index < b.index; });
                bool whole = g.hasInfo && static_cast<long long>(g.items.size()) == g.count;
                for (std::size_t k = 0; whole && k < g.items.size(); ++k)
                    whole = g.items[k].index == static_cast<long long>(k);
                auto info = whole ? blocks::arrayFromText(g.info, k_) : std::nullopt;
                if (info && blocks::arrayItemCount(*info) != g.count)
                    info.reset();
                const Entity* first = info ? doc_->find(g.items.front().id) : nullptr;
                if (first && doc_->ownerBlock(first->common.id))
                    first = nullptr;  // items inside a block definition stay plain
                if (!first) {
                    // Not a whole array: the items stay plain inserts, minus the note.
                    for (const Item& it : g.items)
                        (void)tx.modify(it.id, [](Entity& e) {
                            ExtData x = e.common.ext.get();
                            std::erase_if(x.xdata,
                                          [](const XdataApp& a) { return a.app == "TBCAD_ARRAY"; });
                            e.common.ext = ExtDataPtr(std::move(x));
                        });
                    continue;
                }
                Entity merged = *first;
                merged.common.id = {};
                {
                    ExtData x = merged.common.ext.get();
                    std::erase_if(x.xdata,
                                  [](const XdataApp& a) { return a.app == "TBCAD_ARRAY"; });
                    merged.common.ext = ExtDataPtr(std::move(x));
                }
                auto* ins = entityAs<InsertData>(merged);
                ins->position = ins->position + g.dpos;
                ins->rotation += g.drot;
                ins->array = *info;
                for (const Item& it : g.items)
                    (void)tx.erase(it.id);
                (void)tx.add(std::move(merged));
            }
            tx.commit();
        }
    }
    sum_.outsideWorkArea = cs.outOfArea.size();
    if (sum_.outsideWorkArea > 0) {
        report_.warn(code::kOutsideWorkArea,
                     std::to_string(sum_.outsideWorkArea) +
                         " entities lie beyond the working area (kept, flagged by the document)");
    }
    sum_.layers = doc_->layers().size();

    ImportResult result;
    result.document = std::move(doc_);
    result.summary = std::move(sum_);
    return result;
}

}  // namespace tbcad::dxf::detail

namespace tbcad::dxf {

Result<ImportResult> importDxf(std::span<const std::byte> bytes, const ImportOptions& options,
                               Report& report) {
    detail::Importer importer(bytes, options, report);
    return importer.run();
}

Result<ImportResult> importDxfGroups(const GroupBuffer& groups, const ImportOptions& options,
                                     Report& report) {
    detail::Importer importer(groups, options, report);
    return importer.run();
}

}  // namespace tbcad::dxf
