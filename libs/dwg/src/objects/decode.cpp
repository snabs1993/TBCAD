#include "objects/decode.h"

#include <algorithm>
#include <cmath>

#include "bits/bit_reader.h"

namespace tbcad::dwg::detail {

namespace {

constexpr double kDeg = 180.0 / 3.14159265358979323846;
constexpr std::size_t kMaxNotes = 200;

// Fixed object type numbers (ODA spec section 20.3).
enum Type : std::uint32_t {
    kText = 1,
    kAttrib = 2,
    kAttdef = 3,
    kBlock = 4,
    kEndBlk = 5,
    kSeqEnd = 6,
    kInsert = 7,
    kMinsert = 8,
    kVertex2d = 10,
    kPolyline2d = 15,
    kArc = 0x11,
    kCircle = 0x12,
    kLine = 0x13,
    kPoint = 0x1B,
    kSolid = 0x1F,
    kTrace = 0x20,
    kEllipse = 0x23,
    kSpline = 0x24,
    kMtext = 0x2C,
    kLeader = 0x2D,
    // Class-based objects (type numbers from 500 up) are mapped to private numbers by DXF name.
    kHatch = 1000,
    kDimOrdinate = 0x14,
    kDimLinear = 0x15,
    kDimAligned = 0x16,
    kDimAngular3 = 0x17,
    kDimAngular2 = 0x18,
    kDimRadius = 0x19,
    kDimDiameter = 0x1A,
    kBlockHeader = 0x31,
    kLayer = 0x33,
    kStyle = 0x35,
    kDimStyle = 0x45,
    kLtype = 0x39,
    kLwPolyline = 0x4D,
};

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

// A few helpers on top of BitReader for composite values.
Vec3 bd3(BitReader& r) {
    Vec3 p;
    p.x = r.bd();
    p.y = r.bd();
    p.z = r.bd();
    return p;
}

Vec3 extrusion(BitReader& r) {
    // R2000: one bit; 1 means (0,0,1), otherwise 3BD follows.
    if (r.b())
        return {0, 0, 1};
    return bd3(r);
}

class ObjectReader {
public:
    ObjectReader(Bytes file, const std::vector<ClassInfo>& classes, Drawing& out,
                 std::vector<std::string>& notes, const Format& fmt)
        : file_(file), classes_(classes), out_(out), notes_(notes), fmt_(fmt) {}

    void run(const std::vector<MapEntry>& map) {
        for (const MapEntry& e : map) {
            if (e.offset < 0 || static_cast<std::size_t>(e.offset) >= file_.size()) {
                note("object " + hex(e.handle) + " lies outside the file");
                continue;
            }
            decodeOne(e);
        }
        std::sort(out_.entities.begin(), out_.entities.end(),
                  [](const EntityRec& a, const EntityRec& b) { return a.handle < b.handle; });
    }

private:
    static std::string hex(std::uint64_t v) {
        static const char* d = "0123456789ABCDEF";
        std::string s;
        do {
            s.insert(s.begin(), d[v & 15]);
            v >>= 4;
        } while (v);
        return s;
    }

    void note(const std::string& s) {
        if (notes_.size() < kMaxNotes)
            notes_.push_back(s);
        else if (notes_.size() == kMaxNotes)
            notes_.push_back("further object problems are not listed");
    }

    std::string dxfNameOfClass(std::uint32_t type) const {
        if (type < 500)
            return {};
        const std::size_t idx = type - 500;
        return idx < classes_.size() ? classes_[idx].dxfName : std::string{};
    }

    std::size_t mainEnd_ = 0;

    // Where an object that could not be framed sits: offset, offset inside a 0x7400 page, and
    // its first bytes (a diagnostic for map or page misplacement).
    std::string where(const MapEntry& e) const {
        std::string w = " (at " + std::to_string(e.offset) + ", page offset " +
                        std::to_string(e.offset % 0x7400) + ", bytes";
        static const char* d = "0123456789ABCDEF";
        for (std::size_t i = 0; i < 8 && static_cast<std::size_t>(e.offset) + i < file_.size();
             ++i) {
            const std::uint8_t b = file_[static_cast<std::size_t>(e.offset) + i];
            w += ' ';
            w += d[b >> 4];
            w += d[b & 15];
        }
        return w + ")";
    }

    void decodeOne(const MapEntry& e) {
        BitReader head(file_.subspan(static_cast<std::size_t>(e.offset)));
        const std::uint32_t size = head.ms();
        // R2010 and later: the size of the handle stream (in bits) follows the size. The size
        // counts the bytes after that field (checked against a real R2018 file).
        std::size_t handleBits = 0;
        if (fmt_.r2010)
            handleBits = static_cast<std::size_t>(head.mcUnsigned());
        const std::size_t lead = head.bytePos();
        if (head.failed() || size == 0 || size > file_.size() ||
            static_cast<std::size_t>(e.offset) + lead + size > file_.size()) {
            note("object " + hex(e.handle) + " has an impossible size" + where(e));
            return;
        }
        Bytes body = file_.subspan(static_cast<std::size_t>(e.offset) + lead, size);
        BitReader r(body);
        std::uint32_t type = fmt_.r2010 ? r.objectTypeR2010() : static_cast<std::uint32_t>(r.bs());
        if (type >= 500) {
            const std::string cls = dxfNameOfClass(type);
            if (cls == "LWPOLYLINE")
                type = kLwPolyline;
            else if (cls == "HATCH")
                type = kHatch;
        }
        std::size_t objBits = 0;
        if (fmt_.r2010) {
            if (handleBits > body.size() * 8) {
                note("object " + hex(e.handle) + " has a damaged header" + where(e));
                return;
            }
            objBits = body.size() * 8 - handleBits;
        } else {
            objBits = r.rl();
        }
        const HandleRef self = r.handle();
        if (r.failed() || objBits > body.size() * 8) {
            note("object " + hex(e.handle) + " has a damaged header" + where(e));
            return;
        }
        // R2007 and later: strings live in their own stream just before the handle stream.
        BitReader strings;
        if (fmt_.r2007 && !locateStringStream(body, objBits, strings)) {
            note("object " + hex(e.handle) + " has a damaged string stream" + where(e));
            return;
        }
        mainEnd_ = strings.bitSize() != 0 ? strings.bitPos() : (objBits > 0 ? objBits - 1 : 0);
        if (strings.bitSize() != 0)
            r.setStringStream(&strings);
        switch (type) {
            case kBlockHeader:
            case kLayer:
            case kStyle:
            case kLtype:
            case kDimStyle:
                decodeTableRecord(type, r, objBits, body, self.value);
                return;
            case kLine:
            case kCircle:
            case kArc:
            case kText:
            case kInsert:
            case kLwPolyline:
            case kPoint:
            case kSolid:
            case kTrace:
            case kEllipse:
            case kSpline:
            case kAttrib:
            case kAttdef:
            case kMinsert:
            case kVertex2d:
            case kPolyline2d:
            case kMtext:
            case kHatch:
            case kLeader:
            case kDimOrdinate:
            case kDimLinear:
            case kDimAligned:
            case kDimAngular3:
            case kDimAngular2:
            case kDimRadius:
            case kDimDiameter:
                decodeEntity(type, r, objBits, body, self.value);
                return;
            case kBlock:
            case kEndBlk:
            case kSeqEnd:
                return;  // the block header carries the name and the base point
            default:
                break;
        }
        ++out_.skipped;
        std::string name = type >= 500 ? dxfNameOfClass(type) : "type " + std::to_string(type);
        if (name.empty())
            name = "type " + std::to_string(type);
        ++out_.skippedTypes[name];
    }

    // The exact bit where the main (non-string, non-handle) data of the current object must end
    // (R2007 and later): the start of the string stream, or the string flag when there is none.
    // Layout variants that differ between versions are chosen by which one lands exactly here.
    void tally(std::uint32_t type, const BitReader& r) {
        if (!fmt_.r2007 || r.failed())
            return;
        auto& t = out_.layoutCheck[type];
        ++t[r.bitPos() == mainEnd_ ? 0 : 1];
    }

