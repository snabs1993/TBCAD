// Test helper: writes small R2000 DWG files from our own reading of the format (ODA spec
// sections 2, 3, 20 and 23). It is only as right as that reading; real files confirm it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tbcad::dwg::test {

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
    void text(const std::string& s) {
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

// One object: type + body written by the caller, then the handle stream.
class ObjectWriter {
public:
    ObjectWriter(int type, std::uint64_t handle) : handle_(handle) {
        main.bs(type);
        sizePos_ = main.bitCount();
        main.rl(0);
        main.handle(0, handle);
    }
    BitWriter main;     // everything before the handle stream
    BitWriter handles;  // the handle stream

    // Extended data (none) + the entity common part.
    void entityCommon(int entmode, int color, int ltFlags) {
        main.bs(0);     // no extended data
        main.b(false);  // no graphic image
        main.bb(static_cast<unsigned>(entmode));
        main.bl(0);    // reactors
        main.b(true);  // links are the defaults
        main.bs(color);
        main.bd(1.0);  // linetype scale
        main.bb(static_cast<unsigned>(ltFlags));
        main.bb(0);   // plot style flags
        main.bs(0);   // visible
        main.rc(29);  // lineweight: by layer
    }
    void objectCommon() {
        main.bs(0);  // no extended data
        main.bl(0);  // reactors
    }

    std::vector<std::uint8_t> finish() {
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
        std::vector<std::uint8_t> sec;
        std::uint64_t lastH = 0;
        std::int64_t lastL = 0;
        for (auto& [h, l] : map) {
            putMc(sec, static_cast<std::int64_t>(h - lastH));
            putMc(sec, l - lastL);
            lastH = h;
            lastL = l;
        }
        const std::size_t total = sec.size() + 2;
        out.push_back(static_cast<std::uint8_t>(total >> 8));
        out.push_back(static_cast<std::uint8_t>(total & 0xff));
        out.insert(out.end(), sec.begin(), sec.end());
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);  // empty last section (size 2)
        out.push_back(2);
        out.push_back(0);
        out.push_back(0);
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

}  // namespace tbcad::dwg::test
