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

// A system page (page map or section map): the compressed data, Reed-Solomon coded in
// interleaved blocks of 255 bytes with the data bytes of the blocks following each other. Neither
// the number of blocks nor the number of data bytes per block (251 or 239) is stored with the
// page, and the compressed size in the file header is not always trustworthy, so the
// combinations are tried: first those whose data expands to the stated size and uses exactly
// `comp` bytes, then those that only expand to the stated size and pass `plausible`.
bool readSystemPage(Bytes file, std::uint64_t at, std::uint64_t comp, std::uint64_t expanded,
                    std::uint64_t hint, std::vector<std::uint8_t>& out,
                    std::vector<std::string>& notes, const char* what,
                    bool (*plausible)(const std::vector<std::uint8_t>&),
                    std::string* streamHex = nullptr) {
    if (comp == 0 || comp > kMaxSection || expanded > kMaxSection) {
        notes.push_back(std::string("the ") + what + " has impossible sizes");
        return false;
    }
    std::vector<std::uint64_t> tries;
    if (hint > 0 && hint <= 4096)
        tries.push_back(hint);
    const std::uint64_t most = std::min<std::uint64_t>(
        4096, (file.size() - std::min<std::uint64_t>(at, file.size())) / 255);
    for (std::uint64_t b = 1; b <= most; ++b)
        if (b != hint)
            tries.push_back(b);
    std::string why;
    for (int pass = 0; pass < 2; ++pass) {
        for (std::uint64_t blocks : tries) {
            if (!has(file, at, blocks * 255))
                continue;
            for (std::size_t perBlock : {std::size_t{239}, std::size_t{251}}) {
                if (pass == 0 && blocks * perBlock < comp)
                    continue;
                const std::vector<std::uint8_t> data =
                    rsDataBytes(file.subspan(static_cast<std::size_t>(at),
                                             static_cast<std::size_t>(blocks * 255)),
                                static_cast<std::size_t>(blocks), perBlock, 255, true);
                if (pass == 0) {
                    const Bytes payload(data.data(), static_cast<std::size_t>(comp));
                    if (comp == expanded) {
                        out.assign(payload.begin(), payload.end());
                        return true;
                    }
                    std::size_t used = 0;
                    std::string w;
                    if (lz21Decompress(payload, static_cast<std::size_t>(expanded), out, &w,
                                       &used) &&
                        used == comp && (!plausible || plausible(out))) {
                        if (streamHex)
                            *streamHex = hexBytes(payload, 0, 20000);
                        return true;
                    }
                    if (why.empty())
                        why = w;
                } else {
                    std::size_t used = 0;
                    if (lz21Decompress(data, static_cast<std::size_t>(expanded), out, nullptr,
                                       &used) &&
                        plausible && plausible(out)) {
                        if (streamHex)
                            *streamHex = hexBytes(Bytes(data.data(), used), 0, 20000);
                        notes.push_back(std::string("the ") + what + " was read with " +
                                        std::to_string(blocks) + " blocks of " +
                                        std::to_string(perBlock) + " using " +
                                        std::to_string(used) + " bytes, not the stated " +
                                        std::to_string(comp));
                        return true;
                    }
                }
            }
        }
    }
    notes.push_back(std::string("the ") + what + " could not be decompressed: " + why);
    return false;
}

// A page map is a list of (size, id) pairs: sizes are multiples of 32 and fit in the file.
bool pageMapPlausible(const std::vector<std::uint8_t>& pm) {
    if (pm.empty() || pm.size() % 16 != 0)
        return false;
    std::uint64_t total = 0;
    for (std::size_t at = 0; at + 16 <= pm.size(); at += 16) {
        const std::uint64_t size = le64(pm, at);
        if (size == 0 || size % 32 != 0 || size > (std::uint64_t{1} << 31))
            return false;
        total += size;
    }
    return total < (std::uint64_t{1} << 40);
}