    // Table records start with a 64-bit flag, an xref index and an xref-dependency flag. Files
    // written by recent versions leave the index out (checked on a real R2018 file).
    int tripleVariant_ = -1;  // 1 = xref index present, 0 = absent, -1 = not decided yet
    void readTriple(BitReader& r, bool& flag64, bool& xdep) {
        flag64 = r.b();
        if (tripleVariant_ != 0)
            r.bs();
        xdep = r.b();
    }

    // Entities: the "no links" bit that older versions store may be missing.
    int nolinksVariant_ = -1;
    // INSERT: the owned-object count is stored for every insert (the description says so) or only
    // when attributes follow (what a real R2018 file does).
    bool insertCountAlways_ = true;
    bool insertCountKnown_ = false;

    // ---- shared prefixes ----------------------------------------------------------------------

    struct ColorValue {
        int index = 256;
        int rgb = -1;
        bool book = false;
    };

    // CMC: a plain index up to R2000; from R2004 on an index (always 0), an RGB word whose top
    // byte says how to read it, and optional names (ODA spec 2.11).
    ColorValue readCmc(BitReader& r, int defaultIndex) {
        ColorValue c;
        const int idx = r.bs();
        c.index = idx;
        if (!fmt_.r2004)
            return c;
        const std::uint32_t word = r.bl();
        const unsigned flags = r.rc();
        if (flags & 1)
            r.textBytes();
        if (flags & 2)
            r.textBytes();
        switch (word >> 24) {
            case 0xC3:
                c.index = static_cast<int>(word & 0xFF);
                break;
            case 0xC2:
                c.rgb = static_cast<int>(word & 0xFFFFFF);
                c.index = defaultIndex;
                break;
            default:
                c.index = idx > 0 ? idx : defaultIndex;
                break;
        }
        return c;
    }

    // ENC (entity colour, R2004+): flags in the high bits of the first value (ODA spec 2.11).
    ColorValue readEnc(BitReader& r) {
        ColorValue c;
        const unsigned v = static_cast<unsigned>(r.bs()) & 0xFFFF;
        c.index = static_cast<int>(v & 0x1FF);
        if (v & 0x8000) {
            c.rgb = static_cast<int>(r.bl() & 0xFFFFFF);
            c.index = 256;
        }
        if (v & 0x4000)
            c.book = true;
        if (v & 0x2000)
            r.bl();  // transparency
        return c;
    }

    void skipEed(BitReader& r) {
        for (int guard = 0; guard < 100000; ++guard) {
            const std::int32_t size = r.bs();
            if (size <= 0 || r.failed())
                return;
            r.handle();
            for (std::int32_t i = 0; i < size && !r.failed(); ++i)
                r.rc();
        }
    }

    // After the object's own handle: extended data, then (non-entities) the reactor count.
    void commonObjectPrefix(BitReader& r, std::uint32_t& reactors) {
        skipEed(r);
        reactors = static_cast<std::uint32_t>(r.bl());
        xdicMissing_ = false;
        if (fmt_.r2004)
            xdicMissing_ = r.b();
        if (fmt_.r2013)
            r.b();  // object has data in the data store section
    }

    // Handle stream: owner/parent, reactors, xdictionary. Returns the owner handle.
    std::uint64_t leadingHandles(BitReader& h, std::uint64_t self, std::uint32_t reactors,
                                 bool hasOwner) {
        std::uint64_t owner = 0;
        if (hasOwner)
            owner = resolveHandle(h.handle(), self);
        for (std::uint32_t i = 0; i < reactors && i < 100000 && !h.failed(); ++i)
            h.handle();
        if (!xdicMissing_)
            h.handle();  // xdictionary (always stored up to R2000, possibly null)
        return owner;
    }

    void decodeTableRecord(std::uint32_t type, BitReader& r, std::size_t objBits, Bytes body,
                           std::uint64_t self) {
        std::uint32_t reactors = 0;
        commonObjectPrefix(r, reactors);
        BitReader h(body, objBits);
        switch (type) {
            case kLayer: {
                LayerRec l;
                l.handle = self;
                if (fmt_.r2007 && tripleVariant_ < 0) {
                    for (int v : {1, 0}) {
                        BitReader probe = r;
                        tripleVariant_ = v;
                        bool f = false, x = false;
                        readTriple(probe, f, x);
                        probe.bs();
                        readCmc(probe, 7);
                        if (!probe.failed() && probe.bitPos() == mainEnd_)
                            break;
                        tripleVariant_ = -1;
                    }
                }
                l.name = r.textBytes();
                bool flag64 = false, xdep = false;
                readTriple(r, flag64, xdep);
                const int values = r.bs();
                const ColorValue color = readCmc(r, 7);
                leadingHandles(h, self, reactors, true);
                h.handle();  // external reference block
                h.handle();  // plot style
                if (fmt_.r2007)
                    h.handle();  // material
                const HandleRef lt = h.handle();
                if (r.failed()) {
                    note("layer " + hex(self) + " is damaged");
                    return;
                }
                l.frozen = (values & 1) != 0;
                l.locked = (values & 8) != 0;
                l.plot = (values & 16) != 0;
                l.lineweight = lineweightToDxf((values & 0x03E0) >> 5);
                l.color = color.index;
                l.trueColor = color.rgb;
                l.flags = (l.frozen ? 1 : 0) | (l.locked ? 4 : 0) | (xdep ? 16 : 0);
                l.linetype = resolveHandle(lt, self);
                out_.layers[self] = std::move(l);
                break;
            }
            case kLtype: {
                LinetypeRec t;
                t.handle = self;
                t.name = r.textBytes();
                {
                    bool f = false, x = false;
                    readTriple(r, f, x);
                }
                t.description = r.textBytes();
                t.patternLength = r.bd();
                r.rc();  // alignment, always 'A'
                const int n = r.rc();
                for (int i = 0; i < n && !r.failed(); ++i) {
                    t.dashes.push_back(r.bd());
                    r.bs();  // shape code
                    r.rd();
                    r.rd();
                    r.bd();
                    r.bd();
                    r.bs();  // shape flags
                }
                if (r.failed()) {
                    note("linetype " + hex(self) + " is damaged");
                    return;
                }
                out_.linetypes[self] = std::move(t);
                break;
            }
            case kStyle: {
                StyleRec s;
                s.handle = self;
                s.name = r.textBytes();
                bool f64 = false, xdep = false;
                readTriple(r, f64, xdep);
                const bool vertical = r.b();
                const bool shapeFile = r.b();
                s.height = r.bd();
                s.width = r.bd();
                s.oblique = r.bd();
                s.generation = r.rc();
                s.lastHeight = r.bd();
                s.font = r.textBytes();
                s.bigFont = r.textBytes();
                if (r.failed()) {
                    note("text style " + hex(self) + " is damaged");
                    return;
                }
                s.flags = (shapeFile ? 1 : 0) | (vertical ? 4 : 0) | (xdep ? 16 : 0);
                out_.styles[self] = std::move(s);
                break;
            }
            case kDimStyle: {
                DimStyleRec d;
                d.handle = self;
                d.name = r.textBytes();
                bool flag64 = false, xdep = false;
                readTriple(r, flag64, xdep);
                dimStyleVars(r, d);
                const bool unknown = r.b();
                if (r.failed()) {
                    note("dimension style " + hex(self) + " is damaged");
                    return;
                }
                d.flags = (flag64 ? 64 : 0) | (xdep ? 16 : 0) | (unknown ? 1 : 0);
                out_.dimStyles[self] = std::move(d);
                break;
            }
            case kBlockHeader: {
                BlockRec b;
                b.handle = self;
                b.name = r.textBytes();
                bool f64 = false, xdep = false;
                readTriple(r, f64, xdep);
                const bool anonymous = r.b();
                const bool hasAtts = r.b();
                const bool isXref = r.b();
                const bool overlay = r.b();
                r.b();  // loaded bit
                if (fmt_.r2004)
                    r.bl();  // owned object count
                b.baseX = r.bd();
                b.baseY = r.bd();
                b.baseZ = r.bd();
                b.xrefPath = r.textBytes();
                if (fmt_.r2007) {
                    // Insert count (non-zero bytes ending in a zero), description, preview, units.
                    for (int i = 0; i < 100000 && !r.failed() && r.rc() != 0; ++i) {
                    }
                    r.textBytes();
                    const std::uint32_t preview = r.bl();
                    if (preview > r.bitsLeft() / 8)
                        r.fail();
                    else
                        r.seekBit(r.bitPos() + static_cast<std::size_t>(preview) * 8);
                    r.bs();  // insert units
                    r.b();   // explodable
                    r.rc();  // block scaling
                }
                if (r.failed()) {
                    note("block header " + hex(self) + " is damaged");
                    return;
                }
                b.flags = (anonymous ? 1 : 0) | (hasAtts ? 2 : 0) | (isXref ? 4 : 0) |
                          (overlay ? 8 : 0) | (xdep ? 16 : 0);
                out_.blocks[self] = std::move(b);
                break;
            }
            default:
                break;
        }
        tally(type, r);
    }

