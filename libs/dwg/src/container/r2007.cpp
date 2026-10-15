#include "container/r2007.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "container/lz21.h"

namespace tbcad::dwg::detail {

namespace {

using Bytes = std::span<const std::uint8_t>;

constexpr std::size_t kMaxSection = std::size_t{1} << 30;
constexpr std::uint64_t kStreamBase = 0x480;  // the data pages start after the 0x80 + 0x400 header

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

std::string hexBytes(Bytes b, std::size_t from, std::size_t count) {
    std::string s;
    char t[4];
    for (std::size_t i = from; i < b.size() && i < from + count; ++i) {
        std::snprintf(t, sizeof t, "%02X", b[i]);
        s += t;
    }
    return s;
}

// A system page (page map or section map): the data, repeated `factor` times, padded to eight
// bytes and Reed-Solomon coded in interleaved blocks of (255, 239). Returns the expanded data.
bool readSystemPage(Bytes file, std::uint64_t at, std::uint64_t comp, std::uint64_t expanded,
                    std::uint64_t factor, std::vector<std::uint8_t>& out,
                    std::vector<std::string>& notes, const char* what) {
    if (comp == 0 || comp > kMaxSection || expanded > kMaxSection || factor == 0 ||
        factor > 1000) {
        notes.push_back(std::string("the ") + what + " has impossible sizes");
        return false;
    }
    const std::uint64_t aligned = (comp + 7) & ~std::uint64_t{7};
    const std::uint64_t blocks = (aligned * factor + 238) / 239;
    if (!has(file, at, blocks * 255)) {
        notes.push_back(std::string("the ") + what + " lies outside the file");
        return false;
    }
    const std::vector<std::uint8_t> data = rsDataBytes(
        file.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(blocks * 255)),
        static_cast<std::size_t>(blocks), 239, 255, true);
    const Bytes payload(data.data(), static_cast<std::size_t>(comp));
    if (comp == expanded) {
        out.assign(payload.begin(), payload.end());
        return true;
    }
    std::string why;
    if (!lz21Decompress(payload, static_cast<std::size_t>(expanded), out, &why)) {
        notes.push_back(std::string("the ") + what + " could not be decompressed: " + why);
        return false;
    }
    return true;
}

}  // namespace

std::vector<std::uint8_t> rsDataBytes(Bytes coded, std::size_t blocks, std::size_t k,
                                      std::size_t n, bool interleaved) {
    std::vector<std::uint8_t> out(blocks * k, 0);
    for (std::size_t b = 0; b < blocks; ++b) {
        for (std::size_t j = 0; j < k; ++j) {
            const std::size_t from = interleaved ? j * blocks + b : b * k + j;
            if (from < coded.size())
                out[b * k + j] = coded[from];
        }
    }
    (void)n;
    return out;
}

