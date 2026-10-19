#include "objects/decode.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

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
    kDictionary = 0x2A,
    // Class-based objects (type numbers from 500 up) are mapped to private numbers by DXF name.
    kHatch = 100000,
    kLayoutObject = 0x52,
    kSortEnts = 200000,
    kSpatialFilter = 400000,
    kMLeader = 300000,
    kViewport = 0x22,
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
        : file_(file), classes_(classes), out_(out), notes_(notes), fmt_(fmt) {
        if (!classes_.empty()) {
            int lo = classes_.front().number, hi = lo;
            for (const ClassInfo& c : classes_) {
                lo = std::min(lo, c.number);
                hi = std::max(hi, c.number);
            }
            std::string t = "classes: " + std::to_string(classes_.size()) + ", numbers " +
                            std::to_string(lo) + " to " + std::to_string(hi) + ", first:";
            for (std::size_t i = 0; i < classes_.size() && i < 6; ++i)
                t += " " + std::to_string(classes_[i].number) + "=" + classes_[i].dxfName;
            out_.walkNotes.push_back(t);
        }
    }

    // The layouts of layers, lines and inserts differ between files; each is decided by the first
    // object that fits one reading only. Objects that need a decision (block headers) can come
    // before that object in the stream, so decode ahead quietly and throw the result away.
    void calibrate() {
        const Drawing saved = out_;
        const auto savedFail = failDumps_;
        const auto savedMiss = missDumps_;
        const std::size_t notesBefore = notes_.size();
        const bool wasQuiet = quiet_;
        quiet_ = true;
        std::size_t seen = 0;
        for (const auto& [h, off] : walkedList_) {
            if (tripleVariant_ >= 0 && nolinksVariant_ >= 0 && insertCountKnown_)
                break;
            if (++seen > 40000)
                break;
            MapEntry e;
            e.handle = h;
            e.offset = off;
            decodeOne(e);
        }
        quiet_ = wasQuiet;
        out_ = saved;
        failDumps_ = savedFail;
        missDumps_ = savedMiss;
        notes_.resize(std::min(notes_.size(), notesBefore));
    }

    void run(const std::vector<MapEntry>& map) {
        // R2004 and later: the objects follow each other in the stream and carry their own
        // handle, so walking the stream is more dependable than the object map (whose entries
        // are written in a shape that is easy to misread). Use the map only as a fallback. A real
        // R2004 file had 2192 of 17669 map entries unusable while the walk found all 17669.
        if (fmt_.r2004) {
            const auto tAnchor = std::chrono::steady_clock::now();
            for (const MapEntry& e : map)
                if (e.offset > 0 && frameHandle(e.offset) == e.handle)
                    anchors_.push_back(e.offset);
            std::sort(anchors_.begin(), anchors_.end());
            out_.walkNotes.push_back(
                "stream anchors took " +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - tAnchor)
                                   .count()) +
                " ms");
            walked_.reserve(map.size() + map.size() / 8);
            walkedList_.reserve(map.size() + map.size() / 8);
            const auto tWalk = std::chrono::steady_clock::now();
            walkStream();
            out_.walkNotes.push_back(
                "stream walk took " +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - tWalk)
                                   .count()) +
                " ms");
            if (walkedList_.size() * 10 >= map.size() * 7) {
                const auto tCal = std::chrono::steady_clock::now();
                calibrate();
                const auto tDec = std::chrono::steady_clock::now();
                for (const auto& [h, off] : walkedList_) {
                    MapEntry e;
                    e.handle = h;
                    e.offset = off;
                    decodeOne(e);
                }
                const auto tEnd = std::chrono::steady_clock::now();
                auto ms = [](auto a, auto b) {
                    return std::to_string(
                        std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
                };
                out_.walkNotes.push_back("calibrate took " + ms(tCal, tDec) +
                                         " ms, walked decode " + ms(tDec, tEnd) + " ms");
                // Map entries the walk did not meet but whose frame carries their handle.
                for (const MapEntry& e : map)
                    if (!walked_.count(e.handle) && e.offset > 0 &&
                        frameHandle(e.offset) == e.handle)
                        decodeOne(e);
                std::sort(
                    out_.entities.begin(), out_.entities.end(),
                    [](const EntityRec& a, const EntityRec& b) { return a.handle < b.handle; });
                noteTypeTimes();
                return;
            }
        }
        for (const MapEntry& e : map) {
            if (e.offset < 0 || static_cast<std::size_t>(e.offset) >= file_.size()) {
                note("object " + hex(e.handle) + " lies outside the file");
                continue;
            }
            quiet_ = true;
            decodeOne(e);
            quiet_ = false;
            if (framed_)
                continue;
            // The frame at the place the map names is damaged: look for the object elsewhere. Its
            // own handle sits inside its frame, so the walk of the whole stream can find it.
            if (!walkedDone_)
                walkStream();
            std::int64_t found = 0;
            bool ok = false;
            if (const auto w = walked_.find(e.handle);
                w != walked_.end() && w->second != e.offset) {
                found = w->second - e.offset;
                ok = true;
            }
            // The search probes up to 16000 places per object. Neighbouring objects probe the
            // same places, so every place is looked at once (cached in blocks, only where a search
            // went). Only when the cache would pass its memory budget does the search back off to
            // every 64th object after 60 misses in a row.
            const bool cached = !cacheFull_;
            const bool search = cached || searchMisses_ < 60 || ++searchSkipped_ % 64 == 0;
            for (std::int64_t d = 1; !ok && search && d <= 8192; ++d) {
                for (std::int64_t sd : {d, -d}) {
                    if (frameHandleIs(e.offset + sd, e.handle, cached)) {
                        found = sd;
                        ok = true;
                        break;
                    }
                }
            }
            if (search)
                searchMisses_ = ok ? 0 : searchMisses_ + 1;
            if (ok) {
                ++out_.relocated[found];
                if (out_.relocatedSamples.size() < 24 &&
                    (out_.relocatedSamples.size() < 8 || out_.relocated.size() % 50 == 0))
                    out_.relocatedSamples.push_back({static_cast<std::int64_t>(e.handle),
                                                     static_cast<std::int64_t>(e.offset),
                                                     static_cast<std::int64_t>(e.offset) + found});
                MapEntry moved = e;
                moved.offset += found;
                decodeOne(moved);
                continue;
            }
            decodeOne(e);
        }
        std::sort(out_.entities.begin(), out_.entities.end(),
                  [](const EntityRec& a, const EntityRec& b) { return a.handle < b.handle; });
        noteTypeTimes();
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

    bool quiet_ = false;
    bool framed_ = true;
    // frameHandle per place, as the low 32 bits, in blocks allocated where the search goes.
    static constexpr std::size_t kProbeBlock = 4096;
    static constexpr std::size_t kProbeBudgetBlocks = 12288;  // 192 MB
    std::vector<std::unique_ptr<std::uint32_t[]>> probeBlocks_;
    std::size_t probeBlocksUsed_ = 0;
    bool cacheFull_ = false;
    bool frameHandleIs(std::int64_t offset, std::uint64_t handle, bool cached) {
        if (!cached || offset < 0 || static_cast<std::size_t>(offset) >= file_.size())
            return frameHandle(offset) == handle;
        if (probeBlocks_.empty())
            probeBlocks_.resize(file_.size() / kProbeBlock + 1);
        const auto at = static_cast<std::size_t>(offset);
        auto& block = probeBlocks_[at / kProbeBlock];
        if (!block) {
            if (probeBlocksUsed_ >= kProbeBudgetBlocks) {
                cacheFull_ = true;
                return frameHandle(offset) == handle;
            }
            block = std::make_unique<std::uint32_t[]>(kProbeBlock);
            std::fill_n(block.get(), kProbeBlock, 0xFFFFFFFFu);
            ++probeBlocksUsed_;
        }
        std::uint32_t& slot = block[at % kProbeBlock];
        if (slot == 0xFFFFFFFFu) {
            const std::uint64_t h = frameHandle(offset);
            slot = static_cast<std::uint32_t>(h);
            if (slot == 0xFFFFFFFFu)
                slot = 0;  // a handle that collides with the marker is treated as no frame
            return h == handle;
        }
        return slot != 0 && slot == static_cast<std::uint32_t>(handle) &&
               frameHandle(offset) == handle;
    }
    int searchSkipped_ = 0;
    int searchMisses_ = 0;  // objects in a row whose frame was not found near the map's place
    // A problem with the frame of an object: silent while the caller may still look elsewhere.
    void frameNote(const std::string& s) {
        framed_ = false;
        if (!quiet_)
            note(s);
    }

    void note(const std::string& s) {
        if (notes_.size() < kMaxNotes)
            notes_.push_back(s);
        else if (notes_.size() == kMaxNotes)
            notes_.push_back("further object problems are not listed");
    }

    std::string dxfNameOfClass(std::uint32_t type) const {
        if (type < 0x1F0)
            return {};
        for (const ClassInfo& c : classes_)
            if (c.number == static_cast<int>(type))
                return c.dxfName;
        if (type < 500)
            return {};
        const std::size_t idx = type - 500;
        return idx < classes_.size() ? classes_[idx].dxfName : std::string{};
    }

    std::size_t mainEnd_ = 0;
    int hugeSeen_ = 0;
    std::map<std::uint32_t, int> failDumps_;
    std::uint64_t currentHandle_ = 0;
    Bytes currentBody_;
    std::size_t currentBits_ = 0;
    // All data bits of the object being decoded (diagnostic for values that look wrong).
    std::string dumpCurrent() const {
        std::string bits;
        BitReader t(currentBody_);
        for (std::size_t i = 0; i < currentBits_ && i < 3000; ++i)
            bits += t.b() ? '1' : '0';
        return bits;
    }

    // The own handle of the object framed at `offset` (0 when nothing sensible starts there).
    std::uint64_t frameHandle(std::int64_t offset) const {
        if (offset < 0 || static_cast<std::size_t>(offset) >= file_.size())
            return 0;
        BitReader head(file_.subspan(static_cast<std::size_t>(offset)));
        const std::uint32_t size = head.ms();
        std::size_t handleBits = 0;
        if (fmt_.r2010)
            handleBits = static_cast<std::size_t>(head.mcUnsigned());
        const std::size_t lead = head.bytePos();
        if (head.failed() || size == 0 ||
            static_cast<std::size_t>(offset) + lead + size > file_.size())
            return 0;
        if (handleBits > static_cast<std::size_t>(size) * 8)
            return 0;
        BitReader r(file_.subspan(static_cast<std::size_t>(offset) + lead, size));
        if (fmt_.r2010)
            r.objectTypeR2010();
        else {
            r.bs();
            r.rl();
        }
        const HandleRef self = r.handle();
        return r.failed() ? 0 : self.value;
    }

    // Size in bytes of the object framed at `offset` including the leading size fields (0 when
    // nothing sensible starts there).
    std::size_t frameLength(std::int64_t offset) const {
        if (offset < 0 || static_cast<std::size_t>(offset) >= file_.size())
            return 0;
        BitReader head(file_.subspan(static_cast<std::size_t>(offset)));
        const std::uint32_t size = head.ms();
        if (fmt_.r2010)
            head.mcUnsigned();
        const std::size_t lead = head.bytePos();
        if (head.failed() || size == 0)
            return 0;
        return lead + size;
    }

    // Objects follow each other in the stream (size, body, a 2 byte check value): walking it from
    // the start gives every object's real position, whatever the object map says.
    std::unordered_map<std::uint64_t, std::int64_t> walked_;
    std::vector<std::pair<std::uint64_t, std::int64_t>> walkedList_;
    bool walkedDone_ = false;
    std::vector<std::int64_t> anchors_;  // map offsets whose frame carries the mapped handle
    void walkStream() {
        walkedDone_ = true;
        std::int64_t off = 4;
        std::size_t resyncs = 0;
        struct Summary {
            ObjectReader* self;
            std::int64_t* off;
            std::size_t* resyncs;
            ~Summary() {
                self->out_.walkSummary = "stream walk: " + std::to_string(self->walked_.size()) +
                                         " objects, " + std::to_string(*resyncs) +
                                         " resyncs, stopped at byte " + std::to_string(*off) +
                                         " of " + std::to_string(self->file_.size());
            }
        } summary{this, &off, &resyncs};
        std::size_t misses = 0;
        std::size_t crcOk = 0, crcBad = 0;
        std::vector<std::int64_t> badAt;
        std::array<std::size_t, 6> crcVariant{};
        struct CrcNote {
            ObjectReader* self;
            std::size_t* ok;
            std::size_t* bad;
            std::vector<std::int64_t>* at;
            std::array<std::size_t, 6>* variants;
            ~CrcNote() {
                std::string t =
                    "stream walk object check values (CRC from the size field to the end of the "
                    "data): " +
                    std::to_string(*ok) + " match, " + std::to_string(*bad) +
                    " do not; first mismatches at";
                t += " [whole/C0C1 " + std::to_string((*variants)[0]) + ", whole/0 " +
                     std::to_string((*variants)[1]) + ", whole/FFFF " +
                     std::to_string((*variants)[2]) + ", body/C0C1 " +
                     std::to_string((*variants)[3]) + ", body/0 " + std::to_string((*variants)[4]) +
                     ", body/FFFF " + std::to_string((*variants)[5]) + "]";
                for (std::size_t i = 0; i < at->size() && i < 12; ++i)
                    t += " " + std::to_string((*at)[i]);
                self->out_.walkNotes.push_back(t);
            }
        } crcNote{this, &crcOk, &crcBad, &badAt, &crcVariant};
        const std::int64_t end = static_cast<std::int64_t>(file_.size());
        while (off + 8 < end) {
            const std::uint64_t h = frameHandle(off);
            const std::size_t len = frameLength(off);
            if (h != 0 && len != 0 && off + static_cast<std::int64_t>(len) + 2 <= end) {
                if (crcOk + crcBad < 4000) {  // a sample: six passes over every object are slow
                    const auto whole = file_.subspan(static_cast<std::size_t>(off), len);
                    BitReader head(whole);
                    head.ms();
                    if (fmt_.r2010)
                        head.mcUnsigned();
                    const std::size_t lead = head.bytePos();
                    const std::size_t at = static_cast<std::size_t>(off) + len;
                    const auto want = static_cast<std::uint16_t>(file_[at] | (file_[at + 1] << 8));
                    const auto wantBig =
                        static_cast<std::uint16_t>(file_[at + 1] | (file_[at] << 8));
                    bool any = false;
                    int k = 0;
                    for (const std::size_t skip : {std::size_t{0}, lead}) {
                        for (const std::uint16_t seed :
                             {std::uint16_t{0xC0C1}, std::uint16_t{0}, std::uint16_t{0xFFFF}}) {
                            const std::uint16_t c = crc16(seed, whole.subspan(skip));
                            if (c == want || c == wantBig) {
                                ++crcVariant[static_cast<std::size_t>(k)];
                                any = true;
                            }
                            ++k;
                        }
                    }
                    if (any)
                        ++crcOk;
                    else {
                        ++crcBad;
                        badAt.push_back(off);
                    }
                }
                walked_.emplace(h, off);
                walkedList_.emplace_back(h, off);
                off += static_cast<std::int64_t>(len) + 2;
                misses = 0;
                continue;
            }
            // Lost the frame: move on byte by byte until two frames in a row look right, or to the
            // next object the map places correctly, whichever comes first.
            bool again = false;
            std::int64_t scan = 0;
            for (std::int64_t d = 1; d <= 4096 && off + d + 8 < end; ++d) {
                const std::size_t l1 = frameLength(off + d);
                if (l1 != 0 && frameHandle(off + d) != 0 &&
                    frameHandle(off + d + static_cast<std::int64_t>(l1) + 2) != 0) {
                    scan = d;
                    break;
                }
            }
            const auto anchor = std::upper_bound(anchors_.begin(), anchors_.end(), off);
            const std::int64_t viaMap = anchor == anchors_.end() ? 0 : *anchor - off;
            const std::int64_t step = scan != 0 && (viaMap == 0 || scan <= viaMap) ? scan : viaMap;
            if (step > 0) {
                if (resyncs < 200)
                    out_.walkNotes.push_back(
                        "stream walk lost its place at " + std::to_string(off) + " (page offset " +
                        std::to_string(off % 0x7400) + "), skipped " + std::to_string(step) +
                        " bytes by " + (step == scan ? "scanning" : "the object map") + ", scan " +
                        std::to_string(scan) + ", map " + std::to_string(viaMap) +
                        ", previous frame " +
                        (walkedList_.empty()
                             ? std::string("none")
                             : hex(walkedList_.back().first) + " at " +
                                   std::to_string(walkedList_.back().second) + " ends at " +
                                   std::to_string(off) + " " + dumpBytes(off - 6, 6)) +
                        ", bytes from there " +
                        dumpBytes(off, static_cast<std::size_t>(step) + 12));
                off += step;
                ++resyncs;
                again = true;
            }
            if (!again || ++misses > 100000)
                break;
        }
    }

    std::string dumpBytes(std::int64_t from, std::size_t n) const {
        static const char* d = "0123456789ABCDEF";
        std::string w;
        for (std::size_t i = 0; i < n; ++i) {
            const std::int64_t at = from + static_cast<std::int64_t>(i);
            if (at < 0 || static_cast<std::size_t>(at) >= file_.size())
                break;
            const auto b = static_cast<unsigned char>(file_[static_cast<std::size_t>(at)]);
            w += d[b >> 4];
            w += d[b & 15];
            w += ' ';
        }
        return "[" + w + "]";
    }

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

    // Time spent decoding, per object type (diagnostic: which kind is slow).
    std::map<std::uint32_t, std::pair<std::size_t, double>> typeTime_;
    std::uint32_t curType_ = 0xFFFF;
    struct TypeTimer {
        ObjectReader& self;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        explicit TypeTimer(ObjectReader& s) : self(s) { self.curType_ = 0xFFFF; }
        ~TypeTimer() {
            auto& row = self.typeTime_[self.curType_];
            ++row.first;
            row.second +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
        }
    };