    // ---- entities -----------------------------------------------------------------------------

    struct CommonBits {
        int entmode = 0;
        std::uint32_t reactors = 0;
        bool nolinks = true;
        int color = 256;
        int trueColor = -1;
        bool colorBook = false;
        double ltScale = 1;
        int ltFlags = 0;
        int plotFlags = 0;
        int materialFlags = 0;
        bool visualStyles[3] = {false, false, false};
        int invisible = 0;
        int lineweight = 29;
    };

    bool readCommonEntity(BitReader& r, CommonBits& c) {
        skipEed(r);
        if (r.b()) {  // graphic image present
            const std::uint64_t bytes = fmt_.r2010 ? static_cast<std::uint64_t>(r.bll()) : r.rl();
            if (bytes > r.bitsLeft() / 8)
                return false;
            r.seekBit(r.bitPos() + static_cast<std::size_t>(bytes) * 8);
        }
        c.entmode = static_cast<int>(r.bb());
        c.reactors = static_cast<std::uint32_t>(r.bl());
        xdicMissing_ = false;
        if (fmt_.r2004)
            xdicMissing_ = r.b();
        if (fmt_.r2013)
            r.b();  // object has data in the data store section
        if (nolinksVariant_ != 0 || !fmt_.r2007)
            c.nolinks = r.b();
        if (fmt_.r2004) {
            const ColorValue cv = readEnc(r);
            c.color = cv.index;
            c.trueColor = cv.rgb;
            c.colorBook = cv.book;
        } else {
            c.color = r.bs();
        }
        c.ltScale = r.bd();
        c.ltFlags = static_cast<int>(r.bb());
        c.plotFlags = static_cast<int>(r.bb());
        if (fmt_.r2007) {
            c.materialFlags = static_cast<int>(r.bb());
            r.rc();  // shadow flags
        }
        if (fmt_.r2010) {
            for (bool& v : c.visualStyles)
                v = r.b();
        }
        c.invisible = r.bs();
        c.lineweight = r.rc();
        return !r.failed();
    }

    void decodeEntity(std::uint32_t type, BitReader& r, std::size_t objBits, Bytes body,
                      std::uint64_t self) {
        CommonBits c;
        ownedCount_ = 0;
        if (fmt_.r2007 && type == kLine && nolinksVariant_ < 0) {
            for (int v : {1, 0}) {
                BitReader probe = r;
                nolinksVariant_ = v;
                CommonBits pc;
                EntityRec pe;
                if (readCommonEntity(probe, pc)) {
                    line(probe, pe);
                    if (!probe.failed() && probe.bitPos() == mainEnd_)
                        break;
                }
                nolinksVariant_ = -1;
            }
        }
        if (fmt_.r2007 && (type == kInsert) && !insertCountKnown_) {
            bool fits[2] = {false, false};
            for (int v = 0; v < 2; ++v) {
                BitReader probe = r;
                insertCountAlways_ = v == 1;
                CommonBits pc;
                EntityRec pe;
                if (readCommonEntity(probe, pc)) {
                    insert(probe, pe, false);
                    fits[v] = !probe.failed() && probe.bitPos() == mainEnd_;
                }
            }
            if (fits[0] != fits[1]) {
                insertCountAlways_ = fits[1];
                insertCountKnown_ = true;
            } else {
                insertCountAlways_ = true;
            }
        }
        if (!readCommonEntity(r, c)) {
            note("entity " + hex(self) + " has a damaged common part");
            return;
        }
        EntityRec e;
        e.handle = self;
        e.entmode = c.entmode;
        e.color = c.color;
        e.trueColor = c.trueColor;
        e.ltScale = c.ltScale;
        e.invisible = (c.invisible & 1) != 0;
        e.lineweight = c.lineweight;
        e.ltFlags = c.ltFlags;
        bool wantStyle = false;
        bool wantBlock = false;
        bool wantChildren = false;
        bool wantDim = false;
        bool wantLeader = false;
        switch (type) {
            case kLine:
                line(r, e);
                break;
            case kCircle:
                circle(r, e);
                break;
            case kArc:
                arc(r, e);
                break;
            case kText:
                text(r, e, "TEXT");
                wantStyle = true;
                break;
            case kAttrib:
            case kAttdef:
                text(r, e, type == kAttrib ? "ATTRIB" : "ATTDEF");
                attribTail(r, e, type == kAttdef);
                wantStyle = true;
                break;
            case kInsert:
            case kMinsert:
                insert(r, e, type == kMinsert);
                wantBlock = true;
                wantChildren = e.hasChildren;
                break;
            case kPolyline2d:
                polyline2d(r, e);
                wantChildren = true;
                break;
            case kVertex2d:
                vertex2d(r, e);
                break;
            case kMtext:
                mtext(r, e);
                wantStyle = true;
                break;
            case kHatch:
                hatch(r, e);
                break;
            case kLeader:
                leader(r, e);
                wantLeader = true;
                break;
            case kDimOrdinate:
            case kDimLinear:
            case kDimAligned:
            case kDimAngular3:
            case kDimAngular2:
            case kDimRadius:
            case kDimDiameter:
                dimension(r, e, type);
                wantDim = true;
                break;
            case kLwPolyline:
                lwPolyline(r, e);
                break;
            case kPoint:
                point(r, e);
                break;
            case kSolid:
            case kTrace:
                solid(r, e, type == kTrace);
                break;
            case kEllipse:
                ellipse(r, e);
                break;
            case kSpline:
                spline(r, e);
                break;
            default:
                return;
        }
        tally(type, r);
        if (r.failed()) {
            note(e.dxfType + " " + hex(self) + " is damaged and was skipped (read to bit " +
                 std::to_string(r.bitPos()) + " of " + std::to_string(mainEnd_) + ")");
            return;
        }
        BitReader h(body, objBits);
        e.owner = leadingHandles(h, self, c.reactors, c.entmode == 0);
        if (!c.nolinks && !fmt_.r2004) {
            h.handle();  // previous entity
            h.handle();  // next entity
        }
        if (c.colorBook)
            h.handle();
        e.layer = resolveHandle(h.handle(), self);
        if (c.ltFlags == 3)
            e.linetype = resolveHandle(h.handle(), self);
        if (fmt_.r2007 && c.materialFlags == 3)
            h.handle();
        if (c.plotFlags == 3)
            h.handle();
        for (bool v : c.visualStyles)
            if (v)
                h.handle();
        if (wantStyle)
            e.style = resolveHandle(h.handle(), self);
        if (wantBlock)
            e.blockHeader = resolveHandle(h.handle(), self);
        if (wantLeader)
            h.handle();  // associated annotation
        if (wantLeader || wantDim)
            e.dimStyle = resolveHandle(h.handle(), self);
        if (wantDim)
            e.blockHeader = resolveHandle(h.handle(), self);
        if (wantChildren) {
            if (fmt_.r2004) {
                const std::int32_t n = std::max<std::int32_t>(0, ownedCount_);
                for (std::int32_t i = 0; i < n && !h.failed() && h.bitsLeft() >= 8; ++i)
                    e.childHandles.push_back(resolveHandle(h.handle(), self));
                h.handle();  // SEQEND
            } else {
                e.firstChild = resolveHandle(h.handle(), self);
                e.lastChild = resolveHandle(h.handle(), self);
                h.handle();  // SEQEND
            }
        }
        if (h.failed()) {
            note(e.dxfType + " " + hex(self) + " has damaged references and was skipped");
            return;
        }
        out_.entities.push_back(std::move(e));
    }

