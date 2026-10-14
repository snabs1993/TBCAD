#include "container/r2004.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bits/bit_reader.h"
#include "container/lz77.h"

namespace tbcad::dwg::detail {

namespace {

using Bytes = std::span<const std::uint8_t>;

constexpr std::size_t kMaxSection = std::size_t{1} << 30;
constexpr std::size_t kMaxPages = 5'000'000;

std::uint32_t le32(Bytes b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
std::uint64_t le64(Bytes b, std::size_t at) {
    return static_cast<std::uint64_t>(le32(b, at)) |
           (static_cast<std::uint64_t>(le32(b, at + 4)) << 32);
}

bool has(Bytes b, std::uint64_t at, std::uint64_t len) {
    return at <= b.size() && len <= b.size() - at;
}

struct PageRef {
    std::int64_t number = 0;
    std::uint64_t size = 0;
    std::uint64_t address = 0;
};

// A system page (page map or section map): 0x14-byte header, then compressed data.
bool readSystemPage(Bytes file, std::uint64_t at, std::uint32_t expectedType,
                    std::vector<std::uint8_t>& out, std::vector<std::string>& notes,
                    const char* what) {
    if (!has(file, at, 0x14)) {
        notes.push_back(std::string("the ") + what + " lies outside the file");
        return false;
    }
    const Bytes h = file.subspan(static_cast<std::size_t>(at), 0x14);
    const std::uint32_t type = le32(h, 0);
    const std::uint32_t decomp = le32(h, 4);
    const std::uint32_t comp = le32(h, 8);
    if (type != expectedType)
        notes.push_back(std::string("the ") + what + " has an unexpected page type");
    if (decomp > kMaxSection || !has(file, at + 0x14, comp)) {
        notes.push_back(std::string("the ") + what + " is damaged");
        return false;
    }
    const Bytes payload = file.subspan(static_cast<std::size_t>(at) + 0x14, comp);
    if (le32(h, 12) == 2) {
        std::string why;
        if (!lz77Decompress(payload, decomp, out, nullptr, &why)) {
            notes.push_back(std::string("the ") + what + " could not be decompressed: " + why +
                            "; header: compressed " + std::to_string(comp) + ", expanded " +
                            std::to_string(decomp));
            return false;
        }
    } else {
        out.assign(payload.begin(), payload.end());
    }
    return true;
}

}  // namespace

std::uint32_t pageChecksum(std::uint32_t seed, std::span<const std::uint8_t> data) {
    std::uint32_t sum1 = seed & 0xFFFF;
    std::uint32_t sum2 = seed >> 16;
    std::size_t i = 0;
    while (i < data.size()) {
        const std::size_t chunk = std::min<std::size_t>(0x15B0, data.size() - i);
        for (std::size_t k = 0; k < chunk; ++k) {
            sum1 += data[i + k];
            sum2 += sum1;
        }
        i += chunk;
        sum1 %= 0xFFF1;
        sum2 %= 0xFFF1;
    }
    return (sum2 << 16) | (sum1 & 0xFFFF);
}

std::vector<std::uint8_t> headerMask(std::size_t length) {
    std::vector<std::uint8_t> out(length);
    std::uint32_t seed = 1;
    for (std::size_t i = 0; i < length; ++i) {
        seed = seed * 0x343FDu + 0x269EC3u;
        out[i] = static_cast<std::uint8_t>(seed >> 16);
    }
    return out;
}

Result<SectionSet> readSections(Bytes file, std::vector<std::string>& notes,
                                std::vector<std::string>* trace) {
    auto tr = [&](const std::string& line) {
        if (trace && trace->size() < 400)
            trace->push_back(line);
    };
    auto hex = [](std::uint64_t v) {
        char b[24];
        std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
        return std::string(b);
    };
    if (file.size() < 0x100)
        return makeError(ErrorCode::Parse, "the DWG file is too short");
    SectionSet set;
    set.maintenance = file[0x0B];
    set.codepage = static_cast<int>(file[0x13]) | (static_cast<int>(file[0x14]) << 8);
    const std::uint32_t security = le32(file, 0x18);
    if (security & 0x1)
        return makeError(ErrorCode::Parse,
                         "this DWG file is password protected and cannot be read");

    // The 0x6C bytes at 0x80 are XORed with a fixed pseudo-random sequence.
    std::vector<std::uint8_t> head(file.begin() + 0x80, file.begin() + 0x80 + 0x6C);
    const std::vector<std::uint8_t> mask = headerMask(head.size());
    for (std::size_t i = 0; i < head.size(); ++i)
        head[i] = static_cast<std::uint8_t>(head[i] ^ mask[i]);
    if (std::memcmp(head.data(), "AcFssFcAJMB", 11) != 0)
        return makeError(ErrorCode::Parse, "the DWG file header could not be decoded");
    const Bytes hd(head);
    const std::uint32_t pageMapId = le32(hd, 0x50);
    const std::uint64_t pageMapAt = le64(hd, 0x54) + 0x100;
    const std::uint32_t sectionMapId = le32(hd, 0x5C);
    const std::uint32_t pageCountHint = le32(hd, 0x60);
    tr("file header: page map id " + std::to_string(pageMapId) + " at " + hex(pageMapAt) +
       ", section map id " + std::to_string(sectionMapId) + ", page array size " +
       std::to_string(pageCountHint));

    // The page map lists every page in file order; addresses follow from the sizes.
    std::vector<std::uint8_t> map;
    if (!readSystemPage(file, pageMapAt, 0x41630E3Bu, map, notes, "page map"))
        return makeError(ErrorCode::Parse, "the DWG page map is damaged");
    std::vector<PageRef> pages;
    {
        std::uint64_t address = 0x100;
        std::size_t pos = 0;
        while (pos + 8 <= map.size() && pages.size() < kMaxPages) {
            PageRef p;
            p.number = static_cast<std::int32_t>(le32(map, pos));
            p.size = le32(map, pos + 4);
            p.address = address;
            pos += 8;
            if (p.number < 0)
                pos += 16;  // gap: parent, left, right, zero
            address += p.size;
            pages.push_back(p);
        }
    }
    tr("page map: " + std::to_string(pages.size()) + " entries, " + std::to_string(map.size()) +
       " bytes");
    auto findPage = [&](std::int64_t number) -> const PageRef* {
        for (const PageRef& p : pages)
            if (p.number == number)
                return &p;
        return nullptr;
    };
    (void)pageMapId;

    const PageRef* sm = findPage(sectionMapId);
    if (!sm)
        return makeError(ErrorCode::Parse, "the DWG section map is missing");
    std::vector<std::uint8_t> desc;
    if (!readSystemPage(file, sm->address, 0x4163003Bu, desc, notes, "section map"))
        return makeError(ErrorCode::Parse, "the DWG section map is damaged");
    if (trace && has(file, sm->address, 0x14 + le32(file, sm->address + 8))) {
        // Structure only: the compressed section map and what it expands to (no drawing data).
        const std::size_t comp = le32(file, sm->address + 8);
        std::string raw = "section map compressed bytes (" + std::to_string(comp) + "):";
        std::string dec = "section map expanded bytes (" + std::to_string(desc.size()) + "):";
        char b[4];
        for (std::size_t k = 0; k < comp; ++k) {
            std::snprintf(b, sizeof b, "%02X", file[sm->address + 0x14 + k]);
            raw += b;
        }
        for (std::size_t k = 0; k < desc.size() && k < 40000; ++k) {
            std::snprintf(b, sizeof b, "%02X", desc[k]);
            dec += b;
        }
        tr(raw);
        tr(dec);
    }
    if (desc.size() < 0x14)
        return makeError(ErrorCode::Parse, "the DWG section map is too short");
    const std::uint32_t count = le32(desc, 0);
    tr("section map: " + std::to_string(count) + " sections, " + std::to_string(desc.size()) +
       " bytes at page address " + hex(sm->address));
    std::size_t pos = 0x14;
    for (std::uint32_t si = 0; si < count; ++si) {
        if (pos + 0x60 > desc.size()) {
            notes.push_back("the section map is cut off");
            break;
        }
        const std::uint64_t total = le64(desc, pos);
        const std::uint32_t pageCount = le32(desc, pos + 8);
        const std::uint32_t maxPage = le32(desc, pos + 12);
        const std::uint32_t compressed = le32(desc, pos + 20);
        char name[65] = {};
        std::memcpy(name, desc.data() + pos + 0x20, 64);
        pos += 0x60;
        if (static_cast<std::uint64_t>(pageCount) * 16 > desc.size() - pos) {
            notes.push_back("the section map is cut off");
            break;
        }
        struct Rec {
            std::uint32_t page;
            std::uint32_t size;
            std::uint64_t start;
        };
        std::vector<Rec> recs;
        for (std::uint32_t k = 0; k < pageCount; ++k) {
            recs.push_back({le32(desc, pos), le32(desc, pos + 4), le64(desc, pos + 8)});
            if (trace && recs.size() <= 3)
                tr("  record " + std::to_string(k) + " at map byte " + std::to_string(pos) + ": " +
                   std::to_string(recs.back().page) + " / " + std::to_string(recs.back().size) +
                   " / " + std::to_string(recs.back().start));
            pos += 16;
        }
        if (trace && !recs.empty()) {
            // Hex of the map bytes around the first record that names no page.
            for (std::size_t k = 0; k < recs.size(); ++k) {
                if (findPage(recs[k].page))
                    continue;
                const std::size_t at = pos - 16 * (recs.size() - k);
                std::string l = "  first unknown page: record " + std::to_string(k) +
                                " at map byte " + std::to_string(at) + ", bytes from 48 before:";
                char b[8];
                for (std::size_t q = at >= 48 ? at - 48 : 0; q < at + 64 && q < desc.size(); ++q) {
                    std::snprintf(b, sizeof b, " %02X", desc[q]);
                    l += b;
                }
                tr(l);
                break;
            }
        }
        const std::string sname(name);
        tr("section \"" + sname + "\": size " + std::to_string(total) + ", " +
           std::to_string(pageCount) + " pages, max page " + hex(maxPage) + ", compressed " +
           std::to_string(compressed));
        if (sname.empty() || total == 0 || total > kMaxSection || maxPage == 0 ||
            maxPage > (1u << 24))
            continue;
        std::vector<std::uint8_t> data(static_cast<std::size_t>(total), 0);
        bool bad = false;
        for (const Rec& rec : recs) {
            const PageRef* pg = findPage(rec.page);
            if (!pg || !has(file, pg->address, 32)) {
                tr("  page " + std::to_string(rec.page) + ": not found in the page map");
                bad = true;
                continue;
            }
            // The 32-byte page header is XORed with a mask derived from its own address.
            std::uint8_t hdr[32];
            std::memcpy(hdr, file.data() + pg->address, 32);
            const std::uint32_t m = 0x4164536Bu ^ static_cast<std::uint32_t>(pg->address);
            for (int w = 0; w < 8; ++w) {
                std::uint32_t v = le32(Bytes(hdr, 32), static_cast<std::size_t>(w) * 4) ^ m;
                std::memcpy(hdr + w * 4, &v, 4);  // little endian host
            }
            const std::uint32_t type = le32(Bytes(hdr, 32), 0);
            const std::uint32_t dataSize = le32(Bytes(hdr, 32), 8);
            if (type != 0x4163043Bu || !has(file, pg->address + 32, dataSize)) {
                tr("  page " + std::to_string(rec.page) + " at " + hex(pg->address) +
                   ": header type " + hex(type) + ", data size " + std::to_string(dataSize) +
                   " (not a data page)");
                bad = true;
                continue;
            }
            const std::uint64_t start = rec.start;
            if (start >= total)
                continue;
            const std::size_t logical =
                static_cast<std::size_t>(std::min<std::uint64_t>(maxPage, total - start));
            const Bytes payload =
                file.subspan(static_cast<std::size_t>(pg->address) + 32, dataSize);
            if (compressed == 2) {
                std::vector<std::uint8_t> page;
                int bias = 1;
                if (!lz77Decompress(payload, maxPage, page, &bias) &&
                    !lz77Decompress(payload, logical, page, &bias)) {
                    tr("  page " + std::to_string(rec.page) + " at " + hex(pg->address) +
                       ": cannot decompress " + std::to_string(dataSize) + " bytes");
                    bad = true;
                    continue;
                }
                if (bias != 1)
                    tr("  page " + std::to_string(rec.page) + " of \"" + sname + "\" at " +
                       std::to_string(start) + " only decompresses with distance bias " +
                       std::to_string(bias));
                const std::size_t n = std::min(logical, page.size());
                std::memcpy(data.data() + start, page.data(), n);
            } else {
                const std::size_t n = std::min<std::size_t>(logical, payload.size());
                std::memcpy(data.data() + start, payload.data(), n);
            }
        }
        if (bad)
            notes.push_back("some pages of section " + sname + " are damaged and were left empty");
        set.sections[sname] = std::move(data);
    }
    if (set.sections.find("AcDb:AcDbObjects") == set.sections.end())
        return makeError(ErrorCode::Parse, "the DWG file has no object section");
    return set;
}

}  // namespace tbcad::dwg::detail
