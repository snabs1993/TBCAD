// Test helper: writes small R2000 DWG files from our own reading of the format (ODA spec
// sections 2, 3, 20 and 23). It is only as right as that reading; real files confirm it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tbcad::dwg::test {

// When set, objects are written in the R2004 layout (extension dictionary flag, CMC colours).
inline bool gR2004 = false;
inline bool gR2007 = false;  // R2010 layout: string stream, no object size
inline bool gR2013 = false;

class BitWriter {
public:
    void bits(std::uint64_t v, unsigned n) {
        for (unsigned i = n; i > 0; --i)
            push(static_cast<unsigned>((v >> (i - 1)) & 1));
    }
    void b(bool v) { bits(v ? 1 : 0, 1); }
    void bb(unsigned v) { bits(v, 2); }
    void rc(unsigned v) { bits(v & 0xff, 8); }
    void rs(unsigned v) {
        rc(v & 0xff);
        rc((v >> 8) & 0xff);
    }
    void rl(std::uint32_t v) {
        rs(v & 0xffff);
        rs(v >> 16);
    }
    void rd(double d) {
        std::uint64_t u;
        std::memcpy(&u, &d, 8);
        rl(static_cast<std::uint32_t>(u));
        rl(static_cast<std::uint32_t>(u >> 32));
    }
    void bs(int v) {
        if (v == 0) {
            bb(2);
        } else if (v == 256) {
            bb(3);
        } else if (v > 0 && v < 256) {
            bb(1);
            rc(static_cast<unsigned>(v));
        } else {
            bb(0);
            rs(static_cast<unsigned>(v) & 0xffff);
        }
    }
    void bl(int v) {
        if (v == 0) {
            bb(2);
        } else if (v > 0 && v < 256) {
            bb(1);
            rc(static_cast<unsigned>(v));
        } else {
            bb(0);
            rl(static_cast<std::uint32_t>(v));
        }
    }
    void bd(double d) {
        if (d == 0.0) {
            bb(2);
        } else if (d == 1.0) {
            bb(1);
        } else {
            bb(0);
            rd(d);
        }
    }
    // DD: full value form only.
    void dd(double d) {
        bb(3);
        rd(d);
    }
    void dd(double d, double def) {
        if (d == def)
            bb(0);
        else
            dd(d);
    }
    // Strings go to the string stream when one is attached (R2007 and later), as UTF-16.
    BitWriter* strings = nullptr;
    void text(const std::string& s) {
        if (strings) {
            strings->bs(static_cast<int>(s.size()) + 1);
            for (char c : s)
                strings->rs(static_cast<unsigned char>(c));
            strings->rs(0);
            return;
        }
        bs(static_cast<int>(s.size()) + 1);
        for (char c : s)
            rc(static_cast<unsigned char>(c));
        rc(0);
    }
    void handle(unsigned code, std::uint64_t v) {
        std::vector<unsigned> bytes;
        while (v) {
            bytes.insert(bytes.begin(), static_cast<unsigned>(v & 0xff));
            v >>= 8;
        }
        bits(code, 4);
        bits(bytes.size(), 4);
        for (unsigned x : bytes)
            rc(x);
    }
    void ms(std::uint32_t v) {  // modular short
        for (;;) {
            if (v < 0x8000) {
                rs(v);
                return;
            }
            rs((v & 0x7fff) | 0x8000);
            v >>= 15;
        }
    }
    std::size_t bitCount() const { return n_; }
    void patch32(std::size_t bitPos, std::uint32_t v) {
        for (int i = 0; i < 32; ++i) {
            const std::size_t p = bitPos + static_cast<std::size_t>(i);
            // The value is stored as four little-endian bytes, each most significant bit first.
            const unsigned byteIdx = static_cast<unsigned>(i) / 8;
            const unsigned bitIdx = 7 - static_cast<unsigned>(i) % 8;
            const unsigned bit = (v >> (byteIdx * 8 + bitIdx)) & 1;
            set(p, bit);
        }
    }
    std::vector<std::uint8_t> bytes() const { return data_; }

private:
    void push(unsigned bit) {
        if (n_ % 8 == 0)
            data_.push_back(0);
        if (bit)
            data_.back() = static_cast<std::uint8_t>(data_.back() | (0x80 >> (n_ % 8)));
        ++n_;
    }
    void set(std::size_t p, unsigned bit) {
        const std::uint8_t mask = static_cast<std::uint8_t>(0x80 >> (p % 8));
        if (bit)
            data_[p / 8] = static_cast<std::uint8_t>(data_[p / 8] | mask);
        else
            data_[p / 8] = static_cast<std::uint8_t>(data_[p / 8] & ~mask);
    }
    std::vector<std::uint8_t> data_{};
    std::size_t n_ = 0;
};