    static void add(EntityRec& e, int code, Value v) { e.groups.push_back({code, std::move(v)}); }
    // Numeric groups are built in place (avoids a GCC maybe-uninitialized false positive on the
    // variant move).
    static void add(EntityRec& e, int code, double v) {
        Group& g = e.groups.emplace_back();
        g.code = code;
        g.value.v.emplace<double>(v);
    }
    static void add(EntityRec& e, int code, int v) {
        Group& g = e.groups.emplace_back();
        g.code = code;
        g.value.v.emplace<long long>(v);
    }
    static void add(EntityRec& e, int code, long long v) {
        Group& g = e.groups.emplace_back();
        g.code = code;
        g.value.v.emplace<long long>(v);
    }

    static void point3(EntityRec& e, int code, const Vec3& p) {
        add(e, code, p.x);
        add(e, code + 10, p.y);
        add(e, code + 20, p.z);
    }

    static void extrusionGroups(EntityRec& e, const Vec3& n) {
        if (n.x != 0 || n.y != 0 || n.z != 1)
            point3(e, 210, n);
    }

    void line(BitReader& r, EntityRec& e) {
        e.dxfType = "LINE";
        const bool zeros = r.b();
        Vec3 a, b;
        a.x = r.rd();
        b.x = r.bdWithDefault(a.x);
        a.y = r.rd();
        b.y = r.bdWithDefault(a.y);
        if (!zeros) {
            a.z = r.rd();
            b.z = r.bdWithDefault(a.z);
        }
        const double thickness = r.bitThickness();
        const Vec3 n = extrusion(r);
        point3(e, 10, a);
        point3(e, 11, b);
        if (thickness != 0)
            add(e, 39, thickness);
        extrusionGroups(e, n);
    }

    void circle(BitReader& r, EntityRec& e) {
        e.dxfType = "CIRCLE";
        const Vec3 c = bd3(r);
        const double radius = r.bd();
        const double thickness = r.bitThickness();
        const Vec3 n = extrusion(r);
        point3(e, 10, c);
        add(e, 40, radius);
        if (thickness != 0)
            add(e, 39, thickness);
        extrusionGroups(e, n);
    }

    void arc(BitReader& r, EntityRec& e) {
        e.dxfType = "ARC";
        const Vec3 c = bd3(r);
        const double radius = r.bd();
        const double thickness = r.bitThickness();
        const Vec3 n = extrusion(r);
        const double a0 = r.bd();
        const double a1 = r.bd();
        point3(e, 10, c);
        add(e, 40, radius);
        if (thickness != 0)
            add(e, 39, thickness);
        add(e, 50, a0 * kDeg);
        add(e, 51, a1 * kDeg);
        extrusionGroups(e, n);
    }

    void point(BitReader& r, EntityRec& e) {
        e.dxfType = "POINT";
        const Vec3 p = bd3(r);
        const double thickness = r.bitThickness();
        const Vec3 n = extrusion(r);
        r.bd();  // x axis angle
        point3(e, 10, p);
        if (thickness != 0)
            add(e, 39, thickness);
        extrusionGroups(e, n);
    }

    void text(BitReader& r, EntityRec& e, const char* dxfType) {
        e.dxfType = dxfType;
        const unsigned flags = r.rc();
        double elevation = 0;
        if (!(flags & 0x01))
            elevation = r.rd();
        const double ix = r.rd();
        const double iy = r.rd();
        double ax = ix, ay = iy;
        const bool aligned = !(flags & 0x02);
        if (aligned) {
            ax = r.bdWithDefault(ix);
            ay = r.bdWithDefault(iy);
        }
        const Vec3 n = extrusion(r);
        const double thickness = r.bitThickness();
        double oblique = 0, rotation = 0, width = 1;
        if (!(flags & 0x04))
            oblique = r.rd();
        if (!(flags & 0x08))
            rotation = r.rd();
        const double height = r.rd();
        if (!(flags & 0x10))
            width = r.rd();
        const std::string value = r.textBytes();
        int generation = 0, horizontal = 0, vertical = 0;
        if (!(flags & 0x20))
            generation = r.bs();
        if (!(flags & 0x40))
            horizontal = r.bs();
        if (!(flags & 0x80))
            vertical = r.bs();
        point3(e, 10, {ix, iy, elevation});
        add(e, 40, height);
        add(e, 1, value);
        if (rotation != 0)
            add(e, 50, rotation * kDeg);
        if (width != 1)
            add(e, 41, width);
        if (oblique != 0)
            add(e, 51, oblique * kDeg);
        if (generation != 0)
            add(e, 71, generation);
        if (horizontal != 0)
            add(e, 72, horizontal);
        if (vertical != 0)
            add(e, 73, vertical);
        if (aligned && (horizontal != 0 || vertical != 0))
            point3(e, 11, {ax, ay, elevation});
        if (thickness != 0)
            add(e, 39, thickness);
        extrusionGroups(e, n);
    }

    void solid(BitReader& r, EntityRec& e, bool trace) {
        e.dxfType = trace ? "TRACE" : "SOLID";
        const double thickness = r.bitThickness();
        const double elevation = r.bd();
        double xy[4][2];
        for (auto& c : xy) {
            c[0] = r.rd();
            c[1] = r.rd();
        }
        const Vec3 n = extrusion(r);
        for (int i = 0; i < 4; ++i)
            point3(e, 10 + i, {xy[i][0], xy[i][1], elevation});
        if (thickness != 0)
            add(e, 39, thickness);
        extrusionGroups(e, n);
    }

    void ellipse(BitReader& r, EntityRec& e) {
        e.dxfType = "ELLIPSE";
        const Vec3 center = bd3(r);
        const Vec3 major = bd3(r);
        const Vec3 n = bd3(r);
        const double ratio = r.bd();
        const double a0 = r.bd();
        const double a1 = r.bd();
        point3(e, 10, center);
        point3(e, 11, major);
        extrusionGroups(e, n);
        add(e, 40, ratio);
        add(e, 41, a0);
        add(e, 42, a1);
    }

