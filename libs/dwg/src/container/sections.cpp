#include "container/sections.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bits/bit_reader.h"

namespace tbcad::dwg::detail {

namespace {

std::uint32_t le32(Bytes b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

constexpr std::uint8_t kClassStart[16] = {0x8D, 0xA1, 0xC4, 0xB8, 0xC4, 0xA9, 0xF8, 0xC5,
                                          0xC0, 0xDC, 0xF4, 0x5F, 0xE7, 0xCF, 0xB6, 0x8A};
constexpr std::uint8_t kHeaderStart[16] = {0xCF, 0x7B, 0x1F, 0x23, 0xFD, 0xDE, 0x38, 0xA9,
                                           0x5F, 0x7C, 0x68, 0xB8, 0x4E, 0x6D, 0x33, 0x5F};

// Sentinel, size, and the optional extra size word; returns the data bytes or empty.
Bytes sectionData(Bytes section, const std::uint8_t (&sentinel)[16], std::size_t extraBytes,
                  std::vector<std::string>& notes, const char* what) {
    if (section.size() < 20) {
        notes.push_back(std::string("the ") + what + " section is too short");
        return {};
    }
    if (std::memcmp(section.data(), sentinel, 16) != 0)
        notes.push_back(std::string("the ") + what + " section does not start with its marker");
    std::size_t pos = 16;
    const std::uint32_t size = le32(section, pos);
    pos += 4;
    if (extraBytes) {
        if (section.size() < pos + extraBytes) {
            notes.push_back(std::string("the ") + what + " section is too short");
            return {};
        }
        pos += extraBytes;
    }
    if (size > section.size() - pos) {
        notes.push_back(std::string("the ") + what + " section is cut off");
        return {};
    }
    return section.subspan(pos, size);
}

}  // namespace

namespace {

std::vector<ClassInfo> parseClassesWith(Bytes section, const Format& fmt, std::size_t extra,
                                        bool useBitsWord, std::vector<std::string>& notes) {
    std::vector<ClassInfo> out;
    const Bytes data = sectionData(section, kClassStart, extra, notes, "class");
    if (data.empty())
        return out;
    BitReader r(data);
    BitReader strings;
    std::size_t endBit = data.size() * 8;
    if (extra >= 8 && useBitsWord) {
        // A real R2018 file: a high size word, then the number of data bits.
        std::uint32_t bitsWord = 0;
        for (int i = 0; i < 4; ++i)
            bitsWord |= static_cast<std::uint32_t>(section[16 + 4 + 4 + i]) << (8 * i);
        if (bitsWord > 0 && bitsWord <= endBit)
            endBit = bitsWord;
    }
    if (fmt.r2007) {
        if (!locateStringStream(data, endBit, strings)) {
            notes.push_back("the class section has a damaged string stream");
            return out;
        }
        if (strings.bitSize() != 0) {
            endBit = strings.bitPos();
            r.setStringStream(&strings);
        }
    }
    r.bs();  // maximum class number
    r.rc();
    r.rc();
    r.b();
    while (r.bitPos() + 16 <= endBit && !r.failed()) {
        ClassInfo c;
        c.number = r.bs();
        r.bs();  // proxy flags
        c.app = r.textBytes();
        c.cppName = r.textBytes();
        c.dxfName = r.textBytes();
        r.b();  // was a zombie
        c.entity = r.bs() == 0x1F2;
        r.bl();  // objects of this class
        r.bs();  // version
        r.bs();  // maintenance release
        r.bl();
        r.bl();
        if (r.failed() || c.number < 500) {
            if (out.size() < 5) {
                notes.push_back("class data size " + std::to_string(data.size()) + ", end bit " +
                                std::to_string(endBit) + ", strings at " +
                                std::to_string(strings.bitSize() ? strings.bitPos() : 0));
                std::string hx = "class data head:";
                char b[8];
                for (std::size_t k = 0; k < 96 && k < data.size(); ++k) {
                    std::snprintf(b, sizeof b, " %02X", data[k]);
                    hx += b;
                }
                hx += " ... tail:";
                for (std::size_t k = data.size() > 64 ? data.size() - 64 : 0; k < data.size();
                     ++k) {
                    std::snprintf(b, sizeof b, " %02X", data[k]);
                    hx += b;
                }
                notes.push_back(hx);
            }
            if (out.empty())
                notes.push_back("first class entry unreadable: number " + std::to_string(c.number) +
                                ", names [" + c.app + "] [" + c.cppName + "] [" + c.dxfName +
                                "], bit " + std::to_string(r.bitPos()) + " of " +
                                std::to_string(endBit) + " (extra word " +
                                (extra ? "read" : "not read") + ")");
            break;
        }
        out.push_back(std::move(c));
        if (out.size() > 100000)
            break;
    }
    return out;
}

}  // namespace

std::vector<ClassInfo> parseClasses(Bytes section, const Format& fmt, int maintenance,
                                    std::vector<std::string>& notes) {
    // The size of the words between the size and the data depends on the release (a real R2018
    // file has two); the first reading that yields classes wins.
    const std::size_t known = fmt.r2018 ? 8 : (fmt.r2010 && maintenance > 3 ? 4 : 0);
    std::vector<std::size_t> tries{known};
    if (fmt.r2010)
        for (std::size_t x : {8u, 4u, 0u})
            if (x != known)
                tries.push_back(x);
    std::vector<std::string> first;
    for (std::size_t i = 0; i < tries.size(); ++i) {
        std::vector<std::string> n;
        auto a = parseClassesWith(section, fmt, tries[i], true, n);
        if (a.size() < 5 && tries[i] >= 8) {
            std::vector<std::string> n2;
            auto b = parseClassesWith(section, fmt, tries[i], false, n2);
            if (b.size() > a.size()) {
                a = std::move(b);
                n = n2;
            }
        }
        if (!a.empty()) {
            if (i == 0)
                notes.insert(notes.end(), n.begin(), n.end());
            return a;
        }
        if (i == 0)
            first = n;
    }
    notes.insert(notes.end(), first.begin(), first.end());
    return {};
}

HeaderVars parseHeader(Bytes section, const Format& fmt, int maintenance,
                       std::vector<std::string>& notes) {
    HeaderVars v;
    const bool extra = fmt.r2018 || (fmt.r2010 && maintenance > 3);
    const Bytes data = sectionData(section, kHeaderStart, extra ? 4 : 0, notes, "header");
    if (data.empty())
        return v;
    std::vector<std::string> tried;
    if (!fmt.r2007) {
        BitReader r(data);
        parseHeaderVariables(r, fmt, v, notes);
        return v;
    }
    // R2007 and later: strings are in a stream at the end of the data. The way that stream is
    // found is the one used for objects; if the result is not plausible the strings are tried
    // inline. Neither has been confirmed against a real file.
    {
        BitReader r(data);
        BitReader strings;
        if (locateStringStream(data, data.size() * 8, strings)) {
            if (strings.bitSize() != 0)
                r.setStringStream(&strings);
            HeaderVars a;
            parseHeaderVariables(r, fmt, a, tried);
            if (a.valid)
                return a;
        }
    }
    {
        BitReader r(data);
        r.setStringStream(&r);  // UTF-16 strings in the main stream
        HeaderVars b;
        parseHeaderVariables(r, fmt, b, tried);
        if (b.valid)
            return b;
    }
    notes.push_back("the drawing units could not be read from the header; unitless assumed");
    return v;
}

}  // namespace tbcad::dwg::detail