inline void putMc(std::vector<std::uint8_t>& out, std::int64_t v) {
    const bool neg = v < 0;
    std::uint64_t u = static_cast<std::uint64_t>(neg ? -v : v);
    for (;;) {
        if (u < 0x40) {
            out.push_back(static_cast<std::uint8_t>(u | (neg ? 0x40 : 0)));
            return;
        }
        out.push_back(static_cast<std::uint8_t>((u & 0x7f) | 0x80));
        u >>= 7;
    }
}

// The object map in sections that reach about 2035 bytes, as real files do (size field big endian,
// counting itself, then two CRC bytes), ending with the empty section.
inline void appendObjectMap(std::vector<std::uint8_t>& out,
                            std::vector<std::pair<std::uint64_t, std::int64_t>> map) {
    std::sort(map.begin(), map.end());
    std::vector<std::uint8_t> sec;
    std::uint64_t lastH = 0;
    std::int64_t lastL = 0;
    auto flush = [&]() {
        if (sec.empty())
            return;
        const std::size_t total = sec.size() + 2;
        out.push_back(static_cast<std::uint8_t>(total >> 8));
        out.push_back(static_cast<std::uint8_t>(total & 0xff));
        out.insert(out.end(), sec.begin(), sec.end());
        out.push_back(0);
        out.push_back(0);
        sec.clear();
    };
    for (auto& [h, l] : map) {
        putMc(sec, static_cast<std::int64_t>(h - lastH));
        putMc(sec, l - lastL);
        lastH = h;
        lastL = l;
        if (sec.size() > 2033)
            flush();
    }
    flush();
    out.push_back(0);  // empty last section (size 2)
    out.push_back(2);
    out.push_back(0);
    out.push_back(0);
}

// One object: type + body written by the caller, then the handle stream.
class ObjectWriter {
public:
    ObjectWriter(int type, std::uint64_t handle) : handle_(handle) {
        if (gR2007) {
            // type form: BB 0 + RC below 0x1F0, BB 1 + RC for class numbers
            if (type < 0x1F0) {
                main.bb(0);
                main.rc(static_cast<unsigned>(type));
            } else {
                main.bb(1);
                main.rc(static_cast<unsigned>(type - 0x1F0));
            }
            main.strings = &strings;
        } else {
            main.bs(type);
            sizePos_ = main.bitCount();
            main.rl(0);
        }
        main.handle(0, handle);
    }
    BitWriter main;     // everything before the string stream
    BitWriter strings;  // the string stream (R2007 and later)
    BitWriter handles;  // the handle stream