    void spline(BitReader& r, EntityRec& e) {
        e.dxfType = "SPLINE";
        const std::int32_t scenario = r.bl();
        bool flagClosed = false;
        if (fmt_.r2013) {
            const std::int32_t flags1 = r.bl();
            r.bl();  // knot parameter
            flagClosed = (flags1 & 4) != 0;
        }
        const std::int32_t degree = r.bl();
        bool rational = false, closed = flagClosed, periodic = false;
        double fitTol = 0, knotTol = 0, ctrlTol = 0;
        Vec3 beginTan, endTan;
        std::int32_t knots = 0, ctrl = 0, fit = 0;
        bool weighted = false;
        if (scenario == 2) {
            fitTol = r.bd();
            beginTan = bd3(r);
            endTan = bd3(r);
            fit = r.bl();
        } else if (scenario == 1) {
            rational = r.b();
            closed = r.b();
            periodic = r.b();
            knotTol = r.bd();
            ctrlTol = r.bd();
            knots = r.bl();
            ctrl = r.bl();
            weighted = r.b();
        } else {
            r.fail();
            return;
        }
        // Every value needs at least two bits, so counts beyond the data are damage.
        const auto limit = static_cast<std::int64_t>(r.bitsLeft()) / 2;
        if (degree < 0 || degree > 1000 || knots < 0 || ctrl < 0 || fit < 0 || knots > limit ||
            ctrl > limit || fit > limit) {
            r.fail();
            return;
        }
        std::vector<double> knotValues;
        for (std::int32_t i = 0; i < knots && !r.failed(); ++i)
            knotValues.push_back(r.bd());
        std::vector<Vec3> ctrlPts;
        std::vector<double> weights;
        for (std::int32_t i = 0; i < ctrl && !r.failed(); ++i) {
            ctrlPts.push_back(bd3(r));
            if (weighted)
                weights.push_back(r.bd());
        }
        std::vector<Vec3> fitPts;
        for (std::int32_t i = 0; i < fit && !r.failed(); ++i)
            fitPts.push_back(bd3(r));
        add(e, 70, (closed ? 1 : 0) | (periodic ? 2 : 0) | (rational ? 4 : 0));
        add(e, 71, degree);
        add(e, 72, static_cast<long long>(knotValues.size()));
        add(e, 73, static_cast<long long>(ctrlPts.size()));
        add(e, 74, static_cast<long long>(fitPts.size()));
        if (knotTol != 0)
            add(e, 42, knotTol);
        if (ctrlTol != 0)
            add(e, 43, ctrlTol);
        if (fitTol != 0)
            add(e, 44, fitTol);
        if (scenario == 2) {
            point3(e, 12, beginTan);
            point3(e, 13, endTan);
        }
        for (double k : knotValues)
            add(e, 40, k);
        for (std::size_t i = 0; i < ctrlPts.size(); ++i) {
            point3(e, 10, ctrlPts[i]);
            if (i < weights.size())
                add(e, 41, weights[i]);
        }
        for (const Vec3& p : fitPts)
            point3(e, 11, p);
    }

    static void point2(EntityRec& e, int code, double x, double y) {
        add(e, code, x);
        add(e, code + 10, y);
    }

    // HATCH (ODA spec 20.4.75, R2000 layout). Angles are taken as radians, as for the other
    // angles of the format; this is unconfirmed until a real file is checked.
    void hatch(BitReader& r, EntityRec& e) {
        e.dxfType = "HATCH";
        if (fmt_.r2004) {
            // Gradient fill description; the hatch is imported with a plain fill.
            r.bl();  // is gradient
            r.bl();
            r.bd();  // angle
            r.bd();  // shift
            r.bl();  // single colour
            r.bd();  // tint
            const std::int32_t colors = r.bl();
            if (colors < 0 ||
                static_cast<std::int64_t>(colors) > static_cast<std::int64_t>(r.bitsLeft()) / 8) {
                r.fail();
                return;
            }
            for (std::int32_t i = 0; i < colors && !r.failed(); ++i) {
                r.bd();
                r.bs();
                r.bl();
                r.rc();
            }
            r.textBytes();  // gradient name
        }
        const double elevation = r.bd();
        const Vec3 n = bd3(r);
        const std::string name = r.textBytes();
        const bool solid = r.b();
        const bool assoc = r.b();
        const std::int32_t paths = r.bl();
        const auto limit = static_cast<std::int64_t>(r.bitsLeft());
        if (paths < 0 || paths > limit / 4) {
            r.fail();
            return;
        }
        point3(e, 10, {0, 0, elevation});
        extrusionGroups(e, n);
        add(e, 2, name);
        add(e, 70, solid ? 1 : 0);
        add(e, 71, assoc ? 1 : 0);
        add(e, 91, paths);
        bool anyDerived = false;
        for (std::int32_t pi = 0; pi < paths && !r.failed(); ++pi) {
            const std::int32_t flag = r.bl();
            if (flag & 4)
                anyDerived = true;
            add(e, 92, flag);
            if (!(flag & 2)) {
                const std::int32_t segs = r.bl();
                if (segs < 0 ||
                    static_cast<std::int64_t>(segs) > static_cast<std::int64_t>(r.bitsLeft()) / 4) {
                    r.fail();
                    return;
                }
                add(e, 93, segs);
                for (std::int32_t si = 0; si < segs && !r.failed(); ++si)
                    hatchEdge(r, e);
            } else {
                const bool bulges = r.b();
                const bool closed = r.b();
                const std::int32_t count = r.bl();
                if (count < 0 || static_cast<std::int64_t>(count) >
                                     static_cast<std::int64_t>(r.bitsLeft()) / 16) {
                    r.fail();
                    return;
                }
                add(e, 72, bulges ? 1 : 0);
                add(e, 73, closed ? 1 : 0);
                add(e, 93, count);
                for (std::int32_t vi = 0; vi < count && !r.failed(); ++vi) {
                    const double x = r.rd();
                    const double y = r.rd();
                    point2(e, 10, x, y);
                    if (bulges) {
                        const double b = r.bd();
                        add(e, 42, b);
                    }
                }
            }
            const std::int32_t handles = r.bl();  // boundary objects: not carried over
            (void)handles;
            add(e, 97, 0);
        }
        const int style = r.bs();
        const int patternType = r.bs();
        add(e, 75, style);
        add(e, 76, patternType);
        if (!solid) {
            const double angle = r.bd();
            const double scale = r.bd();
            const bool doubled = r.b();
            const int lines = r.bs();
            if (lines < 0 ||
                static_cast<std::int64_t>(lines) > static_cast<std::int64_t>(r.bitsLeft()) / 8) {
                r.fail();
                return;
            }
            add(e, 52, angle * kDeg);
            add(e, 41, scale);
            add(e, 77, doubled ? 1 : 0);
            add(e, 78, lines);
            for (int li = 0; li < lines && !r.failed(); ++li) {
                const double a = r.bd();
                const double px = r.bd();
                const double py = r.bd();
                const double ox = r.bd();
                const double oy = r.bd();
                const int dashes = r.bs();
                if (dashes < 0 || static_cast<std::int64_t>(dashes) >
                                      static_cast<std::int64_t>(r.bitsLeft()) / 2) {
                    r.fail();
                    return;
                }
                add(e, 53, a * kDeg);
                add(e, 43, px);
                add(e, 44, py);
                add(e, 45, ox);
                add(e, 46, oy);
                add(e, 79, dashes);
                for (int di = 0; di < dashes && !r.failed(); ++di)
                    add(e, 49, r.bd());
            }
        }
        if (anyDerived)
            add(e, 47, r.bd());
        const std::int32_t seeds = r.bl();
        if (seeds < 0 ||
            static_cast<std::int64_t>(seeds) > static_cast<std::int64_t>(r.bitsLeft()) / 16) {
            r.fail();
            return;
        }
        add(e, 98, seeds);
        for (std::int32_t i = 0; i < seeds && !r.failed(); ++i) {
            const double x = r.rd();
            const double y = r.rd();
            point2(e, 10, x, y);
        }
    }

