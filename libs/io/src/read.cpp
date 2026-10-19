#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_set>

#include "chunks.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/platform/paths.h"

namespace tbcad::io {

using namespace detail;

namespace {

// Ids are kept exactly as stored. The Document hands out ids itself, so a gap in the stored ids
// (rows that were deleted before saving) is reproduced by adding and erasing placeholders. A
// bigger gap than this is treated as a corrupt file rather than as a reason to spin.
constexpr std::uint64_t kMaxIdGap = std::uint64_t{1} << 24;

[[noreturn]] void bad(const std::string& msg) {
    throw FormatError(msg);
}

template <class T>
T take(Result<T>&& r, const char* what) {
    if (!r)
        bad(std::string(what) + ": " + r.error().message);
    return std::move(r).value();
}
void check(const Result<void>& r, const std::string& what) {
    if (!r)
        bad(what + ": " + r.error().message);
}

std::uint32_t id32(ByteReader& r) {
    std::uint64_t v = r.varint();
    if (v == 0 || v > std::numeric_limits<std::uint32_t>::max())
        bad("table row id out of range");
    return static_cast<std::uint32_t>(v);
}

Color readColor(ByteReader& r) {
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
E readEnum(ByteReader& r, std::uint8_t maxValue) {
    std::uint8_t v = r.u8();
    if (v > maxValue)
        bad("enumeration value out of range");
    return static_cast<E>(v);
}

// Reads one chunk's payload, tracking the inflated copy.
struct Payload {
    std::vector<std::uint8_t> scratch{};
    std::span<const std::uint8_t> data{};
    explicit Payload(const RawChunk& c) {
        if (c.flags & ~kChunkDeflate)
            bad("unsupported chunk flags in " + fourccName(c.type));
        data = chunkPayload(c, scratch);
    }
};

class Loader {
public:
    Loader(const ParsedFile& f, IoReport& report, bool salvage = false)
        : file_(f), report_(report), salvage_(salvage) {}

    std::unique_ptr<Document> run() {
        classify();
        loadStrings();
        Header header = loadHead();
        doc_ = std::make_unique<Document>();
        Transaction tx = doc_->begin("load");
        step("linetypes", [&] { loadLinetypes(tx); });
        step("linetype marks", [&] { loadLinetypeMarks(tx); });
        step("text styles", [&] { loadTextStyles(tx); });
        step("text style extras", [&] { loadTextStyleExtras(tx); });
        step("text heights", [&] { loadTextHeights(tx); });
        step("dimension styles", [&] { loadDimStyles(tx); });
        step("dimension style extras", [&] { loadDimStyleExtras(tx); });
        step("layers", [&] { loadLayers(tx); });
        step("levels", [&] { loadLevels(tx); });
        step("entity levels", [&] { loadEntityLevels(); });
        step("blocks", [&] { loadBlocks(tx); });
        step("external references", [&] { loadXrefs(tx); });
        step("xref overlays", [&] { loadXrefOverlays(tx); });
        step("dynamic blocks", [&] { loadDynamic(tx); });
        step("assemblies", [&] { loadAssemblies(tx); });
        check(tx.modifyHeader([&](Header& h) { h = header; }), "header");
        step("entities", [&] { loadEntities(tx); });
        step("draw order", [&] { loadOrder(tx); });
        tx.commit();
        check(doc_->reserveIds(nextIds_), "next ids");
        return std::move(doc_);
    }

private:
    // In salvage mode a table that does not parse is skipped (with a warning) instead of failing
    // the whole file.
    template <class Fn>
    void step(const char* what, Fn&& fn) {
        if (!salvage_) {
            fn();
            return;
        }
        try {
            fn();
        } catch (const FormatError& e) {
            report_.warnings.push_back(std::string("damaged ") + what +
                                       " could not be read in full (" + e.what() + ")");
        }
    }

    void classify() {
        for (const RawChunk& c : file_.chunks) {
            switch (c.type) {
                case kHead:
                case kStrs:
                case kLtyp:
                case kTsty:
                case kTsth:
                case kDstx:
                case kLtmk:
                case kTstx:
                case kDsty:
                case kLayr:
                case kLvls:
                case kElvl:
                case kBlks:
                case kXrfs:
                case kXrfo:
                case kDynb:
                case kAsmb:
                case kOrdr:
                    if (std::exchange(seen_[c.type], true)) {
                        if (!salvage_)
                            bad("duplicate chunk " + fourccName(c.type));
                        report_.warnings.push_back("ignored a repeated " + fourccName(c.type) +
                                                   " chunk");
                        break;
                    }
                    single_[c.type] = &c;
                    break;
                case kEnts:
                    ents_.push_back(&c);
                    break;
                default:
                    unknown_.push_back(&c);
            }
        }
        if (!seen_[kHead] || !seen_[kStrs])
            bad("required chunk missing (HEAD or STRS)");
    }

    const RawChunk* find(std::uint32_t type) const {
        auto it = single_.find(type);
        return it == single_.end() ? nullptr : it->second;
    }

    void loadStrings() {
        Payload p(*find(kStrs));
        ByteReader r(p.data);
        std::size_t n = r.count(1);
        strings_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t len = r.count(1);
            auto b = r.bytes(len);
            std::string s(reinterpret_cast<const char*>(b.data()), b.size());
            if (!validUtf8(s))
                bad("string table holds invalid UTF-8");
            strings_.push_back(std::move(s));
        }
        if (!r.atEnd())
            bad("trailing data in STRS");
        if (strings_.empty() || !strings_[0].empty())
            bad("string table must start with the empty string");
    }

    const std::string& str(ByteReader& r) {
        std::uint64_t i = r.varint();
        if (i >= strings_.size())
            bad("string index out of range");
        return strings_[static_cast<std::size_t>(i)];
    }

    NextIds nextIds_{};
    std::unordered_map<std::uint64_t, LevelId> entityLevel_{};

    Header loadHead() {
        Header h;
        Payload p(*find(kHead));
        ByteReader r(p.data);
        std::size_t n = r.count(3);
        for (std::size_t i = 0; i < n; ++i) {
            const std::string key = str(r);
            std::uint8_t type = r.u8();
            if (type > 2)
                bad("unknown HEAD value type");
            std::string sval;
            std::int64_t ival = 0;
            double fval = 0;
            if (type == 0)
                sval = str(r);
            else if (type == 1)
                ival = r.i64();
            else
                fval = r.f64();
            auto wantInt = [&] {
                if (type != 1)
                    bad("HEAD key " + key + " has the wrong type");
            };
            auto wantReal = [&] {
                if (type != 2)
                    bad("HEAD key " + key + " has the wrong type");
            };
            auto id = [&](auto& dst) {
                wantInt();
                if (ival < 0 || ival > std::numeric_limits<std::uint32_t>::max())
                    bad("HEAD id out of range");
                dst.value = static_cast<std::uint32_t>(ival);
            };
            if (key == "units") {
                wantInt();
                if (ival < 0 || ival > static_cast<std::int64_t>(LengthUnit::Foot))
                    bad("invalid drawing units");
                h.units = static_cast<LengthUnit>(ival);
            } else if (key == "workArea") {
                wantReal();
                h.workArea = fval;
            } else if (key == "ltScale") {
                wantReal();
                h.ltScale = fval;
            } else if (key == "currentLayer") {
                id(h.currentLayer);
            } else if (key == "currentColor") {
                wantInt();
                if (ival < 0 || (ival >> 24) > static_cast<std::int64_t>(ColorKind::ByParent) ||
                    ival >= (std::int64_t{1} << 32))
                    bad("invalid current colour");
                h.currentColor.kind = static_cast<ColorKind>(ival >> 24);
                h.currentColor.r = static_cast<std::uint8_t>(ival >> 16);
                h.currentColor.g = static_cast<std::uint8_t>(ival >> 8);
                h.currentColor.b = static_cast<std::uint8_t>(ival);
            } else if (key == "currentLevel") {
                id(h.currentLevel);
            } else if (key == "levelView") {
                wantInt();
                if (ival < 0 || ival > static_cast<std::int64_t>(LevelView::ShowAll))
                    bad("invalid level view");
                h.levelView = static_cast<LevelView>(ival);
            } else if (key == "currentLinetype") {
                id(h.currentLinetype);
            } else if (key == "currentLineweight") {
                wantInt();
                if (ival < -4 || ival > 32767)
                    bad("invalid current lineweight");
                h.currentLineweight.v = static_cast<std::int16_t>(ival);
            } else if (key == "currentTransparency") {
                wantInt();
                if (!validObjectTransparency(static_cast<int>(ival)))
                    bad("invalid current transparency");
                h.currentTransparency = static_cast<std::int8_t>(ival);
            } else if (key == "currentTextStyle") {
                id(h.currentTextStyle);
            } else if (key == "currentDimStyle") {
                id(h.currentDimStyle);
            } else if (key.starts_with("next") && type == 1 &&
                       (key == "nextEntity" || key == "nextLayer" || key == "nextLevel" ||
                        key == "nextLinetype" || key == "nextTextStyle" || key == "nextDimStyle" ||
                        key == "nextBlock")) {
                if (ival < 1 || ival > (std::int64_t{1} << 48))
                    bad("HEAD next-id out of range");
                if (key == "nextEntity") {
                    nextIds_.entity = static_cast<std::uint64_t>(ival);
                } else {
                    if (ival > std::numeric_limits<std::uint32_t>::max())
                        bad("HEAD next-id out of range");
                    auto v = static_cast<std::uint32_t>(ival);
                    if (key == "nextLayer")
                        nextIds_.layer = v;
                    else if (key == "nextLevel")
                        nextIds_.level = v;
                    else if (key == "nextLinetype")
                        nextIds_.linetype = v;
                    else if (key == "nextTextStyle")
                        nextIds_.textStyle = v;
                    else if (key == "nextDimStyle")
                        nextIds_.dimStyle = v;
                    else
                        nextIds_.block = v;
                }
            } else if (key == "createdBy") {
                if (type != 0)
                    bad("HEAD key createdBy has the wrong type");
                h.createdBy = sval;
            } else if (key.starts_with(kCustomPrefix) && type == 0) {
                h.custom[key.substr(kCustomPrefix.size())] = sval;
            } else {
                report_.warnings.push_back("ignored unknown header entry \"" + key + "\"");
            }
        }
        if (!r.atEnd())
            bad("trailing data in HEAD");
        // Opaque chunks travel in Header::custom so that a later save writes them back.
        char idx[32];
        for (std::size_t i = 0; i < unknown_.size(); ++i) {
            const RawChunk& c = *unknown_[i];
            std::snprintf(idx, sizeof idx, "%08zu", i);
            ByteWriter blob;
            blob.u32(c.type);
            blob.u32(c.flags);
            blob.raw(c.stored);
            h.custom[std::string(kOpaqueChunkPrefix) + idx] =
                std::string(blob.bytes().begin(), blob.bytes().end());
        }
        report_.unknownChunks = unknown_.size();
        return h;
    }

    template <class Fn>
    void forRows(std::uint32_t type, std::size_t minRow, Fn&& fn) {
        const RawChunk* c = find(type);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        std::size_t n = r.count(minRow);
        std::uint64_t last = 0;
        for (std::size_t i = 0; i < n; ++i) {
            std::uint32_t id = id32(r);
            if (id <= last)
                bad("table rows must be in ascending id order (" + fourccName(type) + ")");
            last = id;
            fn(id, r);
        }
        if (!r.atEnd())
            bad("trailing data in " + fourccName(type));
    }

    // Makes the table's next id equal `want` by adding and erasing placeholders.
    template <class AddFn, class EraseFn>
    static void reachId(std::uint32_t want, std::uint32_t& next, AddFn&& add, EraseFn&& erase) {
        if (want < next)
            bad("table row id already taken");
        if (want - next > kMaxIdGap)
            bad("implausible gap in table row ids");
        while (next < want) {
            auto id = add();
            if (id.value != next)
                bad("internal id mismatch");
            erase(id);
            ++next;
        }
    }

    void loadLinetypes(Transaction& tx) {
        std::uint32_t next = 2;
        forRows(kLtyp, 4, [&](std::uint32_t id, ByteReader& r) {
            Linetype row;
            row.name = str(r);
            row.description = str(r);
            std::size_t n = r.count(8);
            row.pattern.resize(n);
            for (double& d : row.pattern)
                d = r.f64();
            if (id == kLinetypeContinuous.value) {
                check(tx.modifyLinetype(kLinetypeContinuous, [&](Linetype& l) { l = row; }),
                      "linetype Continuous");
                return;
            }
            reachId(
                id, next,
                [&] { return take(tx.addLinetype(Linetype{.name = "~burn"}), "linetype"); },
                [&](LinetypeId b) { check(tx.eraseLinetype(b), "linetype"); });
            LinetypeId got = take(tx.addLinetype(std::move(row)), "linetype");
            if (got.value != id)
                bad("internal id mismatch");
            ++next;
        });
    }

    void loadTextStyleExtras(Transaction& tx) {
        const RawChunk* c = find(kTstx);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        std::size_t n = r.count(4);
        for (std::size_t i = 0; i < n; ++i) {
            const TextStyleId id{id32(r)};
            const std::uint8_t flags = r.u8();
            std::string shx = str(r);
            std::string big = str(r);
            if (!doc_->textStyle(id))
                continue;
            check(tx.modifyTextStyle(id,
                                     [&](TextStyle& t) {
                                         t.annotative = (flags & 1) != 0;
                                         t.backward = (flags & 2) != 0;
                                         t.upsideDown = (flags & 4) != 0;
                                         t.shxFile = std::move(shx);
                                         t.bigFontFile = std::move(big);
                                     }),
                  "text style");
        }
        if (!r.atEnd())
            bad("trailing data in TSTX");
    }

    void loadLinetypeMarks(Transaction& tx) {
        const RawChunk* c = find(kLtmk);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        std::size_t n = r.count(2);
        for (std::size_t i = 0; i < n; ++i) {
            const LinetypeId id{id32(r)};
            std::size_t nm = r.count(30);
            std::vector<LinetypeMark> marks(nm);
            for (LinetypeMark& m : marks) {
                std::uint8_t kind = r.u8();
                std::uint8_t rot = r.u8();
                if (kind > 1 || rot > 2)
                    bad("invalid linetype mark");
                m.kind = static_cast<LinetypeMark::Kind>(kind);
                m.rotationKind = static_cast<LinetypeMark::Rotation>(rot);
                m.offset = r.f64();
                m.text = str(r);
                m.style = str(r);
                m.scale = r.f64();
                m.rotation = r.f64();
                m.shift.x = r.f64();
                m.shift.y = r.f64();
                if (!std::isfinite(m.offset) || !std::isfinite(m.scale) ||
                    !std::isfinite(m.rotation) || !std::isfinite(m.shift.x) ||
                    !std::isfinite(m.shift.y))
                    bad("invalid linetype mark");
            }
            if (!doc_->linetype(id))
                continue;
            check(tx.modifyLinetype(id, [&](Linetype& l) { l.marks = std::move(marks); }),
                  "linetype");
        }
        if (!r.atEnd())
            bad("trailing data in LTMK");
    }

    void loadTextStyles(Transaction& tx) {
        std::uint32_t next = 2;
        forRows(kTsty, 18, [&](std::uint32_t id, ByteReader& r) {
            TextStyle row;
            row.name = str(r);
            row.fontFamily = str(r);
            row.fontFile = str(r);
            row.widthFactor = r.f64();
            row.oblique = r.f64();
            if (id == kTextStyleStandard.value) {
                check(tx.modifyTextStyle(kTextStyleStandard, [&](TextStyle& t) { t = row; }),
                      "text style Standard");
                return;
            }
            reachId(
                id, next,
                [&] { return take(tx.addTextStyle(TextStyle{.name = "~burn"}), "text style"); },
                [&](TextStyleId b) { check(tx.eraseTextStyle(b), "text style"); });
            TextStyleId got = take(tx.addTextStyle(std::move(row)), "text style");
            if (got.value != id)
                bad("internal id mismatch");
            ++next;
        });
    }

    // Fixed text heights: a separate chunk so that older programs can still read the file.
    void loadTextHeights(Transaction& tx) {
        const RawChunk* c = find(kTsth);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        std::size_t n = r.count(9);
        for (std::size_t i = 0; i < n; ++i) {
            const TextStyleId id{id32(r)};
            const double h = r.f64();
            if (!std::isfinite(h) || h < 0)
                bad("invalid text style height");
            if (!doc_->textStyle(id))
                continue;
            check(tx.modifyTextStyle(id, [&](TextStyle& t) { t.height = h; }), "text style");
        }
        if (!r.atEnd())
            bad("trailing data in TSTH");
    }

    // Alternate units and tolerances of dimension styles (the DSTX chunk, absent when no style
    // uses them).
    void loadDimStyleExtras(Transaction& tx) {
        const RawChunk* c = find(kDstx);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        std::size_t n = r.count(20);
        for (std::size_t i = 0; i < n; ++i) {
            const DimStyleId id{id32(r)};
            const std::uint8_t flags = r.u8();
            if (flags > 7)
                bad("invalid dimension style flags");
            const double altScale = r.f64();
            const auto altUnit =
                readEnum<LengthUnit>(r, static_cast<std::uint8_t>(LengthUnit::Foot));
            const int altPrecision = r.i32();
            std::string altPrefix = str(r);
            std::string altSuffix = str(r);
            const auto tolerance =
                readEnum<DimTolerance>(r, static_cast<std::uint8_t>(DimTolerance::Limits));
            const double upper = r.f64();
            const double lower = r.f64();
            const int tolPrecision = r.i32();
            double linearFactor = 1;
            DimFit fit = DimFit::Best;
            DimTextMove textMove = DimTextMove::WithLine;
            if (flags & 2) {
                linearFactor = r.f64();
                fit = readEnum<DimFit>(r, static_cast<std::uint8_t>(DimFit::BothOutside));
                textMove = readEnum<DimTextMove>(r, static_cast<std::uint8_t>(DimTextMove::Free));
                if (!std::isfinite(linearFactor) || !(linearFactor > 0))
                    bad("invalid dimension style linear factor");
            }
            std::uint8_t rulesByte = kDimRulesDefault;
            int justify = 0;
            if (flags & 4) {
                rulesByte = r.u8();
                justify = r.u8();
            }
            if (!std::isfinite(altScale) || !(altScale > 0) || !std::isfinite(upper) ||
                !std::isfinite(lower) || upper < 0 || lower < 0)
                bad("invalid dimension style tolerance");
            if (!doc_->dimStyle(id))
                continue;
            check(tx.modifyDimStyle(id,
                                    [&](DimStyle& d) {
                                        d.altUnits = (flags & 1) != 0;
                                        d.linearFactor = linearFactor;
                                        d.fit = fit;
                                        d.textMove = textMove;
                                        unpackDimRules(d, rulesByte, justify);
                                        d.altScale = altScale;
                                        d.altUnit = altUnit;
                                        d.altPrecision = altPrecision;
                                        d.altPrefix = std::move(altPrefix);
                                        d.altSuffix = std::move(altSuffix);
                                        d.tolerance = tolerance;
                                        d.tolUpper = upper;
                                        d.tolLower = lower;
                                        d.tolPrecision = tolPrecision;
                                    }),
                  "dimension style");
        }
        if (!r.atEnd())
            bad("trailing data in DSTX");
    }

    void loadDimStyles(Transaction& tx) {
        std::uint32_t next = 2;
        forRows(kDsty, 60, [&](std::uint32_t id, ByteReader& r) {
            DimStyle row;
            row.name = str(r);
            row.textHeight = r.f64();
            row.textStyle = TextStyleId{static_cast<std::uint32_t>(
                std::min<std::uint64_t>(r.varint(), std::numeric_limits<std::uint32_t>::max()))};
            row.textGap = r.f64();
            row.arrowType = readEnum<ArrowType>(r, static_cast<std::uint8_t>(ArrowType::None));
            row.arrowSize = r.f64();
            row.extLineOffset = r.f64();
            row.extLineExtension = r.f64();
            row.dimLineExtension = r.f64();
            row.textPlacement = readEnum<DimTextPlacement>(
                r, static_cast<std::uint8_t>(DimTextPlacement::Centered));
            row.textAlign =
                readEnum<DimTextAlign>(r, static_cast<std::uint8_t>(DimTextAlign::Horizontal));
            row.units = readEnum<LengthUnit>(r, static_cast<std::uint8_t>(LengthUnit::Foot));
            row.precision = r.i32();
            row.prefix = str(r);
            row.suffix = str(r);
            row.roundOff = r.f64();
            row.overallScale = r.f64();
            std::uint8_t f = r.u8();
            if (f > 3)
                bad("invalid dimension style flags");
            row.suppressLeadingZeros = (f & 1) != 0;
            row.suppressTrailingZeros = (f & 2) != 0;
            row.dimLineColor = readColor(r);
            row.extLineColor = readColor(r);
            row.textColor = readColor(r);
            if (id == kDimStyleStandard.value) {
                check(tx.modifyDimStyle(kDimStyleStandard, [&](DimStyle& d) { d = row; }),
                      "dimension style Standard");
                return;
            }
            reachId(
                id, next,
                [&] { return take(tx.addDimStyle(DimStyle{.name = "~burn"}), "dim style"); },
                [&](DimStyleId b) { check(tx.eraseDimStyle(b), "dim style"); });
            DimStyleId got = take(tx.addDimStyle(std::move(row)), "dimension style");
            if (got.value != id)
                bad("internal id mismatch");
            ++next;
        });
    }

    void loadLayers(Transaction& tx) {
        // Two passes. First every layer is added flat under a unique temporary name so that ids
        // come out as stored; then the real name, parent and ByParent settings are applied
        // (a parent may have a higher id than its child).
        std::vector<Layer> rows;
        std::uint32_t next = 2;
        forRows(kLayr, 12, [&](std::uint32_t id, ByteReader& r) {
            Layer row;
            row.id = LayerId{id};
            row.name = str(r);
            std::uint64_t parent = r.varint();
            if (parent > std::numeric_limits<std::uint32_t>::max())
                bad("layer parent id out of range");
            row.parent = LayerId{static_cast<std::uint32_t>(parent)};
            row.color = readColor(r);
            row.linetype = LinetypeId{r.u32()};
            row.lineweight = LineWeight{r.i16()};
            std::uint8_t f = r.u8();
            if (f > 63)
                bad("invalid layer flags");
            row.on = (f & 1) != 0;
            row.frozen = (f & 2) != 0;
            row.locked = (f & 4) != 0;
            row.plot = (f & 8) != 0;
            if (f & 16) {  // transparency, written only when the layer has some
                row.transparency = r.u8();
                if (row.transparency > kTransparencyMax)
                    bad("invalid layer transparency");
            }
            if (f & 32)
                row.description = str(r);
            rows.push_back(std::move(row));
            if (id == kLayerZero.value)
                return;
            reachId(
                id, next, [&] { return take(tx.addLayer(Layer{.name = "~burn"}), "layer"); },
                [&](LayerId b) { check(tx.eraseLayer(b), "layer"); });
            LayerId got = take(tx.addLayer(Layer{.name = "~tmp" + std::to_string(id)}), "layer");
            if (got.value != id)
                bad("internal id mismatch");
            ++next;
        });
        for (const Layer& row : rows) {
            check(tx.modifyLayer(row.id, [&](Layer& l) { l = row; }), "layer \"" + row.name + "\"");
        }
    }

    void loadLevels(Transaction& tx) {
        std::uint32_t next = 2;
        forRows(kLvls, 11, [&](std::uint32_t id, ByteReader& r) {
            Level row;
            row.name = str(r);
            row.elevation = r.f64();
            std::uint8_t f = r.u8();
            if (f > 3)
                bad("invalid level flags");
            row.visible = (f & 1) != 0;
            row.locked = (f & 2) != 0;
            if (id == kLevelDefault.value) {
                check(tx.modifyLevel(kLevelDefault, [&](Level& l) { l = row; }), "level");
                return;
            }
            reachId(
                id, next, [&] { return take(tx.addLevel(Level{.name = "~burn"}), "level"); },
                [&](LevelId b) { check(tx.eraseLevel(b), "level"); });
            LevelId got = take(tx.addLevel(std::move(row)), "level");
            if (got.value != id)
                bad("internal id mismatch");
            ++next;
        });
    }

    // ELVL: entity id (delta coded) -> level id, for entities not on the default level.
    void loadEntityLevels() {
        const RawChunk* c = find(kElvl);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        std::size_t n = r.count(2);
        std::uint64_t prev = 0;
        for (std::size_t i = 0; i < n; ++i) {
            prev += r.varint();
            entityLevel_[prev] = LevelId{id32(r)};
        }
        if (!r.atEnd())
            bad("trailing data in ELVL");
    }

    void loadBlocks(Transaction& tx) {
        std::uint32_t next = 1;
        forRows(kBlks, 12, [&](std::uint32_t id, ByteReader& r) {
            BlockDef row;
            row.name = str(r);
            row.base = r.vec2();
            row.anonymous = r.boolean();
            reachId(
                id, next, [&] { return take(tx.addBlock(BlockDef{.name = "~burn"}), "block"); },
                [&](BlockId b) { check(tx.eraseBlock(b), "block"); });
            BlockId got = take(tx.addBlock(std::move(row)), "block");
            if (got.value != id)
                bad("internal id mismatch");
            ++next;
        });
    }

    void loadXrefs(Transaction& tx) {
        const RawChunk* c = find(kXrfs);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        std::size_t n = r.count(3);
        for (std::size_t i = 0; i < n; ++i) {
            BlockId b{id32(r)};
            std::string path = str(r);
            std::int64_t stamp = r.i64();
            if (!tx.modifyBlock(b, [&](BlockDef& d) {
                       d.xrefPath = path;
                       d.xrefStamp = stamp;
                   }).ok())
                bad("external reference names an unknown block");
        }
        if (!r.atEnd())
            bad("trailing data in XRFS");
    }

    void loadXrefOverlays(Transaction& tx) {
        const RawChunk* c = find(kXrfo);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        std::size_t n = r.count(1);
        for (std::size_t i = 0; i < n; ++i) {
            BlockId b{id32(r)};
            if (!tx.modifyBlock(b, [&](BlockDef& d) { d.xrefOverlay = !d.xrefPath.empty(); }).ok())
                bad("overlay flag names an unknown block");
        }
        if (!r.atEnd())
            bad("trailing data in XRFO");
    }

    void loadDynamic(Transaction& tx) {
        const RawChunk* c = find(kDynb);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        auto small = [&](std::uint64_t n, std::uint64_t cap, const char* what) {
            if (n > cap)
                bad(std::string(what) + " count out of range");
            return static_cast<std::size_t>(n);
        };
        auto ids = [&] {
            std::vector<EntityId> v(r.count(1));
            for (EntityId& e : v)
                e = EntityId{id32(r)};
            return v;
        };
        std::size_t n = r.count(3);
        for (std::size_t i = 0; i < n; ++i) {
            BlockId b{id32(r)};
            DynamicDef def;
            std::size_t np = r.count(20);
            for (std::size_t k = 0; k < np; ++k) {
                DynParam prm;
                std::uint8_t kind = r.u8();
                if (kind > static_cast<std::uint8_t>(DynParam::Kind::Visibility))
                    bad("unknown dynamic parameter kind");
                prm.kind = static_cast<DynParam::Kind>(kind);
                prm.name = str(r);
                prm.base = r.vec2();
                prm.end = r.vec2();
                prm.min = r.f64();
                prm.max = r.f64();
                std::size_t ns = small(r.varint(), 10000, "state");
                for (std::size_t s = 0; s < ns; ++s) {
                    prm.states.push_back(str(r));
                    prm.stateObjects.push_back(ids());
                }
                def.params.push_back(std::move(prm));
            }
            std::size_t na = r.count(20);
            for (std::size_t k = 0; k < na; ++k) {
                DynAction a;
                std::uint8_t kind = r.u8();
                if (kind > static_cast<std::uint8_t>(DynAction::Kind::Flip))
                    bad("unknown dynamic action kind");
                a.kind = static_cast<DynAction::Kind>(kind);
                a.param = static_cast<int>(small(r.varint(), 10000, "parameter"));
                a.frameMin = r.vec2();
                a.frameMax = r.vec2();
                a.objects = ids();
                def.actions.push_back(std::move(a));
            }
            if (!tx.modifyBlock(b, [&](BlockDef& d) { d.dynamic = std::move(def); }).ok())
                bad("dynamic definition names an unknown block");
        }
        if (!r.atEnd())
            bad("trailing data in DYNB");
    }

    void loadAssemblies(Transaction& tx) {
        const RawChunk* c = find(kAsmb);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        auto value = [&] {
            AsmValue v;
            v.name = str(r);
            v.isText = r.boolean();
            v.number = r.f64();
            v.text = str(r);
            return v;
        };
        std::size_t n = r.count(3);
        for (std::size_t i = 0; i < n; ++i) {
            BlockId b{id32(r)};
            AssemblyDef def;
            std::size_t np = r.count(12);
            for (std::size_t k = 0; k < np; ++k) {
                AsmParam prm;
                std::uint8_t kind = r.u8();
                if (kind > static_cast<std::uint8_t>(AsmParam::Kind::Choice))
                    bad("unknown assembly parameter kind");
                prm.kind = static_cast<AsmParam::Kind>(kind);
                prm.name = str(r);
                prm.defaultNumber = r.f64();
                prm.defaultText = str(r);
                prm.min = r.f64();
                prm.max = r.f64();
                std::size_t nc = r.count(1);
                for (std::size_t m = 0; m < nc; ++m)
                    prm.choices.push_back(str(r));
                def.params.push_back(std::move(prm));
            }
            std::size_t nb = r.count(4);
            for (std::size_t k = 0; k < nb; ++k) {
                AsmBinding bd;
                std::uint8_t kind = r.u8();
                if (kind > static_cast<std::uint8_t>(AsmBinding::Kind::ArrayField))
                    bad("unknown assembly binding kind");
                bd.kind = static_cast<AsmBinding::Kind>(kind);
                bd.entity = EntityId{r.varint()};
                bd.target = str(r);
                bd.formula = str(r);
                def.bindings.push_back(std::move(bd));
            }
            std::size_t npr = r.count(2);
            for (std::size_t k = 0; k < npr; ++k) {
                AsmPreset pr;
                pr.name = str(r);
                std::size_t nv = r.count(8);
                for (std::size_t m = 0; m < nv; ++m)
                    pr.values.push_back(value());
                def.presets.push_back(std::move(pr));
            }
            def.joinClass = str(r);
            if (!tx.modifyBlock(b, [&](BlockDef& d) { d.assembly = std::move(def); }).ok())
                bad("assembly definition names an unknown block");
        }
        if (!r.atEnd())
            bad("trailing data in ASMB");
    }

    void loadEntities(Transaction& tx) {
        std::uint64_t next = 1;
        for (const RawChunk* c : ents_) {
            if (salvage_) {
                try {
                    loadEntityChunk(tx, *c, next);
                } catch (const FormatError& e) {
                    report_.warnings.push_back(
                        std::string("damaged ENTS chunk: the rest of its objects were lost (") +
                        e.what() + ")");
                }
                continue;
            }
            loadEntityChunk(tx, *c, next);
        }
    }

    void loadEntityChunk(Transaction& tx, const RawChunk& chunk, std::uint64_t& next) {
        const RawChunk* c = &chunk;
        {
            Payload p(*c);
            ByteReader r(p.data);
            std::size_t n = r.count(8);
            for (std::size_t i = 0; i < n; ++i) {
                DecodedEntity d = decodeEntity(r, strings_);
                std::uint64_t id = d.entity.common.id.value;
                if (id < next)
                    bad("entity ids must be unique and ascending");
                if (id - next > kMaxIdGap)
                    bad("implausible gap in entity ids");
                for (; next < id; ++next) {
                    Entity filler;
                    filler.common.layer = kLayerZero;
                    filler.common.level = kLevelDefault;
                    filler.data = PointData{};
                    EntityId got = take(tx.add(std::move(filler)), "entity");
                    if (got.value != next)
                        bad("internal id mismatch");
                    check(tx.erase(got), "entity");
                }
                if (d.unknownKind)
                    ++report_.unknownEntityKinds;
                if (d.trailingBytes != 0) {
                    report_.warnings.push_back("entity " + std::to_string(id) + " has " +
                                               std::to_string(d.trailingBytes) +
                                               " bytes from a newer format that were ignored");
                }
                {
                    auto lv = entityLevel_.find(id);
                    d.entity.common.level = lv == entityLevel_.end() ? kLevelDefault : lv->second;
                }
                Result<EntityId> added = tx.add(std::move(d.entity), d.owner);
                if (!added) {
                    report_.warnings.push_back("skipped entity " + std::to_string(id) + ": " +
                                               added.error().message);
                    continue;
                }
                if (added.value().value != id)
                    bad("internal id mismatch");
                ++next;
                ++report_.entityCount;
            }
            if (!r.atEnd())
                bad("trailing data in ENTS");
        }
    }

    void loadOrder(Transaction& tx) {
        const RawChunk* c = find(kOrdr);
        if (!c)
            return;
        Payload p(*c);
        ByteReader r(p.data);
        std::size_t n = r.count(1);
        std::vector<EntityId> order;
        order.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            order.push_back(EntityId{r.varint()});
        if (!r.atEnd())
            bad("trailing data in ORDR");
        std::vector<EntityId> a = order, b = doc_->modelSpace();
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (a != b) {
            report_.warnings.push_back("ignored draw order that does not match the model space");
            return;
        }
        check(tx.setDrawOrder(order), "draw order");
    }

    const ParsedFile& file_;
    IoReport& report_;
    bool salvage_ = false;
    std::unique_ptr<Document> doc_{};
    std::vector<std::string> strings_{};
    std::unordered_map<std::uint32_t, bool> seen_{};
    std::unordered_map<std::uint32_t, const RawChunk*> single_{};
    std::vector<const RawChunk*> ents_{};
    std::vector<const RawChunk*> unknown_{};
};

}  // namespace

Result<std::unique_ptr<Document>> readTbcad(std::span<const std::uint8_t> bytes, IoReport& report,
                                            const ReadOptions& options) {
    report = IoReport{};
    try {
        ParsedFile f =
            parseFile(bytes, options.verifyCrc, options.salvage ? &report.warnings : nullptr);
        report.formatMajor = f.major;
        report.formatMinor = f.minor;
        if (f.trailingBytes != 0)
            report.warnings.push_back("ignored data after the END chunk");
        Loader loader(f, report, options.salvage);
        return loader.run();
    } catch (const FormatError& e) {
        return makeError(ErrorCode::Parse, e.what());
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::Parse, "file is corrupt (out of memory while reading)");
    } catch (const std::exception& e) {
        return makeError(ErrorCode::Parse, std::string("file is corrupt: ") + e.what());
    }
}