    // Extended data (none) + the entity common part.
    void entityCommon(int entmode, int color, int ltFlags) {
        main.bs(0);     // no extended data
        main.b(false);  // no graphic image
        main.bb(static_cast<unsigned>(entmode));
        main.bl(0);  // reactors
        if (gR2004)
            main.b(true);  // no extension dictionary handle
        if (gR2013)
            main.b(false);  // no data in the data store
        main.b(true);       // links are the defaults
        main.bs(color);
        main.bd(1.0);  // linetype scale
        main.bb(static_cast<unsigned>(ltFlags));
        main.bb(0);  // plot style flags
        if (gR2007) {
            main.bb(0);  // material flags
            main.rc(0);  // shadow flags
            main.b(false);
            main.b(false);
            main.b(false);  // visual style flags
        }
        main.bs(0);   // visible
        main.rc(29);  // lineweight: by layer
    }
    void objectCommon() {
        main.bs(0);  // no extended data
        main.bl(0);  // reactors
        if (gR2004)
            main.b(true);  // no extension dictionary handle
        if (gR2013)
            main.b(false);  // no data in the data store
    }
    // The extension dictionary handle exists only before R2004.
    void xdic() {
        if (!gR2004)
            handles.handle(3, 0);
    }
    // CMC colour: an index up to R2000, index + RGB word + flags from R2004.
    void cmc(int index) {
        main.bs(gR2004 ? 0 : index);
        if (gR2004) {
            main.bl(static_cast<int>(0xC3000000u | static_cast<unsigned>(index)));
            main.rc(0);
        }
    }

    std::vector<std::uint8_t> finish() {
        if (gR2007)
            return finish2007();
        main.patch32(sizePos_, static_cast<std::uint32_t>(main.bitCount()));
        BitWriter all;
        // Append the handle stream after the main data bit for bit.
        std::vector<std::uint8_t> a = main.bytes();
        const std::size_t mainBits = main.bitCount();
        for (std::size_t i = 0; i < mainBits; ++i)
            all.b((a[i / 8] >> (7 - i % 8)) & 1);
        std::vector<std::uint8_t> h = handles.bytes();
        for (std::size_t i = 0; i < handles.bitCount(); ++i)
            all.b((h[i / 8] >> (7 - i % 8)) & 1);
        std::vector<std::uint8_t> body = all.bytes();
        BitWriter size;
        size.ms(static_cast<std::uint32_t>(body.size()));
        std::vector<std::uint8_t> out = size.bytes();
        out.insert(out.end(), body.begin(), body.end());
        out.push_back(0);  // CRC (not checked)
        out.push_back(0);
        return out;
    }

private:
    static void appendBits(BitWriter& to, const BitWriter& from) {
        const std::vector<std::uint8_t> a = from.bytes();
        for (std::size_t i = 0; i < from.bitCount(); ++i)
            to.b((a[i / 8] >> (7 - i % 8)) & 1);
    }
    // R2010 layout: handle stream size first, then the data, the string stream with its size and
    // flag, and the handle stream at the end (padding counts as part of the handle stream).
    std::vector<std::uint8_t> finish2007() {
        BitWriter data;
        appendBits(data, main);
        const std::size_t strBits = strings.bitCount();
        if (strBits) {
            appendBits(data, strings);
            data.rs(static_cast<unsigned>(strBits));
            data.b(true);
        } else {
            data.b(false);
        }
        std::vector<std::uint8_t> mc;
        std::size_t handleBits = handles.bitCount();
        std::size_t total = 0;
        for (int pass = 0; pass < 3; ++pass) {  // the MC length depends on the padded size
            mc.clear();
            std::size_t v = handleBits;
            do {
                std::uint8_t byte = v & 0x7f;
                v >>= 7;
                mc.push_back(static_cast<std::uint8_t>(byte | (v ? 0x80 : 0)));
            } while (v);
            total = mc.size() * 8 + data.bitCount() + handles.bitCount();
            handleBits = handles.bitCount() + ((8 - total % 8) % 8);
        }
        BitWriter all;
        for (std::uint8_t b : mc)
            all.rc(b);
        appendBits(all, data);
        appendBits(all, handles);
        std::vector<std::uint8_t> body = all.bytes();
        BitWriter size;
        size.ms(static_cast<std::uint32_t>(body.size()));
        std::vector<std::uint8_t> out = size.bytes();
        out.insert(out.end(), body.begin(), body.end());
        out.push_back(0);
        out.push_back(0);
        return out;
    }