    void hatchEdge(BitReader& r, EntityRec& e) {
        const int type = r.rc();
        add(e, 72, type);
        switch (type) {
            case 1: {
                const double x0 = r.rd(), y0 = r.rd(), x1 = r.rd(), y1 = r.rd();
                point2(e, 10, x0, y0);
                point2(e, 11, x1, y1);
                break;
            }
            case 2: {
                const double cx = r.rd(), cy = r.rd();
                const double radius = r.bd();
                const double a0 = r.bd();
                const double a1 = r.bd();
                const bool ccw = r.b();
                point2(e, 10, cx, cy);
                add(e, 40, radius);
                add(e, 50, a0 * kDeg);
                add(e, 51, a1 * kDeg);
                add(e, 73, ccw ? 1 : 0);
                break;
            }
            case 3: {
                const double cx = r.rd(), cy = r.rd(), mx = r.rd(), my = r.rd();
                const double ratio = r.bd();
                const double a0 = r.bd();
                const double a1 = r.bd();
                const bool ccw = r.b();
                point2(e, 10, cx, cy);
                point2(e, 11, mx, my);
                add(e, 40, ratio);
                add(e, 50, a0 * kDeg);
                add(e, 51, a1 * kDeg);
                add(e, 73, ccw ? 1 : 0);
                break;
            }
            case 4: {
                const std::int32_t degree = r.bl();
                const bool rational = r.b();
                const bool periodic = r.b();
                const std::int32_t knots = r.bl();
                const std::int32_t ctrl = r.bl();
                const auto limit = static_cast<std::int64_t>(r.bitsLeft()) / 2;
                if (degree < 0 || degree > 1000 || knots < 0 || ctrl < 0 || knots > limit ||
                    ctrl > limit) {
                    r.fail();
                    return;
                }
                add(e, 94, degree);
                add(e, 73, rational ? 1 : 0);
                add(e, 74, periodic ? 1 : 0);
                add(e, 95, knots);
                add(e, 96, ctrl);
                for (std::int32_t i = 0; i < knots && !r.failed(); ++i)
                    add(e, 40, r.bd());
                for (std::int32_t i = 0; i < ctrl && !r.failed(); ++i) {
                    const double x = r.rd(), y = r.rd();
                    point2(e, 10, x, y);
                    if (rational)
                        add(e, 42, r.bd());
                }
                if (fmt_.r2010) {
                    const std::int32_t fit = r.bl();
                    if (fit < 0 || static_cast<std::int64_t>(fit) >
                                       static_cast<std::int64_t>(r.bitsLeft()) / 16) {
                        r.fail();
                        return;
                    }
                    for (std::int32_t i = 0; i < fit && !r.failed(); ++i) {
                        r.rd();
                        r.rd();
                    }
                    r.rd();  // start tangent
                    r.rd();
                    r.rd();  // end tangent
                    r.rd();
                }
                break;
            }
            default:
                r.fail();  // unknown edge type: the rest of the object cannot be read
                break;
        }
    }

    // DIMSTYLE variables in file order (ODA spec 20.4.68, R2000), each with its DXF group code.
    void dimStyleVars(BitReader& r, DimStyleRec& d) {
        // Code 0 reads a value that has no DXF group here and drops it.
        auto bd = [&](int code) {
            const double v = r.bd();
            if (code)
                d.groups.push_back({code, v});
        };
        auto bs = [&](int code) {
            const int v = r.bs();
            if (code)
                d.groups.push_back({code, v});
        };
        auto bit = [&](int code) {
            const int v = r.b() ? 1 : 0;
            if (code)
                d.groups.push_back({code, v});
        };
        auto tv = [&](int code) {
            std::string v = r.textBytes();
            if (code)
                d.groups.push_back({code, std::move(v)});
        };
        auto cmc = [&](int code) {
            const int v = readCmc(r, 0).index;
            if (code)
                d.groups.push_back({code, v});
        };
        tv(3);
        tv(4);
        for (int code : {40, 41, 42, 43, 44, 45, 46, 47, 48})
            bd(code);
        if (fmt_.r2007) {
            bd(0);
            bd(0);
            bs(0);
            cmc(0);
        }
        for (int code : {71, 72, 73, 74, 75, 76})
            bit(code);
        for (int code : {77, 78, 79})
            bs(code);
        if (fmt_.r2007)
            bs(90);
        for (int code : {140, 141, 142, 143, 144, 145, 146, 147, 148})
            bd(code);
        bit(170);
        bs(171);
        for (int code : {172, 173, 174, 175})
            bit(code);
        for (int code : {176, 177, 178})
            cmc(code);
        for (int code : {179, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280})
            bs(code);
        bit(281);
        bit(282);
        for (int code : {283, 284, 285, 286})
            bs(code);
        bit(288);
        bs(287);
        if (fmt_.r2007)
            bit(290);
        if (fmt_.r2010) {
            bit(295);
            bd(0);  // DIMALTMZF
            tv(0);  // DIMALTMZS
            bd(0);  // DIMMZF
            tv(0);  // DIMMZS
        }
        bs(371);
        bs(372);
    }

    // Common data of all DIMENSION kinds (ODA spec 20.4.22, R2000). Returns the kind-specific
    // DXF type flags through `flags1`.
    struct DimCommon {
        Vec3 extrusion{0, 0, 1};
        double midX = 0, midY = 0, elevation = 0;
        int flags1 = 0;
        std::string text{};
        double textRot = 0, horizDir = 0, insRot = 0;
        int attach = 0, spacingStyle = 0;
        double spacingFactor = 1, measurement = 0;
        double insX = 0, insY = 0;
    };

    void dimension(BitReader& r, EntityRec& e, std::uint32_t type) {
        e.dxfType = "DIMENSION";
        DimCommon c;
        if (fmt_.r2010)
            r.rc();  // version
        c.extrusion = bd3(r);
        c.midX = r.rd();
        c.midY = r.rd();
        c.elevation = r.bd();
        c.flags1 = r.rc();
        c.text = r.textBytes();
        c.textRot = r.bd();
        c.horizDir = r.bd();
        r.bd();  // insertion scales of the dimension block (x, y, z) and rotation: not used
        r.bd();
        r.bd();
        c.insRot = r.bd();
        c.attach = r.bs();
        c.spacingStyle = r.bs();
        c.spacingFactor = r.bd();
        c.measurement = r.bd();
        if (fmt_.r2007) {
            r.b();  // unknown
            r.b();  // flip arrow 1
            r.b();  // flip arrow 2
        }
        c.insX = r.rd();
        c.insY = r.rd();

        Vec3 p10, p13, p14, p15;
        double p16x = 0, p16y = 0, extRot = 0, dimRot = 0, leaderLen = 0;
        int dxfKind = 0;
        bool has13 = false, has14 = false, has15 = false, has16 = false;
        int flags2 = 0;
        switch (type) {
            case kDimOrdinate:
                dxfKind = 6;
                p10 = bd3(r);
                p13 = bd3(r);
                p14 = bd3(r);
                flags2 = r.rc();
                has13 = has14 = true;
                break;
            case kDimLinear:
            case kDimAligned:
                dxfKind = type == kDimLinear ? 0 : 1;
                p13 = bd3(r);
                p14 = bd3(r);
                p10 = bd3(r);
                extRot = r.bd();
                if (type == kDimLinear)
                    dimRot = r.bd();
                has13 = has14 = true;
                break;
            case kDimAngular3:
                dxfKind = 5;
                p10 = bd3(r);
                p13 = bd3(r);
                p14 = bd3(r);
                p15 = bd3(r);
                has13 = has14 = has15 = true;
                break;
            case kDimAngular2:
                dxfKind = 2;
                p16x = r.rd();
                p16y = r.rd();
                p13 = bd3(r);
                p14 = bd3(r);
                p15 = bd3(r);
                p10 = bd3(r);
                has13 = has14 = has15 = has16 = true;
                break;
            case kDimRadius:
            case kDimDiameter:
                dxfKind = type == kDimRadius ? 4 : 3;
                if (type == kDimRadius) {
                    p10 = bd3(r);
                    p15 = bd3(r);
                } else {
                    p15 = bd3(r);
                    p10 = bd3(r);
                }
                leaderLen = r.bd();
                has15 = true;
                break;
            default:
                break;
        }
        if (r.failed())
            return;
        // The DXF type word: the kind plus the bits that Flags 1 and Flags 2 stand for.
        int dxfFlags = dxfKind;
        if (!(c.flags1 & 1))
            dxfFlags |= 128;
        if (c.flags1 & 2)
            dxfFlags |= 32;
        if (type == kDimOrdinate && (flags2 & 1))
            dxfFlags |= 64;
        if (!c.text.empty())
            add(e, 1, c.text);
        point3(e, 10, p10);
        point3(e, 11, {c.midX, c.midY, c.elevation});
        point3(e, 12, {c.insX, c.insY, c.elevation});
        add(e, 70, dxfFlags);
        if (has13)
            point3(e, 13, p13);
        if (has14)
            point3(e, 14, p14);
        if (has15)
            point3(e, 15, p15);
        if (has16)
            point3(e, 16, {p16x, p16y, c.elevation});
        if (c.textRot != 0)
            add(e, 53, c.textRot * kDeg);
        if (c.horizDir != 0)
            add(e, 51, c.horizDir * kDeg);
        add(e, 71, c.attach);
        add(e, 72, c.spacingStyle);
        add(e, 41, c.spacingFactor);
        add(e, 42, c.measurement);
        if (dimRot != 0)
            add(e, 50, dimRot * kDeg);
        if (extRot != 0)
            add(e, 52, extRot * kDeg);
        if (leaderLen != 0)
            add(e, 40, leaderLen);
        extrusionGroups(e, c.extrusion);
    }