public:
    void noteTypeTimes() {
        std::vector<std::pair<double, std::uint32_t>> order;
        for (const auto& [type, row] : typeTime_)
            order.emplace_back(row.second, type);
        std::sort(order.rbegin(), order.rend());
        std::string t = "decode time per object type (type: count, ms), slowest first:";
        for (std::size_t i = 0; i < order.size() && i < 10; ++i) {
            const auto& row = typeTime_[order[i].second];
            char b[96];
            std::snprintf(b, sizeof b, " %u: %zu, %.0f ms;", order[i].second, row.first,
                          row.second);
            t += b;
        }
        out_.walkNotes.push_back(t);
    }

private:
    void decodeOne(const MapEntry& e) {
        TypeTimer timer(*this);
        framed_ = true;
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
            frameNote("object " + hex(e.handle) + " has an impossible size" + where(e));
            return;
        }
        Bytes body = file_.subspan(static_cast<std::size_t>(e.offset) + lead, size);
        BitReader r(body);
        std::uint32_t type = fmt_.r2010 ? r.objectTypeR2010() : static_cast<std::uint32_t>(r.bs());
        out_.rawType[e.handle] = type;
        curType_ = type;
        if (type == 0x4E)
            type = kHatch;  // HATCH has a fixed number in files that list it among the fixed types
        if (type >= 500) {
            const std::string cls = dxfNameOfClass(type);
            if (cls == "LWPOLYLINE")
                type = kLwPolyline;
            else if (cls == "HATCH")
                type = kHatch;
            else if (cls == "MULTILEADER" || cls == "MLEADER")
                type = kMLeader;
            else if (cls == "SORTENTSTABLE")
                type = kSortEnts;
            else if (cls == "SPATIAL_FILTER")
                type = kSpatialFilter;
            else if (cls == "LAYOUT") {
                type = kLayoutObject;
                ++out_.layoutSeen;
            }
        }
        std::size_t objBits = 0;
        if (fmt_.r2010) {
            if (handleBits > body.size() * 8) {
                frameNote("object " + hex(e.handle) + " has a damaged header" + where(e));
                return;
            }
            objBits = body.size() * 8 - handleBits;
        } else {
            objBits = r.rl();
        }
        const HandleRef self = r.handle();
        if (r.failed() || objBits > body.size() * 8) {
            frameNote("object " + hex(e.handle) + " has a damaged header" + where(e));
            return;
        }
        // R2007 and later: strings live in their own stream just before the handle stream.
        BitReader strings;
        if (fmt_.r2007 && !locateStringStream(body, objBits, strings)) {
            frameNote("object " + hex(e.handle) + " has a damaged string stream" + where(e));
            return;
        }
        currentHandle_ = self.value;
        currentBody_ = body;
        currentBits_ = objBits;
        // The data of an R2004 object ends exactly at the stated size (checked against a real
        // file); later releases and R2000 leave one bit.
        const bool exactEnd = fmt_.r2004 && !fmt_.r2007;
        mainEnd_ = strings.bitSize() != 0 ? strings.bitPos()
                                          : (objBits > 0 ? objBits - (exactEnd ? 0 : 1) : 0);
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
            case kViewport:
            case kLeader:
            case kMLeader:
            case kDimOrdinate:
            case kDimLinear:
            case kDimAligned:
            case kDimAngular3:
            case kDimAngular2:
            case kDimRadius:
            case kDimDiameter:
                decodeEntity(type, r, objBits, body, self.value);
                return;
            case kLayoutObject:
                decodeLayoutObject(r, objBits, body, self.value);
                return;
            case kDictionary:
                decodeDictionary(r, objBits, body, self.value);
                return;
            case kSortEnts:
                decodeSortEnts(r, objBits, body, self.value);
                return;
            case kSpatialFilter:
                decodeSpatialFilter(r, self.value);
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
        if (r.bitPos() != mainEnd_ && !quiet_ && missDumps_[type]++ < 2)
            note("layout of type " + std::to_string(type) + " object " + hex(currentHandle_) +
                 " ended at bit " + std::to_string(r.bitPos()) + ", expected " +
                 std::to_string(mainEnd_) + ", bits " + dumpCurrent());
    }
    std::map<std::uint32_t, int> missDumps_;
    int spatialNotes_ = 0;

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
        long long transparency =
            0;              // DXF 440 value: 0 by layer, 0x01000000 by block, 0x02000000 + alpha
        std::string raw{};  // index, RGB word and flag byte as read, for the trace
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
        {
            char buf[48];
            std::snprintf(buf, sizeof buf, "bs %d word %08X flags %u", idx, word, flags);
            c.raw = buf;
        }
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
            c.transparency = r.bl();
        return c;
    }

    // Extended data of the object being read; kept so a dimension can pick out its style
    // overrides (the "DSTYLE" block of the standard application).
    std::vector<std::vector<std::uint8_t>> eed_{};

    void skipEed(BitReader& r) {
        eed_.clear();
        for (int guard = 0; guard < 100000; ++guard) {
            const std::int32_t size = r.bs();
            if (size <= 0 || r.failed())
                return;
            r.handle();
            std::vector<std::uint8_t> bytes;
            for (std::int32_t i = 0; i < size && !r.failed(); ++i) {
                const auto b = static_cast<std::uint8_t>(r.rc());
                if (bytes.size() < 4096)
                    bytes.push_back(b);
            }
            if (!r.failed() && eed_.size() < 16)
                eed_.push_back(std::move(bytes));
        }
    }

    // The description of a layer is the second string of its AcAecLayerStandard extended data:
    // a block of exactly two strings (code 0 each). The application handle is not kept, so the
    // shape of the block is what recognises it.
    bool layerDescription(const std::vector<std::uint8_t>& d, std::string& out) const {
        std::size_t p = 0;
        std::string found[2];
        for (std::string& dest : found) {
            if (p >= d.size() || d[p] != 0)
                return false;
            ++p;
            if (fmt_.r2007) {
                if (p + 2 > d.size())
                    return false;
                const std::size_t n = d[p] | (d[p + 1] << 8);
                p += 2;
                if (p + n * 2 > d.size())
                    return false;
                for (std::size_t i = 0; i < n; ++i) {
                    unsigned ch = d[p + 2 * i] | (d[p + 2 * i + 1] << 8);
                    if (ch >= 0xD800 && ch < 0xDC00 && i + 1 < n) {
                        const unsigned lo = d[p + 2 * i + 2] | (d[p + 2 * i + 3] << 8);
                        if (lo >= 0xDC00 && lo < 0xE000) {
                            ch = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00);
                            ++i;
                        }
                    }
                    if (ch < 0x80) {
                        dest.push_back(static_cast<char>(ch));
                    } else if (ch < 0x800) {
                        dest.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                        dest.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                    } else if (ch < 0x10000) {
                        dest.push_back(static_cast<char>(0xE0 | (ch >> 12)));
                        dest.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                        dest.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                    } else {
                        dest.push_back(static_cast<char>(0xF0 | (ch >> 18)));
                        dest.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
                        dest.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                        dest.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                    }
                }
                p += n * 2;
            } else {
                if (p + 3 > d.size())
                    return false;
                const std::size_t n = d[p];
                p += 3;  // length and code page
                if (p + n > d.size())
                    return false;
                for (std::size_t i = 0; i < n; ++i)
                    dest.push_back(d[p + i] < 128 ? static_cast<char>(d[p + i]) : '?');
                p += n;
            }
        }
        if (p != d.size())
            return false;
        out = found[1];
        return true;
    }

    // Reads the style overrides of a dimension from its extended data (ODA spec section 28:
    // string "DSTYLE", then braces holding pairs of a variable number and its value) and adds
    // them to the entity as standard extended-data groups.
    void dimStyleOverrides(EntityRec& e) {
        for (const std::vector<std::uint8_t>& d : eed_) {
            std::size_t p = 0;
            auto have = [&](std::size_t n) { return p + n <= d.size(); };
            auto str = [&](std::string& out) {
                out.clear();
                if (fmt_.r2007) {
                    if (!have(2))
                        return false;
                    const std::size_t n = d[p] | (d[p + 1] << 8);
                    p += 2;
                    if (!have(n * 2))
                        return false;
                    for (std::size_t i = 0; i < n; ++i) {
                        const unsigned ch = d[p + 2 * i] | (d[p + 2 * i + 1] << 8);
                        out.push_back(ch < 128 ? static_cast<char>(ch) : '?');
                    }
                    p += n * 2;
                } else {
                    if (!have(3))
                        return false;
                    const std::size_t n = d[p];
                    p += 3;  // length and code page
                    if (!have(n))
                        return false;
                    out.assign(reinterpret_cast<const char*>(&d[p]), n);
                    p += n;
                }
                return true;
            };
            auto u16 = [&](long long& v) {
                if (!have(2))
                    return false;
                v = static_cast<std::int16_t>(d[p] | (d[p + 1] << 8));
                p += 2;
                return true;
            };
            if (!have(1) || d[p] != 0)
                continue;
            ++p;
            std::string name;
            if (!str(name) || name != "DSTYLE")
                continue;
            std::vector<Group> found;
            bool ok = true;
            while (ok && have(1)) {
                const int type = d[p++];
                if (type == 2) {  // brace
                    if (!have(1))
                        break;
                    ++p;
                    continue;
                }
                if (type != 70) {
                    ok = false;
                    break;
                }
                long long var = 0;
                if (!u16(var) || !have(1)) {
                    ok = false;
                    break;
                }
                const int vt = d[p++];
                Group g;
                if (vt == 40 || vt == 41 || vt == 42) {
                    if (!have(8)) {
                        ok = false;
                        break;
                    }
                    double v;
                    std::memcpy(&v, &d[p], 8);
                    p += 8;
                    g.value.v.emplace<double>(v);
                } else if (vt == 70) {
                    long long v = 0;
                    if (!u16(v)) {
                        ok = false;
                        break;
                    }
                    g.value.v.emplace<long long>(v);
                } else if (vt == 71) {
                    if (!have(4)) {
                        ok = false;
                        break;
                    }
                    std::int32_t v;
                    std::memcpy(&v, &d[p], 4);
                    p += 4;
                    g.value.v.emplace<long long>(v);
                } else if (vt == 5 || vt == 3) {
                    if (!have(8)) {
                        ok = false;
                        break;
                    }
                    p += 8;  // handles (text style, arrow blocks): not used
                    continue;
                } else {
                    ok = false;
                    break;
                }
                g.code = 1070;
                found.push_back(std::move(g));
                Group k;
                k.code = vt >= 70 ? 1000 + vt : 1040;
                k.value = found.back().value;
                found.back().value.v.emplace<long long>(var);
                found.push_back(std::move(k));
            }
            if (found.empty())
                continue;
            add(e, 1001, Value{std::string("ACAD")});
            add(e, 1000, Value{std::string("DSTYLE")});
            add(e, 1002, Value{std::string("{")});
            for (Group& g : found)
                e.groups.push_back(std::move(g));
            add(e, 1002, Value{std::string("}")});
            return;
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
        lastXdic_ = 0;
        if (!xdicMissing_)
            lastXdic_ = resolveHandle(h.handle(), self);  // xdictionary (always stored up to R2000)
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
                    // Only a layer that fits one reading and not the other decides it.
                    bool fits[2] = {false, false};
                    for (int v = 0; v < 2; ++v) {
                        BitReader probe = r;
                        tripleVariant_ = v;
                        bool f = false, x = false;
                        readTriple(probe, f, x);
                        probe.bs();
                        readCmc(probe, 7);
                        fits[v] = !probe.failed() && probe.bitPos() == mainEnd_;
                    }
                    tripleVariant_ = fits[0] != fits[1] ? (fits[0] ? 0 : 1) : -1;
                }
                l.name = r.textBytes();
                bool flag64 = false, xdep = false;
                readTriple(r, flag64, xdep);
                const int values = r.bs();
                const ColorValue color = readCmc(r, 7);
                l.xdic = 0;
                leadingHandles(h, self, reactors, true);
                l.xdic = lastXdic_;
                for (const std::vector<std::uint8_t>& d : eed_) {
                    // The layer's transparency is a block of one 32-bit integer (code 0x47).
                    // Other applications store plain integers the same way; a transparency
                    // is 0x01000000 (by block) or 0x02000000 plus the alpha byte.
                    if (d.size() == 5 && d[0] == 0x47 && (d[4] == 0x01 || d[4] == 0x02))
                        l.transparency =
                            static_cast<long long>(static_cast<std::uint32_t>(d[1]) |
                                                   (static_cast<std::uint32_t>(d[2]) << 8) |
                                                   (static_cast<std::uint32_t>(d[3]) << 16) |
                                                   (static_cast<std::uint32_t>(d[4]) << 24));
                    if (std::string desc; l.description.empty() && layerDescription(d, desc))
                        l.description = desc;
                    char hx[4];
                    for (std::size_t i = 0; i < d.size() && i < 400; ++i) {
                        std::snprintf(hx, sizeof hx, "%02X", d[i]);
                        l.eed += hx;
                    }
                    l.eed += ' ';
                }
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
                // Bit 1 of the layer flags is "off"; the DXF way to say it is a negative colour.
                if ((values & 2) != 0)
                    l.color = -(l.color > 0 && l.color < 256 ? l.color : 7);
                l.trueColor = color.rgb;
                l.colorRaw = color.raw;
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
                leadingHandles(h, self, reactors, true);
                h.handle();  // external reference block
                const HandleRef textStyle = h.handle();
                if (!h.failed())
                    d.textStyle = resolveHandle(textStyle, self);
                const HandleRef leaderArrow = h.handle();
                const HandleRef arrow = h.handle();
                const HandleRef arrow1 = h.handle();
                const HandleRef arrow2 = h.handle();
                if (!h.failed()) {
                    d.leaderArrow = resolveHandle(leaderArrow, self);
                    d.arrow = resolveHandle(arrow, self);
                    d.arrow1 = resolveHandle(arrow1, self);
                    d.arrow2 = resolveHandle(arrow2, self);
                }
                d.flags = (flag64 ? 64 : 0) | (xdep ? 16 : 0) | (unknown ? 1 : 0);
                out_.dimStyles[self] = std::move(d);
                break;
            }
            case kBlockHeader: {
                BlockRec b;
                b.handle = self;
                bool anonymous = false, hasAtts = false, isXref = false, overlay = false;
                bool f64 = false, xdep = false;
                std::uint32_t ownedCount = 0, insertCount = 0;
                auto blockBody = [&](BitReader& rr, BlockRec& bb) {
                    bb.name = rr.textBytes();
                    readTriple(rr, f64, xdep);
                    anonymous = rr.b();
                    hasAtts = rr.b();
                    isXref = rr.b();
                    overlay = rr.b();
                    rr.b();  // loaded bit
                    // Owned object count; an external reference owns no objects and the files
                    // do not store the count for it.
                    ownedCount = 0;
                    if (fmt_.r2004 && !isXref && !overlay)
                        ownedCount = rr.bl();
                    bb.baseX = rr.bd();
                    bb.baseY = rr.bd();
                    bb.baseZ = rr.bd();
                    bb.xrefPath = rr.textBytes();
                    if (fmt_.r2007) {
                        // Insert count (non-zero bytes ending in a zero), description, preview,
                        // units.
                        insertCount = 0;
                        for (int i = 0; i < 100000 && !rr.failed() && rr.rc() != 0; ++i)
                            ++insertCount;
                        rr.textBytes();
                        const std::uint32_t preview = rr.bl();
                        if (preview > rr.bitsLeft() / 8)
                            rr.fail();
                        else
                            rr.seekBit(rr.bitPos() + static_cast<std::size_t>(preview) * 8);
                        rr.bs();  // insert units
                        rr.b();   // explodable
                        rr.rc();  // block scaling
                    }
                };
                if (fmt_.r2007 && tripleVariant_ < 0) {
                    // Layers did not decide the table record layout; a block header may.
                    bool fits[2] = {false, false};
                    for (int v = 0; v < 2; ++v) {
                        BitReader probe = r;
                        BlockRec pb;
                        tripleVariant_ = v;
                        blockBody(probe, pb);
                        fits[v] = !probe.failed() && probe.bitPos() == mainEnd_;
                    }
                    tripleVariant_ = fits[0] != fits[1] ? (fits[0] ? 0 : 1) : -1;
                }
                blockBody(r, b);
                if (r.failed()) {
                    note("block header " + hex(self) + " is damaged (read to bit " +
                         std::to_string(r.bitPos()) + " of " + std::to_string(mainEnd_) + ")" +
                         (failDumps_[type]++ < 2 ? ", bits " + dumpCurrent() : std::string()));
                    return;
                }
                b.flags = (anonymous ? 1 : 0) | (hasAtts ? 2 : 0) | (isXref ? 4 : 0) |
                          (overlay ? 8 : 0) | (xdep ? 16 : 0);
                // The handle stream names the objects the block owns: the authoritative member
                // list (an entity's own owner handle is the fallback).
                leadingHandles(h, self, reactors, true);
                h.handle();  // null hard pointer
                h.handle();  // BLOCK entity
                if (fmt_.r2004 && ownedCount < 20000000) {
                    for (std::uint32_t i = 0; i < ownedCount && !h.failed(); ++i)
                        b.members.push_back(resolveHandle(h.handle(), self));
                    b.ownedCount = ownedCount;
                    b.membersRead = !h.failed();
                }
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
        long long transparency = 0;
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
        if (nolinksVariant_ != 0 || !fmt_.r2004)
            c.nolinks = r.b();
        if (fmt_.r2004) {
            const ColorValue cv = readEnc(r);
            c.color = cv.index;
            c.trueColor = cv.rgb;
            c.transparency = cv.transparency;
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
        if (fmt_.r2004 && type == kLine && nolinksVariant_ < 0) {
            bool fits[2] = {false, false};
            for (int v = 0; v < 2; ++v) {
                BitReader probe = r;
                nolinksVariant_ = v == 0 ? 1 : 0;
                CommonBits pc;
                EntityRec pe;
                if (readCommonEntity(probe, pc)) {
                    line(probe, pe);
                    fits[v] = !probe.failed() && probe.bitPos() == mainEnd_;
                }
            }
            nolinksVariant_ = fits[0] != fits[1] ? (fits[0] ? 1 : 0) : -1;
        }
        if (fmt_.r2004 && (type == kInsert) && !insertCountKnown_) {
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
        e.transparency = c.transparency;
        e.ltScale = c.ltScale;
        e.invisible = (c.invisible & 1) != 0;
        e.lineweight = c.lineweight;
        e.ltFlags = c.ltFlags;
        bool wantStyle = false;
        bool wantBlock = false;
        bool wantChildren = false;
        bool wantDim = false;
        bool wantLeader = false;
        bool wantViewport = false;
        switch (type) {
            case kViewport:
                viewport(r, e);
                wantViewport = true;
                break;
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
            case kMLeader:
                mleader(r, e);
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
            ++out_.entityFailures[e.dxfType][0];
            note(e.dxfType + " " + hex(self) + " is damaged and was skipped (read to bit " +
                 std::to_string(r.bitPos()) + " of " + std::to_string(mainEnd_) + ")" +
                 (failDumps_[type]++ < 2 ? ", bits " + dumpCurrent() : std::string()));
            return;
        }
        for (const Group& g : e.groups) {
            if (const double* v = std::get_if<double>(&g.value.v)) {
                if (!std::isfinite(*v) || std::abs(*v) > 1e13) {
                    ++out_.entityFailures[e.dxfType][1];
                    note(e.dxfType + " " + hex(self) +
                         " has a number out of range and was skipped");
                    return;
                }
            }
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
        e.xdic = lastXdic_;
        if (wantViewport) {
            e.frozenCount = viewportFrozen_;
            for (std::uint32_t i = 0; i < viewportFrozen_ && i < 100000 && !h.failed(); ++i)
                e.frozenLayers.push_back(resolveHandle(h.handle(), self));
        }
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

    // VIEWPORT (ODA spec 20.4.38): a window on a layout sheet showing model space.
    std::uint32_t viewportFrozen_ = 0;
    void viewport(BitReader& r, EntityRec& e) {
        e.dxfType = "VIEWPORT";
        const Vec3 center = bd3(r);
        const double width = r.bd();
        const double height = r.bd();
        const Vec3 target = bd3(r);
        const Vec3 viewDir = bd3(r);
        const double twist = r.bd();
        const double viewHeight = r.bd();
        r.bd();  // lens length
        r.bd();  // front clip
        r.bd();  // back clip
        r.bd();  // snap angle
        const double vcx = r.rd();
        const double vcy = r.rd();
        for (int i = 0; i < 6; ++i)
            r.rd();  // snap base, snap spacing, grid spacing
        r.bs();      // circle zoom
        if (fmt_.r2007)
            r.bs();  // grid major
        viewportFrozen_ = r.bl();
        const std::uint32_t flags = r.bl();
        r.textBytes();  // style sheet
        r.rc();         // render mode
        r.b();          // UCS at origin
        r.b();          // UCS per viewport
        bd3(r);
        bd3(r);
        bd3(r);  // UCS origin and axes
        r.bd();  // elevation
        r.bs();  // ortho view type
        if (fmt_.r2004)
            r.bs();  // shade plot mode
        if (fmt_.r2007) {
            r.b();   // use default lights
            r.rc();  // default lighting type
            r.bd();  // brightness
            r.bd();  // contrast
            readCmc(r, 7);
        }
        point3(e, 10, center);
        add(e, 40, width);
        add(e, 41, height);
        point3(e, 17, target);
        point3(e, 16, viewDir);
        if (twist != 0)
            add(e, 51, twist);
        add(e, 12, vcx);
        add(e, 22, vcy);
        add(e, 45, viewHeight);
        // The status flags did not match the "off" bit on a real file (every viewport of the
        // sheets read as off), so every viewport is treated as on.
        add(e, 90, static_cast<long long>(flags));
        add(e, 68, 1);
        // A sheet's own overall viewport (id 1) looks at nothing of model space: its view is the
        // paper itself.
        if (std::abs(target.x) < 1e-9 && std::abs(target.y) < 1e-9 &&
            std::abs(vcx - center.x) < 1e-6 && std::abs(vcy - center.y) < 1e-6)
            add(e, 69, 1);
    }

    // DICTIONARY (ODA spec 20.4.44): name and handle pairs; the layout dictionary names the
    // layouts.
    void decodeDictionary(BitReader& r, std::size_t objBits, Bytes body, std::uint64_t self) {
        std::uint32_t reactors = 0;
        commonObjectPrefix(r, reactors);
        // The item count is a raw long in the description; which form a real file uses is decided
        // by where the main data must end (the names live in the string stream).
        std::uint32_t n = 0;
        bool done = false;
        for (int form = 0; form < 2 && !done; ++form) {
            BitReader probe = r;
            probe.setStringStream(nullptr);
            n = form == 0 ? probe.rl() : probe.bl();
            probe.bs();  // cloning flag
            probe.rc();  // hard owner flag
            if (!probe.failed() && (!fmt_.r2007 || probe.bitPos() == mainEnd_)) {
                r.seekBit(probe.bitPos());
                done = true;
            }
        }
        if (!done || n > 100000) {
            note("dictionary " + hex(self) + " has an unexpected layout" +
                 (failDumps_[kDictionary]++ < 2 ? ", bits " + dumpCurrent() : std::string()));
            return;
        }
        std::vector<std::string> names;
        for (std::uint32_t i = 0; i < n && !r.failed(); ++i)
            names.push_back(r.textBytes());
        tally(kDictionary, r);
        BitReader h(body, objBits);
        leadingHandles(h, self, reactors, true);
        std::vector<std::pair<std::string, std::uint64_t>> items;
        for (std::size_t i = 0; i < names.size() && !h.failed(); ++i)
            items.emplace_back(names[i], resolveHandle(h.handle(), self));
        if (!r.failed() && !h.failed())
            out_.dictionaries[self] = std::move(items);
    }

    // SPATIAL_FILTER (the clip of an XCLIP): points, the plane, the display and front/back clip
    // switches and the two transforms (ODA spec, class-based object).
    void decodeSpatialFilter(BitReader& r, std::uint64_t self) {
        std::uint32_t reactors = 0;
        commonObjectPrefix(r, reactors);
        SpatialFilterRec f;
        const std::int32_t n = r.bs();
        if (n < 0 || n > 10000) {
            note("spatial filter " + hex(self) + " has " + std::to_string(n) + " points");
            return;
        }
        for (std::int32_t i = 0; i < n && !r.failed(); ++i) {
            const double x = r.rd();
            const double y = r.rd();
            f.pts.push_back({x, y});
        }
        const Vec3 ex = bd3(r);
        const Vec3 org = bd3(r);
        f.extrusion[0] = ex.x, f.extrusion[1] = ex.y, f.extrusion[2] = ex.z;
        f.origin[0] = org.x, f.origin[1] = org.y, f.origin[2] = org.z;
        f.displayBoundary = r.bs() != 0;
        f.frontOn = r.bs() != 0;
        if (f.frontOn)
            f.frontDist = r.bd();
        f.backOn = r.bs() != 0;
        if (f.backOn)
            f.backDist = r.bd();
        for (double& v : f.inverse)
            v = r.bd();
        for (double& v : f.clipTransform)
            v = r.bd();
        tally(kSpatialFilter, r);
        if (r.failed()) {
            note("spatial filter " + hex(self) + " has an unexpected layout");
            return;
        }
        if (spatialNotes_++ < 3) {
            std::string t = "spatial filter " + hex(self) + ": " + std::to_string(f.pts.size()) +
                            " points, inverse";
            for (double v : f.inverse)
                t += " " + std::to_string(v);
            t += ", transform";
            for (double v : f.clipTransform)
                t += " " + std::to_string(v);
            note(t);
        }
        out_.spatialFilters[self] = std::move(f);
    }

    // SORTENTSTABLE: the draw order of a block. The main data is a count; the handle stream holds
    // the block owner and then pairs of (sort handle, entity handle). Entities sort by the sort
    // handle, those not listed by their own handle.
    void decodeSortEnts(BitReader& r, std::size_t objBits, Bytes body, std::uint64_t self) {
        std::uint32_t reactors = 0;
        commonObjectPrefix(r, reactors);
        const std::uint32_t n = r.bl();
        tally(kSortEnts, r);
        BitReader h(body, objBits);
        leadingHandles(h, self, reactors, true);
        std::vector<std::uint64_t> rest;
        while (!h.failed() && h.bitsLeft() >= 8 && rest.size() < 2000000)
            rest.push_back(resolveHandle(h.handle(), self));
        // Trailing padding can read as a few null handles.
        while (!rest.empty() && rest.back() == 0)
            rest.pop_back();
        const bool okLayout = !r.failed() && n <= 2000000 &&
                              (!fmt_.r2007 || r.bitPos() == mainEnd_) && rest.size() >= 2ull * n;
        if (!okLayout) {
            note("sort table " + hex(self) + " has an unexpected layout (" + std::to_string(n) +
                 " entries, " + std::to_string(rest.size()) + " handles)" +
                 (failDumps_[kSortEnts]++ < 2 ? ", bits " + dumpCurrent() : std::string()));
            return;
        }
        // The block owner comes first when one handle is left over after the pairs.
        const std::size_t lead = rest.size() - 2ull * n;
        const std::uint64_t block = lead >= 1 ? rest[lead - 1] : 0;
        if (block == 0)
            return;
        auto& keys = out_.sortKeys[block];
        for (std::uint32_t i = 0; i < n; ++i)
            keys[rest[lead + 2ull * i + 1]] = rest[lead + 2ull * i];
    }

    // LAYOUT object (ODA spec 20.4.84): the name, paper size and paper space block of a layout.
    void decodeLayoutObject(BitReader& r, std::size_t objBits, Bytes body, std::uint64_t self) {
        std::uint32_t reactors = 0;
        commonObjectPrefix(r, reactors);
        LayoutRec l;
        l.handle = self;
        r.textBytes();  // page setup name
        r.textBytes();  // printer
        r.bs();         // plot layout flags
        for (double& m : l.margins)
            m = r.bd();
        l.width = r.bd();
        l.height = r.bd();
        r.textBytes();  // paper size
        l.plotOrigin[0] = r.bd();
        l.plotOrigin[1] = r.bd();
        r.bs();  // paper units
        l.rotation = r.bs();
        r.bs();  // plot type
        for (int i = 0; i < 4; ++i)
            r.bd();  // window
        r.bd();
        r.bd();                        // custom print scale
        l.styleSheet = r.textBytes();  // style sheet
        r.bs();                        // scale type
        r.bd();                        // scale factor
        r.bd();
        r.bd();  // paper image origin
        if (fmt_.r2004) {
            r.bs();
            r.bs();
            r.bs();  // shade plot
        }
        l.name = r.textBytes();
        l.tab = static_cast<int>(r.bl());
        r.bs();  // flags
        bd3(r);  // UCS origin
        for (double& v : l.limits)
            v = r.rd();
        {
            const Vec3 ib = bd3(r);  // insertion base
            l.insBase[0] = ib.x;
            l.insBase[1] = ib.y;
            l.insBase[2] = ib.z;
        }
        bd3(r);
        bd3(r);  // UCS axes
        r.bd();  // elevation
        r.bs();  // ortho view type
        {
            const Vec3 a = bd3(r), b = bd3(r);  // extents
            l.extents[0] = a.x;
            l.extents[1] = a.y;
            l.extents[2] = a.z;
            l.extents[3] = b.x;
            l.extents[4] = b.y;
            l.extents[5] = b.z;
        }
        if (fmt_.r2004)
            r.bl();  // viewport count (a long in real files, not the raw long of the spec)
        tally(kLayoutObject, r);
        BitReader h(body, objBits);
        leadingHandles(h, self, reactors, true);
        if (fmt_.r2004)
            h.handle();  // plot view
        if (fmt_.r2007)
            h.handle();  // visual style
        l.block = resolveHandle(h.handle(), self);
        if (r.failed() || h.failed()) {
            if (out_.layoutDiag.size() < 4)
                out_.layoutDiag.push_back(
                    "layout " + hex(self) + " did not read: main data to bit " +
                    std::to_string(r.bitPos()) + " of " + std::to_string(mainEnd_) + ", handles " +
                    (h.failed() ? "failed" : "ok") + ", block " + hex(l.block) + ", bits " +
                    dumpCurrent());
            note("layout " + hex(self) + " is damaged (" + (r.failed() ? "data" : "handles") +
                 " read to bit " + std::to_string(r.bitPos()) + " of " + std::to_string(mainEnd_) +
                 ", name \"" + l.name + "\", paper " + std::to_string(l.width) + " x " +
                 std::to_string(l.height) + ")" +
                 (failDumps_[kLayoutObject]++ < 2 ? ", bits " + dumpCurrent() : std::string()));
            // The fields before the block handle are unreliable here; the layout dictionary
            // names the layout and the paper size comes from the importer's default.
            if (l.block == 0)
                return;
            l.name.clear();
            l.width = l.height = 0;
            l.tab = 0;
            l.damaged = true;
        }
        for (const LayoutRec& old : out_.layoutObjects)
            if (old.handle == l.handle)
                return;  // an object met twice (map and stream walk) is one layout
        out_.layoutObjects.push_back(std::move(l));
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
        if (height > 1e5 && hugeSeen_++ < 3) {
            std::string bits;
            note(std::string(dxfType) + " " + hex(currentHandle_) + " has height " +
                 std::to_string(height) + " at (" + std::to_string(ix) + ", " + std::to_string(iy) +
                 "), flags " + hex(flags) + ", data bits " + dumpCurrent());
        }
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
            // Gradient fill description, passed on as the DXF 450 to 470 groups.
            const std::int32_t isGradient = r.bl();
            const std::int32_t reserved = r.bl();
            const double gradAngle = r.bd();
            const double gradShift = r.bd();
            const std::int32_t single = r.bl();
            const double tint = r.bd();
            const std::int32_t colors = r.bl();
            if (colors < 0 ||
                static_cast<std::int64_t>(colors) > static_cast<std::int64_t>(r.bitsLeft()) / 8) {
                r.fail();
                return;
            }
            std::vector<std::pair<double, std::uint32_t>> stops;
            for (std::int32_t i = 0; i < colors && !r.failed(); ++i) {
                const double at = r.bd();
                r.bs();
                const std::uint32_t rgb = r.bl();
                r.rc();
                stops.emplace_back(at, rgb & 0xFFFFFF);
            }
            const std::string gradName = r.textBytes();
            if (isGradient != 0 && !r.failed()) {
                add(e, 450, isGradient);
                add(e, 451, reserved);
                add(e, 460, gradAngle);
                add(e, 461, gradShift);
                add(e, 452, single);
                add(e, 462, tint);
                add(e, 453, colors);
                for (const auto& [at, rgb] : stops) {
                    add(e, 463, at);
                    add(e, 421, static_cast<long long>(rgb));
                }
                add(e, 470, gradName);
            }
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
            bd(49);  // DIMFXL
            bd(0);   // DIMJOGANG
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
        dimStyleOverrides(e);
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
                // A multi-line attribute carries an embedded MTEXT after the text fields. The
                // spec does not say whether the embedded block starts with an entity mode, nor
                // what follows the annotative data, so each layout is tried on a copy and kept
                // only when it ends exactly where the object's main data ends.
                for (int withMode = 0; withMode < 2; ++withMode) {
                    for (int withUnknown = 0; withUnknown < 2; ++withUnknown) {
                        BitReader t = r;
                        EntityRec scratch;
                        if (withMode)
                            t.bb();
                        mtext(t, scratch);
                        const int annot = t.bs();
                        for (int i = 0; i < annot && i < 100000 && !t.failed(); ++i)
                            t.rc();
                        if (annot > 0 && withUnknown)
                            t.bs();
                        const std::string tag = t.textBytes();
                        t.bs();
                        const int flags = t.rc();
                        t.b();  // lock position
                        if (t.failed() || t.bitPos() != mainEnd_)
                            continue;
                        r = t;
                        add(e, 2, tag);
                        add(e, 70, flags);
                        return;
                    }
                }
                add(e, 2, def ? "MLATTDEF" : "MLATTRIB");
                add(e, 70, 0);
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
                r.bd();  // background scale factor (a real in real files, not the long of the spec)
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

    // MULTILEADER (R2010+): the embedded MLeaderAnnotContext carries the leader lines and the
    // note. It is passed on as the DXF group structure (CONTEXT_DATA / LEADER / LEADER_LINE) the
    // importer reads; the entity's own style overrides after it are not needed.
    void mleader(BitReader& r, EntityRec& e) {
        e.dxfType = "MULTILEADER";
        if (!fmt_.r2010) {
            r.fail();
            return;
        }
        if (mleaderNotes_ < 12)
            out_.walkNotes.push_back("MULTILEADER " + hex(currentHandle_) + " starts at bit " +
                                     std::to_string(r.bitPos()) + " of " +
                                     std::to_string(mainEnd_));
        const auto str = [&](int code, const char* v) { add(e, code, Value(std::string(v))); };
        r.bs();  // version (2)
        add(e, 270, 2);
        str(300, "CONTEXT_DATA{");
        const std::uint32_t roots = r.bl();
        if (roots > 64) {
            r.fail();
            return;
        }
        struct Root {
            Vec3 connection, dir;
            double landing = 0;
            std::vector<std::vector<Vec3>> lines;
        };
        std::vector<Root> list;
        for (std::uint32_t i = 0; i < roots && !r.failed(); ++i) {
            Root root;
            r.b();
            r.b();
            root.connection = bd3(r);
            root.dir = bd3(r);
            const std::uint32_t breaks = r.bl();
            if (breaks > 10000) {
                r.fail();
                return;
            }
            for (std::uint32_t k = 0; k < breaks; ++k) {
                bd3(r);
                bd3(r);
            }
            r.bl();  // leader index
            root.landing = r.bd();
            const std::uint32_t lines = r.bl();
            if (lines > 1000) {
                r.fail();
                return;
            }
            for (std::uint32_t k = 0; k < lines && !r.failed(); ++k) {
                const std::uint32_t n = r.bl();
                if (n > 100000) {
                    r.fail();
                    return;
                }
                std::vector<Vec3> pts;
                for (std::uint32_t q = 0; q < n && !r.failed(); ++q)
                    pts.push_back(bd3(r));
                const std::uint32_t info = r.bl();  // break info count
                if (info > 10000) {
                    r.fail();
                    return;
                }
                if (info != 0) {
                    r.bl();  // segment index
                    const std::uint32_t pairs = r.bl();
                    if (pairs > 10000) {
                        r.fail();
                        return;
                    }
                    for (std::uint32_t q = 0; q < pairs; ++q) {
                        bd3(r);
                        bd3(r);
                    }
                }
                r.bl();  // leader line index
                r.bs();  // leader type
                readCmc(r, 7);
                r.bl();  // line weight
                r.bd();  // arrow size
                r.bl();  // override flags
                root.lines.push_back(std::move(pts));
            }
            r.bs();  // attachment direction
            list.push_back(std::move(root));
        }
        if (r.failed())
            return;
        const double scale = r.bd();
        const Vec3 base = bd3(r);
        const double textHeight = r.bd();
        r.bd();  // arrow head size
        r.bd();  // landing gap
        r.bs();
        r.bs();
        r.bs();
        r.bs();
        std::string text;
        Vec3 loc{};
        bool hasLoc = false;
        if (r.b()) {  // has text
            text = r.textBytes();
            bd3(r);  // normal
            loc = bd3(r);
            hasLoc = true;
        }
        add(e, 40, scale);
        point3(e, 10, base);
        add(e, 41, textHeight);
        if (hasLoc)
            point3(e, 12, loc);
        if (!text.empty())
            add(e, 304, Value(text));
        for (const Root& root : list) {
            str(302, "LEADER{");
            point3(e, 10, root.connection);
            point3(e, 11, root.dir);
            add(e, 40, root.landing);
            for (const auto& pts : root.lines) {
                str(304, "LEADER_LINE{");
                for (const Vec3& p : pts)
                    point3(e, 10, p);
                str(305, "}");
            }
            str(303, "}");
        }
        str(301, "}");
        if (mleaderNotes_++ < 12) {
            std::size_t lines = 0;
            for (const Root& root : list)
                lines += root.lines.size();
            out_.walkNotes.push_back("MULTILEADER " + hex(currentHandle_) + ": " +
                                     std::to_string(list.size()) + " roots, " +
                                     std::to_string(lines) + " lines, text '" + text.substr(0, 40) +
                                     "', read to bit " + std::to_string(r.bitPos()) + " of " +
                                     std::to_string(mainEnd_) + (r.failed() ? ", failed" : ""));
        }
    }
    int mleaderNotes_ = 0;

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
        // 512 is the "closed" bit, 128 the linetype generation bit (PLINEGEN; DXF flag 128).
        add(e, 70, ((flag & 512) ? 1 : 0) | ((flag & 128) ? 128 : 0));
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
    std::uint64_t lastXdic_ = 0;  // extension dictionary handle of the object read last
    bool xdicMissing_ = false;    // R2004+: the object stores no extension dictionary handle
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
    {
        // Layouts whose own record could not be read are named by the layout dictionary; the
        // tab order follows the creation order (handles).
        std::map<std::uint64_t, std::string> nameOf;
        for (const auto& [dh, items] : out.dictionaries)
            for (const auto& [name, target] : items)
                if (name == "ACAD_LAYOUT") {
                    const auto d = out.dictionaries.find(target);
                    if (d != out.dictionaries.end())
                        for (const auto& [ln, lh] : d->second)
                            nameOf[lh] = ln;
                }
        // A damaged layout that no layout dictionary lists is a stray frame, not a layout.
        if (!nameOf.empty()) {
            const auto stray = [&](const LayoutRec& l) {
                return l.damaged && !nameOf.count(l.handle);
            };
            for (const LayoutRec& l : out.layoutObjects)
                if (stray(l)) {
                    char hx[24];
                    std::snprintf(hx, sizeof hx, "%llX", static_cast<unsigned long long>(l.handle));
                    const std::string prefix = std::string("layout ") + hx + " ";
                    notes.erase(std::remove_if(
                                    notes.begin(), notes.end(),
                                    [&](const std::string& n) { return n.rfind(prefix, 0) == 0; }),
                                notes.end());
                    out.layoutDiag.erase(
                        std::remove_if(
                            out.layoutDiag.begin(), out.layoutDiag.end(),
                            [&](const std::string& n) { return n.rfind(prefix, 0) == 0; }),
                        out.layoutDiag.end());
                }
            out.layoutObjects.erase(
                std::remove_if(out.layoutObjects.begin(), out.layoutObjects.end(), stray),
                out.layoutObjects.end());
        }
        std::vector<LayoutRec*> todo;
        for (LayoutRec& l : out.layoutObjects)
            if (l.damaged && nameOf.count(l.handle))
                todo.push_back(&l);
        std::sort(todo.begin(), todo.end(),
                  [](const LayoutRec* a, const LayoutRec* b) { return a->handle < b->handle; });
        int tab = 1;
        for (LayoutRec* l : todo) {
            l->name = nameOf[l->handle];
            l->tab = l->name == "Model" ? 0 : tab++;
        }
    }
    {
        // Each block lists the entities it owns; where an entity names another owner, believe
        // the list.
        std::unordered_map<std::uint64_t, std::size_t> index;
        for (std::size_t i = 0; i < out.entities.size(); ++i)
            index[out.entities[i].handle] = i;
        for (const auto& [bh, blk] : out.blocks) {
            if (!blk.membersRead)
                continue;
            std::size_t found = 0;
            for (std::uint64_t m : blk.members)
                found += index.count(m);
            out.membersMissing += blk.members.size() - found;
            // A list that mostly names objects we have is trustworthy; otherwise it was misread.
            const bool trusted = found * 10 >= blk.members.size() * 9;
            for (std::uint64_t m : blk.members) {
                const auto it = index.find(m);
                if (it == index.end())
                    continue;
                EntityRec& e = out.entities[it->second];
                // Even a partly misread list places the entities that have no owner of their own
                // (entity mode 1 or 2 stores none).
                if (!trusted && e.entmode == 0)
                    continue;
                if (e.entmode != 0) {
                    e.entmode = 0;
                    e.owner = bh;
                    ++out.ownerFixed;
                } else if (e.owner != bh) {
                    ++out.ownerFixed;
                    e.owner = bh;
                }
            }
        }
    }
    // Anonymous blocks (dimensions, hatches) are stored with a bare "*D" or "*U" name; the
    // numbering is implied. Give every repeat its own name so inserts keep their own block.
    std::set<std::string> used;
    std::map<std::string, int> counter;
    for (auto& [h, b] : out.blocks) {
        if (b.name.size() < 2 || b.name[0] != '*' || b.name.rfind("*Model_Space", 0) == 0 ||
            b.name.rfind("*Paper_Space", 0) == 0)
            continue;
        if (used.insert(b.name).second)
            continue;
        const std::string stem = b.name;
        std::string candidate;
        do {
            candidate = stem + std::to_string(++counter[stem]);
        } while (!used.insert(candidate).second);
        b.name = candidate;
    }
}

const char* readerRevision() {
    return "r89: MULTILEADER read from its embedded annotation context (leader lines and note); "
           "r88: layers with transparency listed by name and value; r88: the stream walk is used "
           "when it finds 70% of the map entries, stray damaged "
           "layouts left out, up to 200 lost places listed; r87: entity and layer transparency "
           "passed on (DXF 440), counted per layer in the "
           "report; r86: gradient hatches keep colours, angle, shift and name (DXF 450 to 470), "
           "failed "
           "entities counted per type; r84: the stream walk resumes at the next object the map "
           "places correctly, lost places "
           "listed; r83: object map blocks with offsets outside the data are listed; r83: samples "
           "of "
           "objects found away from the object map place are listed; r82: draw order tables "
           "(SORTENTSTABLE) read and applied; r81: dimension style arrow "
           "blocks decoded; r80: --lz21-search tries every cut into "
           "blocks of 1, 4 and 8 bytes; r76: distinct "
           "layer extended data listed; r75: all dictionary entry names and layer "
           "extended data listed; r74: layer extension "
           "dictionaries and xrecords "
           "traced; r79: R2007 literal runs of 11 and 19 bytes re-cut (checked against the class "
           "names of three files); r78: --lz21-search tries other cut arrangements; r77: R2007 "
           "literal runs of "
           "19, 21, 26, 28, 29 and 31 bytes cut (found with the "
           "handle map); r73: viewport extension "
           "dictionary trace; R2007 literal runs of 12, 14, 16, 18, 20, "
           "22, 24, 25, 27 and 30 bytes cut right; "
           "r72: R2018 header read right (plain 3-bit BLL, string stream end found, LTSCALE 800 on "
           "the reference sheet), layers that are off are off, viewport and style sheet trace; "
           "r71: layout plot style table reaches the page setup, AC1032 header tried at several "
           "offsets; r70: compressed streams of the small sections in the trace, converter fails "
           "(and "
           "writes nothing) when an unreadable DWG gives an empty drawing; r69: R2007 literal runs "
           "of 7, 11, 13 and 15 bytes read in the right order (all section "
           "names of a real file read); r68: R2007 odd section names no longer stop the map scan, "
           "section map stream traced; "
           "r67: R2007 literal runs in groups of 32 and more cuts, streams dumped in the trace, "
           "odd section names stop the map, header handles traced; r66: R2007 section names read "
           "by their byte length, slightly tilted planes drawn from "
           "above; r65: R2007 literal runs cut into blocks (section map, hash codes and page map "
           "now "
           "read), "
           "block count from the correction factor; r64: R2007 section map page position traced, "
           "full page list; r63: R2004 entities read "
           "without the "
           "links flag, data end exact; r62: R2007 system "
           "pages tried with 239 and 251 data bytes per block and a plausibility "
           "check; r61: R2004 objects found by walking the stream like R2010; r60: R2007 page map "
           "and "
           "section map block counts found by trial, data page coding "
           "tried; r59: header stage trace; r58: R2007 literal runs stored back to front; r57: "
           "HATCH read "
           "by its fixed type number "
           "78; r56: R2007 container; r55: class fields "
           "tried with both version layouts; r54: header "
           "read diagnostics; r53: "
           "dimension text height from the fixed height of its "
           "text style; r52: multi-line "
           "attribute tag read from the embedded mtext layout; r51: layout "
           "viewport count is a bit-long; r50: mtext background scale is a real; "
           "multi-line attributes kept; r49: data of other "
           "blocks inside each viewport counted; r48: model data inside each "
           "viewport counted; r47: viewport direction and twist "
           "listed; r46: paper rectangle moved to the sheet "
           "origin; r45: layout margins, limits and "
           "extents listed; r44: dimension style overrides read "
           "from extended data; r39: dimension styles listed; "
           "r38: widest fills listed; r37: tallest texts listed; "
           "r36: widest ignores dimension "
           "clone point; r35: widest "
           "entities listed with groups; "
           "r34: widest entities listed; r33: layouts met "
           "twice count once; r32: partly misread "
           "block member lists still place "
           "unowned entities; r31: LAYOUT is "
           "fixed type 82; r30: layout dictionary entry types; r29: layout "
           "diagnostics outside the note cap; r28: "
           "class names read from the string "
           "run after the class fields; r27: class bytes "
           "dump; r26: class string stream end retried; r25: class section skips "
           "its bit-size word; r24: "
           "class section reading retried; r23: "
           "class lookup by number, private type numbers "
           "moved; r22: layout names from the "
           "layout dictionary; r21: layout diagnostics; r20: layouts "
           "(VIEWPORT, LAYOUT), xref paths; r19: "
           "reachability info";
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
            (tails[type]++ < (type == 7    ? 4
                              : type == 79 ? 40
                                           : 2) ||
             e.handle == 3105) &&
            (type == 49 || type == 51 || type == 53 || type == 57 || type == 69 || type == 19 ||
             type == 1 || type == 34 || type == 18 || type == 17 || type == 7 || type == 44 ||
             type == 45 || type == 21 || type == 79 || e.handle == 3105)) {
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
