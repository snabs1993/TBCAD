#include "container/r2000.h"

#include <algorithm>

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

}  // namespace

Result<FileHeader> readFileHeader(Bytes file) {
    // 0x15: number of locator records; each is 1 + 4 + 4 bytes, from 0x19.
    if (file.size() < 0x19)
        return makeError(ErrorCode::Parse, "the file is too short to be a DWG file");
    FileHeader h;
    h.codepage = file[0x13] | (file[0x14] << 8);
    const std::uint32_t count = le32(file, 0x15);
    if (count < 3 || count > 16)
        return makeError(ErrorCode::Parse,
                         "the file header lists an implausible number of sections");
    if (file.size() < 0x19 + static_cast<std::size_t>(count) * 9)
        return makeError(ErrorCode::Parse, "the file header is cut off");
    h.records.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = 0x19 + static_cast<std::size_t>(i) * 9;
        const std::uint8_t recno = file[at];
        if (recno >= count)
            continue;
        Locator& l = h.records[recno];
        l.seeker = le32(file, at + 1);
        l.size = le32(file, at + 5);
        l.present = l.seeker != 0 || l.size != 0;
    }
    return h;
}

std::vector<ClassInfo> readClasses(Bytes file, const Locator& loc,
                                   std::vector<std::string>& notes) {
    std::vector<ClassInfo> out;
    if (!loc.present)
        return out;
    const std::size_t start = loc.seeker;
    if (start > file.size() || file.size() - start < 16 + 4) {
        notes.push_back("the class section lies outside the file");
        return out;
    }
    if (!std::equal(std::begin(kClassStart), std::end(kClassStart), file.begin() + start))
        notes.push_back("the class section does not start with its marker");
    const std::uint32_t size = le32(file, start + 16);
    const std::size_t dataStart = start + 20;
    if (size > file.size() - dataStart) {
        notes.push_back("the class section is cut off");
        return out;
    }
    BitReader r(file.subspan(dataStart, size));
    while (r.bitsLeft() >= 16 && !r.failed()) {
        ClassInfo c;
        c.number = r.bs();
        r.bs();  // version / proxy flags
        c.app = r.textBytes();
        c.cppName = r.textBytes();
        c.dxfName = r.textBytes();
        r.b();  // was a zombie
        c.entity = r.bs() == 0x1F2;
        if (r.failed())
            break;
        out.push_back(std::move(c));
        if (out.size() > 100000)
            break;
    }
    return out;
}