// A section map starts with a section description whose name length is small.
bool sectionMapPlausible(const std::vector<std::uint8_t>& sm) {
    return sm.size() >= 0x40 && le64(sm, 0x20) <= 512 && le64(sm, 0x38) <= 1'000'000;
}

}  // namespace

std::vector<std::uint8_t> rsDataBytes(Bytes coded, std::size_t blocks, std::size_t k, std::size_t n,
                                      bool interleaved) {
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
    const std::vector<std::uint8_t> hd =
        rsDataBytes(file.subspan(0x80, 3 * 255), 3, 239, 255, true);
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
    // The "correction factor" in the file header gives the number of code blocks:
    // ceil(compressed size rounded up to 8 * factor / 239) (checked against a real file).
    const std::uint64_t pagesMapBlocks =
        pagesMapFactor > 0 && pagesMapFactor < 1000
            ? (((pagesMapComp + 7) & ~std::uint64_t{7}) * pagesMapFactor + 238) / 239
            : 0;
    std::string streamHex;
    if (!readSystemPage(file, kStreamBase + pagesMapOffset, pagesMapComp, pagesMapExpanded,
                        pagesMapBlocks, pm, notes, "page map", pageMapPlausible, &streamHex))
        return makeError(ErrorCode::Parse, "the DWG page map is damaged");
    tr("page map stream: " + streamHex);
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
    {
        std::string list;
        std::uint64_t at = 0;
        for (std::size_t i = 0; i + 16 <= pm.size() && i < 16 * 400; i += 16) {
            list += " " + std::to_string(static_cast<std::int64_t>(le64(pm, i + 8))) + ":" +
                    std::to_string(le64(pm, i)) + "@" + std::to_string(at);
            at += le64(pm, i);
        }
        tr("page map (id:size@offset):" + list);
    }
    tr("page map entries: " + std::to_string(pages.size()) + " (" + std::to_string(pm.size()) +
       " bytes)");
    const auto smIt = pages.find(static_cast<std::int64_t>(sectionsMapId));
    if (smIt == pages.end())
        return makeError(ErrorCode::Parse, "the DWG section map is missing");
    tr("section map page: id " + std::to_string(sectionsMapId) + ", size " +
       std::to_string(smIt->second.size) + " at data offset " +
       std::to_string(smIt->second.offset) + " (file offset " +
       std::to_string(kStreamBase + smIt->second.offset) + "), first bytes " +
       hexBytes(file, static_cast<std::size_t>(kStreamBase + smIt->second.offset), 64));
    std::vector<std::uint8_t> sm;
    if (!readSystemPage(file, kStreamBase + smIt->second.offset, sectionsMapComp,
                        sectionsMapExpanded, smIt->second.size / 255, sm, notes, "section map",
                        sectionMapPlausible))
        return makeError(ErrorCode::Parse, "the DWG section map is damaged");
    tr("section map stream: " + streamHex);
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
        // The name length counts bytes of UTF-16 text including the terminator (checked against a
        // real section map: 34 for "AcDb:FileDepList").
        if (nameLen > 512 || numPages > 1'000'000)
            break;
        std::string name;
        const std::size_t nameBytes = static_cast<std::size_t>(nameLen);
        if (pos + nameBytes > sm.size())
            break;
        for (std::size_t i = 0; i + 1 < nameBytes && sm[pos + i] != 0; i += 2)
            name += static_cast<char>(sm[pos + i]);  // section names are ASCII
        pos += nameBytes;
        if (!name.empty() && name.rfind("AcDb:", 0) != 0) {
            notes.push_back("the section map holds a section with the odd name \"" + name +
                            "\"; the rest of it is not read");
            break;
        }
        if (numPages > (sm.size() - pos) / 56) {
            notes.push_back("the section map is cut off");
            break;
        }
        tr("section \"" + name + "\": size " + std::to_string(dataSize) + ", max page " +
           std::to_string(maxSize) + ", encryption " + std::to_string(encryption) + ", encoding " +
           std::to_string(encoding) + ", " + std::to_string(numPages) + " pages");
        std::vector<std::uint8_t> data;
        if (!name.empty() && dataSize <= (std::uint64_t{1} << 28))
            data.assign(static_cast<std::size_t>(dataSize), 0);
        bool bad = false;
        std::uint64_t placed = 0;
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
            // How the page is coded is not fully known: the block count, the number of data
            // bytes per block (251 or 239) and the interleaving are tried until the data
            // expands to the stated size using exactly the stated compressed size.
            const std::uint64_t floorBlocks = pg->second.size / 255;
            std::vector<std::uint8_t> page;
            bool done = false;
            std::string why;
            std::string config;
            std::string head;
            std::string fullStream;
            for (std::uint64_t blocks : {floorBlocks, floorBlocks + 1}) {
                for (std::size_t perBlock : {std::size_t{251}, std::size_t{239}}) {
                    for (int il = 0; il < 2 && !done; ++il) {
                        const bool inter = (encoding == 4) == (il == 0);
                        if (blocks == 0 || !has(file, at, blocks * 255) || comp > blocks * perBlock)
                            continue;
                        const std::vector<std::uint8_t> raw =
                            rsDataBytes(file.subspan(static_cast<std::size_t>(at),
                                                     static_cast<std::size_t>(blocks * 255)),
                                        static_cast<std::size_t>(blocks), perBlock, 255, inter);
                        if (comp == expanded) {
                            page.assign(raw.begin(),
                                        raw.begin() + static_cast<std::ptrdiff_t>(comp));
                            done = true;
                        } else {
                            std::size_t used = 0;
                            std::string w;
                            if (lz21Decompress(Bytes(raw.data(), static_cast<std::size_t>(comp)),
                                               static_cast<std::size_t>(expanded), page, &w,
                                               &used) &&
                                used == comp)
                                done = true;
                            else if (why.empty())
                                why = w;
                        }
                        if (done) {
                            config = std::to_string(blocks) + " blocks of " +
                                     std::to_string(perBlock) +
                                     (inter ? ", interleaved" : ", plain");
                            head = hexBytes(raw, 0, 64);
                            if (comp <= 2000 &&
                                (name == "AcDb:Header" || name == "AcDb:Classes" ||
                                 name == "AcDb:Handles" || name == "AcDb:AuxHeader"))
                                fullStream = hexBytes(raw, 0, static_cast<std::size_t>(comp));
                        }
                    }
                    if (done)
                        break;
                }
                if (done)
                    break;
            }
            if (!done) {
                bad = true;
                if (trace)
                    tr("  page " + std::to_string(pageId) + " of \"" + name +
                       "\" cannot be read: at " + std::to_string(at) + ", page size " +
                       std::to_string(pg->second.size) + ", encoding " + std::to_string(encoding) +
                       ", " + why);
                continue;
            }
            if (trace && !fullStream.empty())
                tr("  stream of \"" + name + "\" (" + std::to_string(expanded) +
                   " bytes): " + fullStream);
            if (trace && k < 2)
                tr("  page " + std::to_string(pageId) + " read as " + config + "; stream starts " +
                   head + "; expanded starts " + hexBytes(page, 0, 32));
            if (offset < data.size()) {
                std::memcpy(data.data() + offset, page.data(),
                            std::min<std::size_t>(page.size(), data.size() - offset));
                ++placed;
            }
        }
        if (bad)
            notes.push_back("some pages of section " + name + " are damaged and were left empty");
        if (name == "AcDb:AcDbObjects" && placed == 0 && numPages > 0)
            return makeError(ErrorCode::Parse,
                             "none of the pages of the object section could be read");
        if (!name.empty())
            set.sections[name] = std::move(data);
    }
    if (set.sections.find("AcDb:AcDbObjects") == set.sections.end())
        return makeError(ErrorCode::Parse, "the DWG file has no object section");
    return set;
}

}  // namespace tbcad::dwg::detail
