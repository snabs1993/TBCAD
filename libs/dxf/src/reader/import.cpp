// DXF import: file structure, header, tables, layers and blocks.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>

#include "importer.h"
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
    bool haveUnits = false;
    double ltscale = 1;
    std::string clayer;
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
        } else if (var == "$LTSCALE" && g.code == 40) {
            ltscale = g.number();
        } else if (var == "$CLAYER" && g.code == 8) {
            clayer = std::string(g.str());
        }
    }
    setUnits(insunits, !haveUnits);
    (void)tx_->modifyHeader([&](Header& h) {
        if (std::isfinite(ltscale) && ltscale > 0)
            h.ltScale = ltscale;
        h.createdBy = "tbcad-convert";
        h.custom["dxf.acadver"] = acadVer_;
        if (!clayer.empty())
            h.custom["dxf.clayer"] = dec(clayer);
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
        if (r.type == "LAYER")
            readLayerRow(r);
        else if (r.type == "LTYPE")
            readLinetype(r);
        else if (r.type == "STYLE")
            readStyle(r);
        else if (r.type == "DIMSTYLE")
            readDimStyle(r);
    }
    createLayers();
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
    bool complex = false;
    double total = 0;
    for (const Group& g : r.g) {
        if (g.code == 49) {
            double v = g.number() * k_;
            if (std::isfinite(v)) {
                lt.pattern.push_back(v);
                total += std::abs(v);
            }
        } else if (g.code == 74 && g.integer() != 0) {
            complex = true;
        }
    }
    if (!(total > 0))
        lt.pattern.clear();
    if (complex) {
        report_.info(code::kComplexLinetype,
                     "linetype \"" + name + "\" has text or shape elements; only dashes are kept",
                     r.line);
    }
    tbcad::Result<LinetypeId> id = tx_->addLinetype(std::move(lt));
    if (!id) {
        report_.warn(code::kRejected, "linetype \"" + name + "\": " + id.error().message, r.line);
        return;
    }
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

void Importer::readStyle(const Record& r) {
    std::string name = dec(r.raw(2));
    if (name.empty())
        return;
    // Shape files (flag bit 0) are not text styles.
    if (r.integer(70) & 1)
        return;
    TextStyle ts;
    ts.name = name;
    ts.fontFile = dec(r.raw(3));
    if (endsWithNoCase(ts.fontFile, ".shx")) {
        // AutoCAD compiled shape fonts are not read; the style gets TBCAD's own stroke font.
        std::string shx = ts.fontFile;
        std::string big = dec(r.raw(4));
        ts.fontFile.clear();
        ts.fontFamily = "TBCAD Stroke";
        report_.info(code::kShxFont,
                     "text style \"" + name + "\": SHX font \"" + shx +
                         "\" is replaced by the TBCAD Stroke font" +
                         (big.empty() ? std::string() : " (big font \"" + big + "\" dropped)"),
                     r.line);
    }
    double wf = r.num(41, 1);
    ts.widthFactor = (std::isfinite(wf) && wf > 0) ? wf : 1;
    ts.oblique = r.num(50, 0) * kPi / 180.0;
    if (!std::isfinite(ts.oblique))
        ts.oblique = 0;
    if (const TextStyle* old = doc_->textStyleByName(name)) {
        TextStyleId id = old->id;
        (void)tx_->modifyTextStyle(id, [&](TextStyle& t) {
            t.fontFile = ts.fontFile;
            t.fontFamily = ts.fontFamily;
            t.widthFactor = ts.widthFactor;
            t.oblique = ts.oblique;
        });
        return;
    }
    tbcad::Result<TextStyleId> id = tx_->addTextStyle(std::move(ts));
    if (!id)
        report_.warn(code::kRejected, "text style \"" + name + "\": " + id.error().message, r.line);
}

void Importer::readDimStyle(const Record& r) {
    std::string name = dec(r.raw(2));
    if (name.empty())
        return;
    DimStyle ds;
    ds.name = name;
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
    positive(140, k_, ds.textHeight);          // DIMTXT
    positive(41, k_, ds.arrowSize);            // DIMASZ
    positive(40, 1.0, ds.overallScale);        // DIMSCALE
    nonNegative(147, k_, ds.textGap);          // DIMGAP
    nonNegative(42, k_, ds.extLineOffset);     // DIMEXO
    nonNegative(44, k_, ds.extLineExtension);  // DIMEXE
    nonNegative(46, k_, ds.dimLineExtension);  // DIMDLE
    if (const Group* g = r.find(271)) {        // DIMDEC
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
    row.lineweight = static_cast<int>(r.integer(370, -3));
    // TBCAD APPID XDATA: "parent=<DXF name of the parent layer>", "name=<own name>", flags.
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
            // The drawn dimension (*D<n>) is regenerated from the definition points; its parts
            // are kept only when the caller asks for the graphics.
            if (!opts_.dimensionGraphics && f.size() > 2 && f[0] == '*' && f[1] == 'd')
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
                    (void)tx_->modifyBlock(id, [&](BlockDef& b) {
                        b.base = base;
                        b.anonymous = (r.integer(70) & 1) != 0 || name[0] == '*';
                    });
                    ++sum_.blocks;
                    if (r.integer(70) & 4) {
                        report_.warn(code::kXref,
                                     "external reference block \"" + name +
                                         "\" is not supported; the block is empty",
                                     r.line);
                    }
                }
            }
        } else if (r.type == "ENDBLK") {
            inBlock_ = false;
            skipBlock_ = false;
            curBlock_ = BlockId{};
        } else if (inBlock_ && !skipBlock_) {
            handleEntity(r, rr, curBlock_);
        }
    }
    inBlock_ = false;
    skipBlock_ = false;
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
        handleEntity(r, rr, BlockId{});
    }
}

// ---------------------------------------------------------------------------------------------

void Importer::finish() {
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
                     std::to_string(paperSpace_) +
                         " paper space entities skipped (layouts are not imported)");
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
    HeaderHints hints = scanHeaderHints(bytes_);
    acadVer_ = hints.acadVer;
    int vnum = versionNumber(acadVer_);
    if (vnum != 0 && vnum < 1009) {
        return makeError(ErrorCode::Unsupported, "unsupported DXF version " + acadVer_ + " (" +
                                                     versionName(acadVer_) +
                                                     "); TBCAD reads AC1009 (R12) and later");
    }
    sum_.acadVer = acadVer_;
    sum_.version = versionName(acadVer_);

    Result<Lexer> opened = Lexer::open(bytes_, report_);
    if (!opened)
        return opened.error();
    Lexer lexer = std::move(opened.value());
    sum_.binary = lexer.isBinary();

    std::size_t guessesBefore = report_.countCode(code::kEncodingGuess);
    if (opts_.encoding) {
        enc_ = *opts_.encoding;
    } else {
        enc_ = detectEncoding(bytes_, hints.acadVer, hints.dwgCodepage, report_);
    }
    sum_.encoding = enc_;
    sum_.encodingGuessed = report_.countCode(code::kEncodingGuess) > guessesBefore;

    doc_ = std::make_unique<Document>();
    tx_.emplace(doc_->begin("import dxf"));
    layerByFold_["0"] = kLayerZero;
    layerByRaw_["0"] = kLayerZero;

    RecordReader rr(lexer);
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
        else
            skipSection(rr);
    }
    if (sum_.unitsName.empty())
        setUnits(0, true);
    if (!sawSection) {
        return makeError(ErrorCode::Parse, "no SECTION found: this does not look like a DXF file");
    }
    createLayers();
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

}  // namespace tbcad::dxf