    std::uint64_t handle_;
    std::size_t sizePos_ = 0;
};

class DwgFile {
public:
    // Adds an object; returns nothing, the map is built from the handles.
    void add(std::uint64_t handle, std::vector<std::uint8_t> bytes) {
        objects_.push_back({handle, std::move(bytes)});
    }

    // Registers a class (type numbers 500, 501, ...); returns its object type number.
    int addClass(const std::string& dxfName) {
        classes_.push_back(dxfName);
        return 499 + static_cast<int>(classes_.size());
    }

    // The R2004+ forms: every object concatenated (offsets count from the section start) and the
    // object map in the same format as R2000, without its leading size or CRC handling changes.
    void sections(std::vector<std::uint8_t>& objects, std::vector<std::uint8_t>& handles) const {
        std::vector<std::pair<std::uint64_t, std::int64_t>> map;
        for (const auto& [h, bytes] : objects_) {
            map.emplace_back(h, static_cast<std::int64_t>(objects.size()));
            objects.insert(objects.end(), bytes.begin(), bytes.end());
        }
        appendObjectMap(handles, map);
    }

    std::vector<std::uint8_t> build(const char* version = "AC1015") const {
        constexpr std::size_t kHeader = 0x19 + 3 * 9 + 2 + 16;
        std::vector<std::uint8_t> out(kHeader, 0);
        std::memcpy(out.data(), version, 6);
        std::vector<std::pair<std::uint64_t, std::int64_t>> map;
        for (const auto& [h, bytes] : objects_) {
            map.emplace_back(h, static_cast<std::int64_t>(out.size()));
            out.insert(out.end(), bytes.begin(), bytes.end());
        }
        std::sort(map.begin(), map.end());
        std::size_t classAt = 0, classSize = 0;
        if (!classes_.empty()) {
            static constexpr std::uint8_t kStart[16] = {0x8D, 0xA1, 0xC4, 0xB8, 0xC4, 0xA9,
                                                        0xF8, 0xC5, 0xC0, 0xDC, 0xF4, 0x5F,
                                                        0xE7, 0xCF, 0xB6, 0x8A};
            BitWriter cw;
            for (std::size_t i = 0; i < classes_.size(); ++i) {
                cw.bs(500 + static_cast<int>(i));
                cw.bs(0);
                cw.text("App");
                cw.text("CppName");
                cw.text(classes_[i]);
                cw.b(false);
                cw.bs(0x1F2);
            }
            const std::vector<std::uint8_t> data = cw.bytes();
            classAt = out.size();
            out.insert(out.end(), kStart, kStart + 16);
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<std::uint8_t>(data.size() >> (8 * i)));
            out.insert(out.end(), data.begin(), data.end());
            out.push_back(0);
            out.push_back(0);
            classSize = out.size() - classAt;
        }
        const std::size_t mapAt = out.size();
        appendObjectMap(out, map);
        const std::size_t mapSize = out.size() - mapAt;
        auto put32 = [&](std::size_t at, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                out[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        };
        out[0x13] = 30;  // code page
        put32(0x15, 3);
        for (std::uint32_t i = 0; i < 3; ++i) {
            const std::size_t at = 0x19 + i * 9;
            out[at] = static_cast<std::uint8_t>(i);
        }
        // record 0 (header) is absent; 1 holds the classes when there are any; 2 is the object map
        if (classAt) {
            put32(0x19 + 9 + 1, static_cast<std::uint32_t>(classAt));
            put32(0x19 + 9 + 5, static_cast<std::uint32_t>(classSize));
        }
        put32(0x19 + 2 * 9 + 1, static_cast<std::uint32_t>(mapAt));
        put32(0x19 + 2 * 9 + 5, static_cast<std::uint32_t>(mapSize));
        return out;
    }

private:
    std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> objects_{};
    std::vector<std::string> classes_{};
};

