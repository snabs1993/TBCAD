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
                 std::vector<std::string>& notes)
        : file_(file), classes_(classes), out_(out), notes_(notes) {}

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

    void decodeOne(const MapEntry& e) {
        BitReader head(file_.subspan(static_cast<std::size_t>(e.offset)));
        const std::uint32_t size = head.ms();
        const std::size_t lead = head.bytePos();
        if (head.failed() || size == 0 || size > file_.size() ||
            static_cast<std::size_t>(e.offset) + lead + size > file_.size()) {
            note("object " + hex(e.handle) + " has an impossible size");
            return;
        }
        Bytes body = file_.subspan(static_cast<std::size_t>(e.offset) + lead, size);
        BitReader r(body);
        std::uint32_t type = static_cast<std::uint32_t>(r.bs());
        if (type >= 500) {
            const std::string cls = dxfNameOfClass(type);
            if (cls == "LWPOLYLINE")
                type = kLwPolyline;
            else if (cls == "HATCH")
                type = kHatch;
        }
        const std::size_t objBits = r.rl();
        const HandleRef self = r.handle();
        if (r.failed() || objBits > body.size() * 8) {
            note("object " + hex(e.handle) + " has a damaged header");
            return;
        }
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

    // ---- shared prefixes ----------------------------------------------------------------------

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
    }

    // Handle stream: owner/parent, reactors, xdictionary. Returns the owner handle.
    std::uint64_t leadingHandles(BitReader& h, std::uint64_t self, std::uint32_t reactors,
                                 bool hasOwner) {
        std::uint64_t owner = 0;
        if (hasOwner)
            owner = resolveHandle(h.handle(), self);
        for (std::uint32_t i = 0; i < reactors && i < 100000 && !h.failed(); ++i)
            h.handle();
        h.handle();  // xdictionary (R2000 always stores it, possibly null)
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
                l.name = r.textBytes();
                r.b();   // 64 flag
                r.bs();  // xref index
                const bool xdep = r.b();
                const int values = r.bs();
                const int color = r.bs();
                leadingHandles(h, self, reactors, true);
                h.handle();  // external reference block
                h.handle();  // plot style
                const HandleRef lt = h.handle();
                if (r.failed()) {
                    note("layer " + hex(self) + " is damaged");
                    return;
                }
                l.frozen = (values & 1) != 0;
                l.locked = (values & 8) != 0;
                l.plot = (values & 16) != 0;
                l.lineweight = lineweightToDxf((values & 0x03E0) >> 5);
                l.color = color;
                l.flags = (l.frozen ? 1 : 0) | (l.locked ? 4 : 0) | (xdep ? 16 : 0);
                l.linetype = resolveHandle(lt, self);
                out_.layers[self] = std::move(l);
                break;
            }
            case kLtype: {
                LinetypeRec t;
                t.handle = self;
                t.name = r.textBytes();
                r.b();
                r.bs();
                r.b();
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
                r.b();
                r.bs();
                const bool xdep = r.b();
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
                const bool flag64 = r.b();
                r.bs();  // xref index + 1
                const bool xdep = r.b();
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
                r.b();
                r.bs();
                const bool xdep = r.b();
                const bool anonymous = r.b();
                const bool hasAtts = r.b();
                const bool isXref = r.b();
                const bool overlay = r.b();
                r.b();  // loaded bit
                b.baseX = r.bd();
                b.baseY = r.bd();
                b.baseZ = r.bd();
                b.xrefPath = r.textBytes();
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
    }

    // ---- entities -----------------------------------------------------------------------------

    struct CommonBits {
        int entmode = 0;
        std::uint32_t reactors = 0;
        bool nolinks = true;
        int color = 256;
        double ltScale = 1;
        int ltFlags = 0;
        int plotFlags = 0;
        int invisible = 0;
        int lineweight = 29;
    };

    bool readCommonEntity(BitReader& r, CommonBits& c) {
        skipEed(r);
        if (r.b()) {  // graphic image present
            const std::uint32_t bytes = r.rl();
            if (bytes > r.bitsLeft() / 8) {
                return false;
            }
            for (std::uint32_t i = 0; i < bytes; ++i)
                r.rc();
        }
        c.entmode = static_cast<int>(r.bb());
        c.reactors = static_cast<std::uint32_t>(r.bl());
        c.nolinks = r.b();
        c.color = r.bs();
        c.ltScale = r.bd();
        c.ltFlags = static_cast<int>(r.bb());
        c.plotFlags = static_cast<int>(r.bb());
        c.invisible = r.bs();
        c.lineweight = r.rc();
        return !r.failed();
    }

    void decodeEntity(std::uint32_t type, BitReader& r, std::size_t objBits, Bytes body,
                      std::uint64_t self) {
        CommonBits c;
        if (!readCommonEntity(r, c)) {
            note("entity " + hex(self) + " has a damaged common part");
            return;
        }
        EntityRec e;
        e.handle = self;
        e.entmode = c.entmode;
        e.color = c.color;
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
        if (r.failed()) {
            note(e.dxfType + " " + hex(self) + " is damaged and was skipped");
            return;
        }
        BitReader h(body, objBits);
        e.owner = leadingHandles(h, self, c.reactors, c.entmode == 0);
        if (!c.nolinks) {
            h.handle();  // previous entity
            h.handle();  // next entity
        }
        e.layer = resolveHandle(h.handle(), self);
        if (c.ltFlags == 3)
            e.linetype = resolveHandle(h.handle(), self);
        if (c.plotFlags == 3)
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
            e.firstChild = resolveHandle(h.handle(), self);
            e.lastChild = resolveHandle(h.handle(), self);
            h.handle();  // SEQEND
        }
        if (h.failed()) {
            note(e.dxfType + " " + hex(self) + " has damaged references and was skipped");
            return;
        }
        out_.entities.push_back(std::move(e));
    }

    static void add(EntityRec& e, int code, Value v) { e.groups.push_back({code, std::move(v)}); }

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
        const std::int32_t degree = r.bl();
        bool rational = false, closed = false, periodic = false;
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
                // The fit point block exists from R24 on, so an R2000 spline edge ends here.
                break;
            }
            default:
                r.fail();  // unknown edge type: the rest of the object cannot be read
                break;
        }
    }

    // DIMSTYLE variables in file order (ODA spec 20.4.68, R2000), each with its DXF group code.
    void dimStyleVars(BitReader& r, DimStyleRec& d) {
        auto bd = [&](int code) { d.groups.push_back({code, r.bd()}); };
        auto bs = [&](int code) { d.groups.push_back({code, r.bs()}); };
        auto bit = [&](int code) { d.groups.push_back({code, r.b() ? 1 : 0}); };
        auto tv = [&](int code) { d.groups.push_back({code, r.textBytes()}); };
        tv(3);
        tv(4);
        for (int code : {40, 41, 42, 43, 44, 45, 46, 47, 48})
            bd(code);
        for (int code : {71, 72, 73, 74, 75, 76})
            bit(code);
        for (int code : {77, 78, 79})
            bs(code);
        for (int code : {140, 141, 142, 143, 144, 145, 146, 147, 148})
            bd(code);
        bit(170);
        bs(171);
        for (int code : {172, 173, 174, 175})
            bit(code);
        for (int code : {176, 177, 178, 179, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280})
            bs(code);
        bit(281);
        bit(282);
        for (int code : {283, 284, 285, 286})
            bs(code);
        bit(288);
        bs(287);
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
        const double boxHeight = r.bd();
        const double boxWidth = r.bd();
        const bool hookOnX = r.b();
        const bool arrow = r.b();
        r.bs();
        r.b();
        r.b();
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
        const std::string tag = r.textBytes();
        r.bs();  // field length, unused
        const int flags = r.rc();
        std::string prompt;
        if (def)
            prompt = r.textBytes();
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
        const double height = r.bd();
        const int attach = r.bs();
        const int direction = r.bs();
        r.bd();  // extents height
        r.bd();  // extents width
        const std::string value = r.textBytes();
        const int spacingStyle = r.bs();
        const double spacingFactor = r.bd();
        r.b();  // unknown bit
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
                   std::vector<std::string>& notes) {
    ObjectReader reader(file, classes, out, notes);
    reader.run(map);
}

}  // namespace tbcad::dwg::detail