HeaderVars readHeaderVars(Bytes file, const Locator& loc, std::vector<std::string>& notes) {
    HeaderVars v;
    if (!loc.present)
        return v;
    const std::size_t start = loc.seeker;
    if (start > file.size() || file.size() - start < 16 + 4) {
        notes.push_back("the header section lies outside the file");
        return v;
    }
    const std::uint32_t size = le32(file, start + 16);
    const std::size_t dataStart = start + 20;
    if (size > file.size() - dataStart) {
        notes.push_back("the header section is cut off");
        return v;
    }
    BitReader r(file.subspan(dataStart, size));
    auto skipBits = [&](int n) {
        for (int i = 0; i < n; ++i)
            r.b();
    };
    auto skipBs = [&](int n) {
        for (int i = 0; i < n; ++i)
            r.bs();
    };
    auto skipBd = [&](int n) {
        for (int i = 0; i < n; ++i)
            r.bd();
    };
    auto skipBl = [&](int n) {
        for (int i = 0; i < n; ++i)
            r.bl();
    };
    auto skipH = [&](int n) {
        for (int i = 0; i < n; ++i)
            r.handle();
    };
    auto skipSpace = [&] {  // one set of paper or model space variables
        skipBd(9);          // INSBASE, EXTMIN, EXTMAX
        skipBd(4);          // LIMMIN, LIMMAX (2RD each)
        r.bd();             // ELEVATION
        skipBd(9);          // UCSORG, UCSXDIR, UCSYDIR
        skipH(2);           // UCSNAME, UCSORTHOREF
        r.bs();             // UCSORTHOVIEW
        r.handle();         // UCSBASE
        skipBd(18);         // six UCSORG* points
    };
    skipBd(4);
    for (int i = 0; i < 4; ++i)
        r.textBytes();
    skipBl(2);
    r.handle();   // current viewport entity header
    skipBits(2);  // DIMASO, DIMSHO
    skipBits(6);  // PLINEGEN .. LIMCHECK
    skipBits(4);  // USRTIMER, SKPOLY, ANGDIR, SPLFRAME
    skipBits(2);  // MIRRTEXT, WORLDVIEW
    skipBits(3);  // TILEMODE, PLIMCHECK, VISRETAIN
    skipBits(2);  // DISPSILH, PELLIPSE
    r.bs();       // PROXYGRAPHICS
    skipBs(5);    // TREEDEPTH, LUNITS, LUPREC, AUNITS, AUPREC
    r.bs();       // ATTMODE
    r.bs();       // PDMODE
    skipBs(5);    // USERI1..5
    skipBs(14);
    v.ltscale = r.bd();
    skipBd(20);
    r.textBytes();  // MENUNAME
    skipBl(8);
    r.bs();       // CECOLOR
    skipH(6);     // HANDSEED, CLAYER, TEXTSTYLE, CELTYPE, DIMSTYLE, CMLSTYLE
    r.bd();       // PSVPSCALE
    skipSpace();  // paper space
    skipSpace();  // model space
    r.textBytes();
    r.textBytes();  // DIMPOST, DIMAPOST
    skipBd(9);
    skipBits(6);
    skipBs(3);
    skipBd(8);
    r.bd();  // DIMALTRND
    r.b();   // DIMALT
    r.bs();  // DIMALTD
    skipBits(4);
    skipBs(3);  // three colours
    skipBs(11);
    skipBits(2);
    skipBs(4);
    r.b();   // DIMUPT
    r.bs();  // DIMATFIT
    skipH(5);
    skipBs(2);   // DIMLWD, DIMLWE
    skipH(9);    // control objects
    r.handle();  // viewport entity header control
    skipH(3);    // dictionaries
    skipBs(2);   // TSTACKALIGN, TSTACKSIZE
    r.textBytes();
    r.textBytes();
    skipH(3);
    r.bl();  // flags
    const int units = r.bs();
    if (r.failed() || units < 0 || units > 20) {
        notes.push_back("the drawing units could not be read from the header; unitless assumed");
        return v;
    }
    v.insunits = units;
    v.valid = true;
    return v;
}

std::vector<MapEntry> readObjectMap(Bytes file, const Locator& loc,
                                    std::vector<std::string>& notes) {
    std::vector<MapEntry> out;
    if (!loc.present)
        return out;
    std::size_t pos = loc.seeker;
    const std::size_t end =
        std::min<std::size_t>(file.size(), static_cast<std::size_t>(loc.seeker) + loc.size);
    std::uint64_t lastHandle = 0;
    std::int64_t lastLoc = 0;
    while (pos + 2 <= end) {
        // The section size is the one big-endian value of the format.
        const std::size_t size = (static_cast<std::size_t>(file[pos]) << 8) | file[pos + 1];
        if (size <= 2)
            break;
        if (size > 2032 + 2 || pos + size + 2 > file.size()) {
            notes.push_back("the object map is damaged; later objects are missing");
            break;
        }
        BitReader r(file.subspan(pos + 2, size - 2));
        while (r.bitsLeft() >= 16 && !r.failed()) {
            const std::int64_t dh = r.mc();
            const std::int64_t dl = r.mc();
            if (r.failed())
                break;
            // Unsigned arithmetic wraps instead of overflowing on garbage values.
            const auto udh = static_cast<std::uint64_t>(dh);
            lastHandle += dh < 0 ? (~udh + 1) : udh;
            lastLoc = static_cast<std::int64_t>(static_cast<std::uint64_t>(lastLoc) +
                                                static_cast<std::uint64_t>(dl));
            out.push_back({lastHandle, lastLoc});
            if (out.size() > 50'000'000)
                return out;
        }
        pos += size + 2;  // size field counts itself; the CRC follows the data
    }
    return out;
}

}  // namespace tbcad::dwg::detail