// Compresses by storing everything as one literal run (valid input for the decompressor).
inline std::vector<std::uint8_t> lzLiteral(const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    const std::size_t n = data.size();
    if (n >= 4) {
        const std::size_t v = n - 3;
        if (v <= 0x0F) {
            out.push_back(static_cast<std::uint8_t>(v));
        } else {
            out.push_back(0);
            const std::size_t rem = v - 0x0F;
            const std::size_t zeros = (rem - 1) / 0xFF;
            for (std::size_t i = 0; i < zeros; ++i)
                out.push_back(0);
            out.push_back(static_cast<std::uint8_t>(rem - zeros * 0xFF));
        }
    } else {
        // Shorter data would need a different form; pad callers to at least four bytes.
        out.push_back(1);
    }
    out.insert(out.end(), data.begin(), data.end());
    if (n < 4)
        out.insert(out.end(), 4 - n, 0);
    out.push_back(0x11);
    return out;
}

// Writes R2004-style files: encrypted file header, compressed pages, page map and section map
// (ODA spec section 4), from the same reading of the spec as the reader. Self-consistency only.
class Dwg2004File {
public:
    void section(const std::string& name, std::vector<std::uint8_t> data,
                 std::uint32_t maxPage = 0x7400, bool compressed = true) {
        sections_.push_back({name, std::move(data), maxPage, compressed});
    }