Result<std::unique_ptr<Document>> readTbcad(const std::filesystem::path& path, IoReport& report,
                                            const ReadOptions& options) {
    std::error_code ec;
    if (std::filesystem::is_directory(platform::longPath(path), ec))
        return makeError(ErrorCode::Io, path.string() + " is a folder, not a drawing file");
    std::ifstream in(platform::longPath(path), std::ios::binary | std::ios::ate);
    if (!in)
        return makeError(ErrorCode::Io, "cannot open " + path.string());
    std::streamoff size = in.tellg();
    if (size < 0)
        return makeError(ErrorCode::Io, "cannot read " + path.string());
    std::vector<std::uint8_t> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(size));
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::Io, path.string() + " is too large to open");
    }
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size))
        return makeError(ErrorCode::Io, "cannot read " + path.string());
    return readTbcad(std::span<const std::uint8_t>(bytes), report, options);
}

Result<std::vector<ChunkInfo>> listChunks(std::span<const std::uint8_t> bytes) {
    try {
        ParsedFile f = parseFile(bytes, false);
        std::vector<ChunkInfo> out;
        for (const RawChunk& c : f.chunks)
            out.push_back({fourccName(c.type), c.flags, c.stored.size(), c.crcOk});
        out.push_back({"END ", 0, 0, true});
        return out;
    } catch (const std::exception& e) {
        return makeError(ErrorCode::Parse, e.what());
    }
}

}  // namespace tbcad::io