    void leader(BitReader& r, EntityRec& e) {
        e.dxfType = "LEADER";
        r.b();  // always 0
        const int annotation = r.bs();
        const int pathType = r.bs();
        const std::int32_t count = r.bl();
        if (count < 0 ||
            static_cast<std::int64_t>(count) > static_cast<std::int64_t>(r.bitsLeft()) / 6) {
            r.fail();
            return;
        }
        std::vector<Vec3> pts;
        for (std::int32_t i = 0; i < count && !r.failed(); ++i)
            pts.push_back(bd3(r));
        bd3(r);  // plane origin
        const Vec3 n = bd3(r);
        const Vec3 xdir = bd3(r);
        bd3(r);  // offset to the block insertion point
        bd3(r);  // end point projection
        // The text box size is missing from leaders without an annotation in R2018 files: pick
        // the tail that ends exactly where the object's data ends.
        double boxHeight = 0, boxWidth = 0;
        bool hookOnX = false, arrow = false;
        auto tail = [&](BitReader& t, bool withBox) {
            if (withBox) {
                boxHeight = t.bd();
                boxWidth = t.bd();
            }
            hookOnX = t.b();
            arrow = t.b();
            t.bs();
            t.b();
            t.b();
        };
        BitReader withBox = r;
        tail(withBox, true);
        if (fmt_.r2007 && (withBox.failed() || withBox.bitPos() != mainEnd_)) {
            BitReader without = r;
            tail(without, false);
            if (!without.failed() && without.bitPos() == mainEnd_) {
                r = without;
            } else {
                tail(r, true);
            }
        } else {
            tail(r, true);
        }
        add(e, 71, arrow ? 1 : 0);
        add(e, 72, pathType);
        add(e, 73, annotation);
        add(e, 74, hookOnX ? 1 : 0);
        add(e, 40, boxHeight);
        add(e, 41, boxWidth);
        add(e, 76, static_cast<long long>(pts.size()));
        for (const Vec3& p : pts)
            point3(e, 10, p);
        extrusionGroups(e, n);
        point3(e, 211, xdir);
    }

    // The tail ATTRIB and ATTDEF add to the TEXT data (ODA spec 20.4.4, 20.4.5; R2000 layout).
    void attribTail(BitReader& r, EntityRec& e, bool def) {
        if (fmt_.r2010)
            r.rc();  // version
        if (fmt_.r2018) {
            const int kind = r.rc();
            if (kind != 1) {
                r.fail();  // multi-line attributes carry an embedded MTEXT: not read yet
                return;
            }
        }
        const std::string tag = r.textBytes();
        r.bs();  // field length, unused
        const int flags = r.rc();
        if (fmt_.r2007)
            r.b();  // lock position
        std::string prompt;
        if (def) {
            if (fmt_.r2010)
                r.rc();  // version
            prompt = r.textBytes();
        }
        add(e, 2, tag);
        add(e, 70, flags);
        if (def)
            add(e, 3, prompt);
    }

    void polyline2d(BitReader& r, EntityRec& e) {
        e.dxfType = "POLYLINE";
        const int flags = r.bs();
        const int curve = r.bs();
        const double w0 = r.bd();
        const double w1 = r.bd();
        const double thickness = r.bitThickness();
        const double elevation = r.bd();
        const Vec3 n = extrusion(r);
        if (fmt_.r2004)
            ownedCount_ = r.bl();
        point3(e, 10, {0, 0, elevation});
        if (thickness != 0)
            add(e, 39, thickness);
        add(e, 70, flags);
        if (w0 != 0)
            add(e, 40, w0);
        if (w1 != 0)
            add(e, 41, w1);
        if (curve != 0)
            add(e, 75, curve);
        extrusionGroups(e, n);
    }

    void vertex2d(BitReader& r, EntityRec& e) {
        e.dxfType = "VERTEX";
        const int flags = r.rc();
        const Vec3 p = bd3(r);
        double w0 = r.bd();
        double w1 = w0;
        if (w0 < 0)
            w0 = w1 = -w0;
        else
            w1 = r.bd();
        const double bulge = r.bd();
        if (fmt_.r2010)
            r.bl();  // vertex id
        const double tangent = r.bd();
        point3(e, 10, p);
        if (w0 != 0)
            add(e, 40, w0);
        if (w1 != 0)
            add(e, 41, w1);
        if (bulge != 0)
            add(e, 42, bulge);
        add(e, 70, flags);
        if (tangent != 0)
            add(e, 50, tangent * kDeg);
    }

    void mtext(BitReader& r, EntityRec& e) {
        e.dxfType = "MTEXT";
        const Vec3 p = bd3(r);
        const Vec3 n = bd3(r);
        const Vec3 xdir = bd3(r);
        const double width = r.bd();
        if (fmt_.r2007)
            r.bd();  // reference rectangle height
        const double height = r.bd();
        const int attach = r.bs();
        const int direction = r.bs();
        r.bd();  // extents height
        r.bd();  // extents width
        const std::string value = r.textBytes();
        const int spacingStyle = r.bs();
        const double spacingFactor = r.bd();
        r.b();  // unknown bit
        if (fmt_.r2004) {
            const std::uint32_t background = r.bl();
            if ((background & 1) || (fmt_.r2018 && (background & 0x10))) {
                r.bl();  // background scale factor
                readCmc(r, 7);
                r.bl();  // background transparency
            }
            if (fmt_.r2018 && r.b()) {  // not annotative: a second copy of the geometry follows
                r.bs();                 // version
                r.b();                  // default flag
                r.bl();                 // attachment point
                bd3(r);
                bd3(r);
                for (int i = 0; i < 4; ++i)
                    r.bd();
                const int columnType = r.bs();
                if (columnType != 0) {
                    const std::uint32_t heights = r.bl();
                    r.bd();  // column width
                    r.bd();  // gutter
                    const bool autoHeight = r.b();
                    r.b();  // flow reversed
                    if (!autoHeight && columnType == 2)
                        for (std::uint32_t i = 0; i < heights && i < 10000 && !r.failed(); ++i)
                            r.bd();
                }
            }
        }
        point3(e, 10, p);
        add(e, 40, height);
        if (width != 0)
            add(e, 41, width);
        add(e, 71, attach);
        add(e, 72, direction);
        add(e, 1, value);
        point3(e, 11, xdir);
        if (spacingStyle != 0)
            add(e, 73, spacingStyle);
        if (spacingFactor != 1 && spacingFactor != 0)
            add(e, 44, spacingFactor);
        extrusionGroups(e, n);
    }

    void insert(BitReader& r, EntityRec& e, bool multi) {
        e.dxfType = "INSERT";
        const Vec3 p = bd3(r);
        Vec3 s{1, 1, 1};
        switch (r.bb()) {
            case 3:
                break;
            case 1:
                s.y = r.bdWithDefault(1.0);
                s.z = r.bdWithDefault(1.0);
                break;
            case 2:
                s.x = r.rd();
                s.y = s.x;
                s.z = s.x;
                break;
            default:
                s.x = r.rd();
                s.y = r.bdWithDefault(s.x);
                s.z = r.bdWithDefault(s.x);
                break;
        }
        const double rotation = r.bd();
        const Vec3 n = bd3(r);
        const bool attribs = r.b();
        if (fmt_.r2004 && (attribs || insertCountAlways_))
            ownedCount_ = r.bl();
        point3(e, 10, p);
        if (s.x != 1)
            add(e, 41, s.x);
        if (s.y != 1)
            add(e, 42, s.y);
        if (s.z != 1)
            add(e, 43, s.z);
        if (rotation != 0)
            add(e, 50, rotation * kDeg);
        if (n.x != 0 || n.y != 0 || n.z != 1)
            point3(e, 210, n);
        e.hasChildren = attribs;
        if (multi) {
            const int cols = r.bs();
            const int rows = r.bs();
            const double colSpacing = r.bd();
            const double rowSpacing = r.bd();
            if (cols > 1)
                add(e, 70, cols);
            if (rows > 1)
                add(e, 71, rows);
            if (colSpacing != 0)
                add(e, 44, colSpacing);
            if (rowSpacing != 0)
                add(e, 45, rowSpacing);
        }
    }