    std::vector<std::uint8_t> build(const char* version = "AC1018", int maintenance = 0) const {
        std::vector<std::uint8_t> out(0x100, 0);
        struct PageInfo {
            std::uint32_t number;
            std::uint32_t extent;
        };
        std::vector<PageInfo> pages;
        std::uint32_t nextPage = 1;
        std::vector<std::uint8_t> desc;
        put32(desc, static_cast<std::uint32_t>(sections_.size()));
        put32(desc, 2);
        put32(desc, 0x7400);
        put32(desc, 0);
        put32(desc, static_cast<std::uint32_t>(sections_.size()));
        std::uint32_t sectionId = 0;
        for (const Section& sec : sections_) {
            struct Rec {
                std::uint32_t page, size;
                std::uint64_t start;
            };
            std::vector<Rec> recs;
            for (std::size_t start = 0; start < sec.data.size() || recs.empty();
                 start += sec.maxPage) {
                std::vector<std::uint8_t> chunk(sec.maxPage, 0);
                const std::size_t n = std::min<std::size_t>(sec.maxPage, sec.data.size() - start);
                std::copy_n(sec.data.begin() + static_cast<std::ptrdiff_t>(start), n,
                            chunk.begin());
                const std::vector<std::uint8_t> payload = sec.compressed ? lzLiteral(chunk) : chunk;
                const std::uint32_t extent =
                    (32 + static_cast<std::uint32_t>(payload.size()) + 31) & ~31u;
                std::vector<std::uint8_t> hdr;
                put32(hdr, 0x4163043B);
                put32(hdr, sectionId);
                put32(hdr, static_cast<std::uint32_t>(payload.size()));
                put32(hdr, extent);
                put32(hdr, static_cast<std::uint32_t>(start));
                put32(hdr, 0);
                put32(hdr, 0);
                put32(hdr, 0);
                const std::uint32_t mask = 0x4164536Bu ^ static_cast<std::uint32_t>(out.size());
                for (std::size_t i = 0; i < 8; ++i)
                    for (int b = 0; b < 4; ++b)
                        hdr[i * 4 + static_cast<std::size_t>(b)] ^=
                            static_cast<std::uint8_t>(mask >> (8 * b));
                out.insert(out.end(), hdr.begin(), hdr.end());
                out.insert(out.end(), payload.begin(), payload.end());
                out.resize(out.size() + (extent - 32 - payload.size()), 0);
                recs.push_back({nextPage, static_cast<std::uint32_t>(payload.size()), start});
                pages.push_back({nextPage, extent});
                ++nextPage;
                if (start + sec.maxPage >= sec.data.size())
                    break;
            }
            put64(desc, sec.data.size());
            put32(desc, static_cast<std::uint32_t>(recs.size()));
            put32(desc, sec.maxPage);
            put32(desc, 0);
            put32(desc, sec.compressed ? 2 : 1);
            put32(desc, sectionId + 1);
            put32(desc, 0);
            std::vector<std::uint8_t> name(64, 0);
            std::copy(sec.name.begin(), sec.name.end(), name.begin());
            desc.insert(desc.end(), name.begin(), name.end());
            for (const Rec& r : recs) {
                put32(desc, r.page);
                put32(desc, r.size);
                put64(desc, r.start);
            }
            ++sectionId;
        }
        // Section map page, then the page map page; the page map also lists the section map.
        const std::uint32_t sectionMapNo = nextPage++;
        const std::uint32_t pageMapNo = nextPage++;
        const std::size_t sectionMapAt = out.size();
        const std::uint32_t sectionMapExtent = systemPage(out, 0x4163003B, desc);
        pages.push_back({sectionMapNo, sectionMapExtent});
        auto mapBytes = [&](std::uint32_t ownExtent) {
            std::vector<std::uint8_t> m;
            for (const PageInfo& p : pages) {
                put32(m, p.number);
                put32(m, p.extent);
            }
            put32(m, pageMapNo);
            put32(m, ownExtent);
            return m;
        };
        std::vector<std::uint8_t> probe;
        const std::uint32_t pageMapExtent = systemPage(probe, 0x41630E3B, mapBytes(0));
        const std::size_t pageMapAt = out.size();
        systemPage(out, 0x41630E3B, mapBytes(pageMapExtent));
        (void)sectionMapAt;

        // The file header: 0x80 plain bytes, then 0x6C bytes XORed with the fixed sequence.
        std::memcpy(out.data(), version, 6);
        out[0x0B] = static_cast<std::uint8_t>(maintenance);
        out[0x13] = 30;
        std::vector<std::uint8_t> head(0x6C, 0);
        std::memcpy(head.data(), "AcFssFcAJMB", 11);
        auto set32 = [&](std::size_t at, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                head[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        };
        set32(0x50, pageMapNo);
        set32(0x54, static_cast<std::uint32_t>(pageMapAt - 0x100));
        set32(0x5C, sectionMapNo);
        set32(0x60, static_cast<std::uint32_t>(pages.size()));
        std::uint32_t seed = 1;
        for (std::size_t i = 0; i < head.size(); ++i) {
            seed = seed * 0x343FDu + 0x269EC3u;
            out[0x80 + i] =
                static_cast<std::uint8_t>(head[i] ^ static_cast<std::uint8_t>(seed >> 16));
        }
        return out;
    }

private:
    struct Section {
        std::string name;
        std::vector<std::uint8_t> data;
        std::uint32_t maxPage;
        bool compressed;
    };
    static void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int i = 0; i < 4; ++i)
            v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
    }
    static void put64(std::vector<std::uint8_t>& v, std::uint64_t x) {
        put32(v, static_cast<std::uint32_t>(x));
        put32(v, static_cast<std::uint32_t>(x >> 32));
    }
    // A system page: 0x14 byte header and compressed data, padded to 0x20. Returns its extent.
    static std::uint32_t systemPage(std::vector<std::uint8_t>& out, std::uint32_t type,
                                    const std::vector<std::uint8_t>& data) {
        const std::vector<std::uint8_t> comp = lzLiteral(data);
        std::vector<std::uint8_t> page;
        put32(page, type);
        put32(page, static_cast<std::uint32_t>(data.size()));
        put32(page, static_cast<std::uint32_t>(comp.size()));
        put32(page, 2);
        put32(page, 0);
        page.insert(page.end(), comp.begin(), comp.end());
        const auto extent = static_cast<std::uint32_t>((page.size() + 31) & ~std::size_t{31});
        page.resize(extent, 0);
        out.insert(out.end(), page.begin(), page.end());
        return extent;
    }
    std::vector<Section> sections_{};
};

}  // namespace tbcad::dwg::test