Result<SectionSet> readSections2007(Bytes file, std::vector<std::string>& notes,
                                    std::vector<std::string>* trace) {
    auto tr = [&](const std::string& s) {
        if (trace)
            trace->push_back(s);
    };
    if (file.size() < kStreamBase)
        return makeError(ErrorCode::Parse, "the DWG file is too short");
    SectionSet set;
    set.maintenance = file[0x0B];
    set.codepage = file[0x13] | (file[0x14] << 8);

    // The file header: three interleaved code words of (255, 239); its decoded data is a
    // 0x20-byte prefix (CRC, key, compressed CRC, compressed size, size) and the header fields.
    const std::vector<std::uint8_t> hd = rsDataBytes(file.subspan(0x80, 3 * 255), 3, 239, 255, true);
    tr("file header decoded bytes: " + hexBytes(hd, 0, 64));
    const std::int32_t comprLen = static_cast<std::int32_t>(le32(hd, 0x18));
    const std::uint32_t length2 = le32(hd, 0x1C);
    tr("file header: compressed length " + std::to_string(comprLen) + ", length2 " +
       std::to_string(length2));
    std::vector<std::uint8_t> fh;
    if (comprLen < 0) {
        const std::size_t n = static_cast<std::size_t>(-static_cast<std::int64_t>(comprLen));
        if (n > hd.size() - 0x20)
            return makeError(ErrorCode::Parse, "the DWG file header is damaged");
        fh.assign(hd.begin() + 0x20, hd.begin() + 0x20 + n);
    } else {
        if (static_cast<std::size_t>(comprLen) > hd.size() - 0x20)
            return makeError(ErrorCode::Parse, "the DWG file header is damaged");
        std::string why;
        if (!lz21Decompress(Bytes(hd.data() + 0x20, static_cast<std::size_t>(comprLen)), 0x110, fh,
                            &why)) {
            notes.push_back("the file header could not be decompressed: " + why);
            return makeError(ErrorCode::Parse, "the DWG file header is damaged");
        }
    }
    tr("file header expanded: " + std::to_string(fh.size()) + " bytes: " + hexBytes(fh, 0, 0x110));
    if (fh.size() < 0x110)
        return makeError(ErrorCode::Parse, "the DWG file header is too short");
    const std::uint64_t pagesMapOffset = le64(fh, 0x38);
    const std::uint64_t pagesMapId = le64(fh, 0x40);
    const std::uint64_t pagesMapComp = le64(fh, 0x50);
    const std::uint64_t pagesMapExpanded = le64(fh, 0x58);
    const std::uint64_t pagesMapFactor = le64(fh, 0x18);
    const std::uint64_t pagesAmount = le64(fh, 0x60);
    const std::uint64_t sectionsMapId = le64(fh, 0xC0);
    const std::uint64_t sectionsMapComp = le64(fh, 0xB0);
    const std::uint64_t sectionsMapExpanded = le64(fh, 0xC8);
    const std::uint64_t sectionsMapFactor = le64(fh, 0xD8);
    tr("page map: offset " + std::to_string(pagesMapOffset) + ", id " + std::to_string(pagesMapId) +
       ", compressed " + std::to_string(pagesMapComp) + ", expanded " +
       std::to_string(pagesMapExpanded) + ", factor " + std::to_string(pagesMapFactor) +
       ", pages " + std::to_string(pagesAmount));
    tr("section map: id " + std::to_string(sectionsMapId) + ", compressed " +
       std::to_string(sectionsMapComp) + ", expanded " + std::to_string(sectionsMapExpanded) +
       ", factor " + std::to_string(sectionsMapFactor));

    std::vector<std::uint8_t> pm;
    if (!readSystemPage(file, kStreamBase + pagesMapOffset, pagesMapComp, pagesMapExpanded,
                        pagesMapFactor, pm, notes, "page map"))
        return makeError(ErrorCode::Parse, "the DWG page map is damaged");
    struct Page {
        std::int64_t id = 0;
        std::uint64_t size = 0;
        std::uint64_t offset = 0;
    };
    std::map<std::int64_t, Page> pages;
    {
        std::uint64_t offset = 0;
        for (std::size_t at = 0; at + 16 <= pm.size(); at += 16) {
            Page p;
            p.size = le64(pm, at);
            p.id = static_cast<std::int64_t>(le64(pm, at + 8));
            p.offset = offset;
            offset += p.size;
            pages[p.id < 0 ? -p.id : p.id] = p;
        }
    }
    tr("page map entries: " + std::to_string(pages.size()) + " (" + std::to_string(pm.size()) +
       " bytes)");
    const auto smIt = pages.find(static_cast<std::int64_t>(sectionsMapId));
    if (smIt == pages.end())
        return makeError(ErrorCode::Parse, "the DWG section map is missing");
    std::vector<std::uint8_t> sm;
    if (!readSystemPage(file, kStreamBase + smIt->second.offset, sectionsMapComp,
                        sectionsMapExpanded, sectionsMapFactor, sm, notes, "section map"))
        return makeError(ErrorCode::Parse, "the DWG section map is damaged");
    tr("section map expanded: " + std::to_string(sm.size()) + " bytes: " + hexBytes(sm, 0, 200));

    std::size_t pos = 0;
    while (pos + 0x40 <= sm.size()) {
        const std::uint64_t dataSize = le64(sm, pos);
        const std::uint64_t maxSize = le64(sm, pos + 0x08);
        const std::uint64_t encryption = le64(sm, pos + 0x10);
        const std::uint64_t nameLen = le64(sm, pos + 0x20);
        const std::uint64_t encoding = le64(sm, pos + 0x30);
        const std::uint64_t numPages = le64(sm, pos + 0x38);
        pos += 0x40;
        if (nameLen > 256 || numPages > 1'000'000)
            break;
        std::string name;
        const std::size_t nameBytes = static_cast<std::size_t>(nameLen) * 2 + (nameLen ? 2 : 0);
        if (pos + nameBytes > sm.size())
            break;
        for (std::size_t i = 0; i < nameLen; ++i)
            name += static_cast<char>(sm[pos + i * 2]);  // section names are ASCII
        pos += nameBytes;
        if (numPages > (sm.size() - pos) / 56) {
            notes.push_back("the section map is cut off");
            break;
        }
        tr("section \"" + name + "\": size " + std::to_string(dataSize) + ", max page " +
           std::to_string(maxSize) + ", encryption " + std::to_string(encryption) + ", encoding " +
           std::to_string(encoding) + ", " + std::to_string(numPages) + " pages");
        std::vector<std::uint8_t> data;
        if (!name.empty() && dataSize <= kMaxSection)
            data.assign(static_cast<std::size_t>(dataSize), 0);
        bool bad = false;
        for (std::uint64_t k = 0; k < numPages; ++k, pos += 56) {
            const std::uint64_t offset = le64(sm, pos);
            const std::uint64_t pageId = le64(sm, pos + 0x10);
            const std::uint64_t expanded = le64(sm, pos + 0x18);
            const std::uint64_t comp = le64(sm, pos + 0x20);
            if (trace && k < 2)
                tr("  page " + std::to_string(k) + ": offset " + std::to_string(offset) + ", id " +
                   std::to_string(pageId) + ", size " + std::to_string(le64(sm, pos + 8)) +
                   ", expanded " + std::to_string(expanded) + ", compressed " +
                   std::to_string(comp));
            if (data.empty())
                continue;
            const auto pg = pages.find(static_cast<std::int64_t>(pageId));
            if (pg == pages.end() || encryption != 0) {
                bad = true;
                continue;
            }
            const std::uint64_t at = kStreamBase + pg->second.offset;
            const std::uint64_t blocks = pg->second.size / 255;
            if (blocks == 0 || !has(file, at, blocks * 255) || comp > blocks * 251) {
                bad = true;
                if (trace)
                    tr("  page " + std::to_string(pageId) + " does not fit: at " +
                       std::to_string(at) + ", size " + std::to_string(pg->second.size));
                continue;
            }
            const std::vector<std::uint8_t> raw = rsDataBytes(
                file.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(blocks * 255)),
                static_cast<std::size_t>(blocks), 251, 255, encoding == 4);
            std::vector<std::uint8_t> page;
            if (comp == expanded) {
                page.assign(raw.begin(), raw.begin() + static_cast<std::size_t>(comp));
            } else {
                std::string why;
                if (!lz21Decompress(Bytes(raw.data(), static_cast<std::size_t>(comp)),
                                    static_cast<std::size_t>(expanded), page, &why)) {
                    bad = true;
                    if (trace)
                        tr("  page " + std::to_string(pageId) + " of \"" + name +
                           "\" cannot be decompressed: " + why);
                    continue;
                }
            }
            if (offset < data.size())
                std::memcpy(data.data() + offset, page.data(),
                            std::min<std::size_t>(page.size(), data.size() - offset));
        }
        if (bad)
            notes.push_back("some pages of section " + name + " are damaged and were left empty");
        if (!name.empty())
            set.sections[name] = std::move(data);
    }
    if (set.sections.find("AcDb:AcDbObjects") == set.sections.end())
        return makeError(ErrorCode::Parse, "the DWG file has no object section");
    return set;
}

}  // namespace tbcad::dwg::detail