    void lwPolyline(BitReader& r, EntityRec& e) {
        e.dxfType = "LWPOLYLINE";
        const int flag = r.bs();
        double constWidth = 0, elevation = 0, thickness = 0;
        Vec3 normal{0, 0, 1};
        if (flag & 4)
            constWidth = r.bd();
        if (flag & 8)
            elevation = r.bd();
        if (flag & 2)
            thickness = r.bd();
        if (flag & 1)
            normal = bd3(r);
        const std::int32_t count = r.bl();
        std::int32_t bulges = 0, widths = 0, ids = 0;
        if (flag & 16)
            bulges = r.bl();
        if (flag & 1024)
            ids = r.bl();
        if (flag & 32)
            widths = r.bl();
        // Every vertex needs at least two bits, so a count beyond the data is damage.
        const auto limit = static_cast<std::int64_t>(r.bitsLeft());
        if (count < 0 || bulges < 0 || widths < 0 || ids < 0 || count > limit / 2 ||
            bulges > limit || widths > limit || ids > limit) {
            r.fail();
            return;
        }
        std::vector<std::pair<double, double>> pts;
        pts.reserve(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count && !r.failed(); ++i) {
            if (i == 0) {
                const double x = r.rd();
                const double y = r.rd();
                pts.emplace_back(x, y);
            } else {
                const double x = r.bdWithDefault(pts.back().first);
                const double y = r.bdWithDefault(pts.back().second);
                pts.emplace_back(x, y);
            }
        }
        std::vector<double> bulgeValues;
        for (std::int32_t i = 0; i < bulges && !r.failed(); ++i)
            bulgeValues.push_back(r.bd());
        for (std::int32_t i = 0; i < ids && !r.failed(); ++i)
            r.bl();
        std::vector<std::pair<double, double>> w;
        for (std::int32_t i = 0; i < widths && !r.failed(); ++i) {
            const double a = r.bd();
            const double b = r.bd();
            w.emplace_back(a, b);
        }
        add(e, 90, static_cast<long long>(pts.size()));
        // 512 is the "closed" bit, 256 the linetype generation bit (inferred from the flag
        // numbering).
        add(e, 70, ((flag & 512) ? 1 : 0) | ((flag & 256) ? 128 : 0));
        if (constWidth != 0)
            add(e, 43, constWidth);
        if (elevation != 0)
            add(e, 38, elevation);
        if (thickness != 0)
            add(e, 39, thickness);
        for (std::size_t i = 0; i < pts.size(); ++i) {
            add(e, 10, pts[i].first);
            add(e, 20, pts[i].second);
            if (i < w.size()) {
                add(e, 40, w[i].first);
                add(e, 41, w[i].second);
            }
            if (i < bulgeValues.size() && bulgeValues[i] != 0)
                add(e, 42, bulgeValues[i]);
        }
        extrusionGroups(e, normal);
    }

    Bytes file_;
    const std::vector<ClassInfo>& classes_;
    Drawing& out_;
    std::vector<std::string>& notes_;
    Format fmt_;
    bool xdicMissing_ = false;  // R2004+: the object stores no extension dictionary handle
    std::int32_t ownedCount_ = 0;
};

}  // namespace

int lineweightToDxf(int code) {
    static constexpr int kStandard[24] = {0,  5,  9,  13, 15, 18,  20,  25,  30,  35,  40,  50,
                                          53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};
    if (code >= 0 && code < 24)
        return kStandard[code];
    if (code == 29)
        return -1;  // by layer
    if (code == 30)
        return -2;  // by block
    return -3;      // default
}

void decodeObjects(Bytes file, const std::vector<MapEntry>& map,
                   const std::vector<ClassInfo>& classes, Drawing& out,
                   std::vector<std::string>& notes, const Format& fmt) {
    ObjectReader reader(file, classes, out, notes, fmt);
    reader.run(map);
}

void describeObjects(Bytes file, const std::vector<MapEntry>& map, const Format& fmt,
                     std::vector<std::string>& lines) {
    std::map<std::uint32_t, std::size_t> histogram, tails;
    std::size_t bad = 0, shown = 0;
    for (const MapEntry& e : map) {
        if (e.offset < 0 || static_cast<std::size_t>(e.offset) >= file.size()) {
            ++bad;
            continue;
        }
        BitReader head(file.subspan(static_cast<std::size_t>(e.offset)));
        const std::uint32_t size = head.ms();
        std::size_t handleBits = 0;
        if (fmt.r2010)
            handleBits = static_cast<std::size_t>(head.mcUnsigned());
        const std::size_t lead = head.bytePos();
        if (head.failed() || size == 0 ||
            static_cast<std::size_t>(e.offset) + lead + size > file.size()) {
            ++bad;
            if (shown++ < 12)
                lines.push_back("object map entry handle " + std::to_string(e.handle) + " at " +
                                std::to_string(e.offset) + ": impossible size");
            continue;
        }
        BitReader r(file.subspan(static_cast<std::size_t>(e.offset) + lead, size));
        const std::uint32_t type =
            fmt.r2010 ? r.objectTypeR2010() : static_cast<std::uint32_t>(r.bs());
        std::size_t objBits = fmt.r2010 ? size * 8 - handleBits : r.rl();
        const HandleRef self = r.handle();
        ++histogram[type];
        // All data bits and the handle stream bits of a few table records and entities, so a
        // layout can be checked field by field against the format description.
        if (objBits >= 8 && objBits <= size * 8 &&
            (tails[type]++ < (type == 7 ? 4 : 2) || e.handle == 3105) &&
            (type == 49 || type == 51 || type == 53 || type == 57 || type == 69 || type == 19 ||
             type == 1 || type == 18 || type == 17 || type == 7 || type == 44 || type == 45 ||
             type == 21 || e.handle == 3105)) {
            Bytes ob = file.subspan(static_cast<std::size_t>(e.offset) + lead, size);
            auto dump = [&](std::size_t from, std::size_t to, std::size_t cap) {
                std::string bits;
                BitReader t(ob, from);
                for (std::size_t i = from; i < to && i < from + cap; ++i)
                    bits += t.b() ? '1' : '0';
                return bits;
            };
            lines.push_back("bits of type " + std::to_string(type) + " object " +
                            std::to_string(e.handle) + " (" + std::to_string(objBits) +
                            " data bits): " + dump(0, objBits, 3200));
            lines.push_back("handle bits of that object (" + std::to_string(handleBits) +
                            "): " + dump(objBits, size * 8, 400));
        }
        if (shown++ < 12)
            lines.push_back("object " + std::to_string(e.handle) + " at " +
                            std::to_string(e.offset) + ": " + std::to_string(size) +
                            " bytes, type " + std::to_string(type) + ", handle bits " +
                            std::to_string(handleBits) + ", data bits " + std::to_string(objBits) +
                            ", own handle " + std::to_string(self.value) +
                            (r.failed() ? " (header read failed)" : ""));
    }
    lines.push_back("objects: " + std::to_string(map.size()) + " in the map, " +
                    std::to_string(bad) + " unreadable");
    std::string h = "object types (type:count):";
    std::size_t n = 0;
    for (const auto& [t, c] : histogram) {
        if (n++ >= 80) {
            h += " ...";
            break;
        }
        h += " " + std::to_string(t) + ":" + std::to_string(c);
    }
    lines.push_back(h);
}

}  // namespace tbcad::dwg::detail
