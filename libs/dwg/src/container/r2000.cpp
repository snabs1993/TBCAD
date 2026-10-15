#include "container/r2000.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
    parseHeaderVariables(r, Format{}, v, notes);
    return v;
}

std::vector<MapEntry> readObjectMap(Bytes file, const Locator& loc,
                                    std::vector<std::string>& notes) {
    if (!loc.present)
        return {};
    const std::size_t end =
        std::min<std::size_t>(file.size(), static_cast<std::size_t>(loc.seeker) + loc.size);
    return readObjectMapData(file, loc.seeker, end, notes);
}

namespace {

struct MapState {
    std::uint64_t handle = 0;
    std::int64_t loc = 0;
};

struct MapParse {
    std::vector<MapEntry> entries{};
    std::size_t valid = 0;  // entries with an offset inside the data and an increasing handle
    MapState end{};
};

// Decodes one object-map section. `limit` is the number of bytes the offsets may point into.
MapParse parseMapSection(Bytes data, MapState state, std::uint64_t limit) {
    MapParse out;
    BitReader r(data);
    std::uint64_t previous = state.handle;
    while (r.bitsLeft() >= 16 && !r.failed()) {
        const std::int64_t dh = r.mc();
        const std::int64_t dl = r.mc();
        if (r.failed())
            break;
        // Unsigned arithmetic wraps instead of overflowing on garbage values.
        const auto udh = static_cast<std::uint64_t>(dh);
        state.handle += dh < 0 ? (~udh + 1) : udh;
        state.loc = static_cast<std::int64_t>(static_cast<std::uint64_t>(state.loc) +
                                              static_cast<std::uint64_t>(dl));
        out.entries.push_back({state.handle, state.loc});
        if (state.loc >= 0 && static_cast<std::uint64_t>(state.loc) < limit &&
            state.handle > previous)
            ++out.valid;
        previous = state.handle;
        if (out.entries.size() > 50'000'000)
            break;
    }
    out.end = state;
    return out;
}

}  // namespace

std::vector<MapEntry> readObjectMapData(Bytes file, std::size_t start, std::size_t end,
                                        std::vector<std::string>& notes, std::uint64_t limit) {
    std::vector<MapEntry> out;
    std::size_t pos = start;
    MapState state;
    if (limit == 0)
        limit = file.size();
    bool mixedReported = false;
    while (pos + 2 <= end) {
        // The section size is the one big-endian value of the format.
        const std::size_t size = (static_cast<std::size_t>(file[pos]) << 8) | file[pos + 1];
        if (size <= 2)
            break;
        // The spec says sections are cut at 2032 bytes; real files have 2035 in the size field.
        if (size > 0x1000 || pos + size + 2 > file.size()) {
            notes.push_back("the object map is damaged; later objects are missing");
            break;
        }
        const Bytes data = file.subspan(pos + 2, size - 2);
        // The spec starts the running handle and offset at zero once; a real R2018 file looks
        // restarted at every section. Take whichever reading gives offsets that lie inside the
        // object data (the first section decides nothing: both start at zero).
        const MapParse cont = parseMapSection(data, state, limit);
        MapParse best = cont;
        if (cont.valid < cont.entries.size()) {
            const MapParse reset = parseMapSection(data, MapState{}, limit);
            if (reset.valid > best.valid) {
                best = reset;
                if (!mixedReported) {
                    mixedReported = true;
                    notes.push_back("the object map restarts its running values in each section");
                }
            }
            if (best.valid < best.entries.size()) {
                const MapParse mixedA = parseMapSection(data, MapState{0, state.loc}, limit);
                if (mixedA.valid > best.valid)
                    best = mixedA;
                const MapParse mixedB = parseMapSection(data, MapState{state.handle, 0}, limit);
                if (mixedB.valid > best.valid)
                    best = mixedB;
            }
        }
        out.insert(out.end(), best.entries.begin(), best.entries.end());
        state = best.end;
        if (out.size() > 50'000'000)
            return out;
        pos += size + 2;  // size field counts itself; the CRC follows the data
    }
    return out;
}

void parseHeaderVariables(BitReader& r, const Format& fmt, HeaderVars& v,
                          std::vector<std::string>& notes) {
    const bool r2004 = fmt.r2004;
    const bool r2007 = fmt.r2007;
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
    // From R2007 the handles are in a stream of their own.
    std::string handles;  // the handles read, for the trace
    auto skipH = [&](int n) {
        if (r2007)
            return;
        for (int i = 0; i < n; ++i) {
            const HandleRef h = r.handle();
            char b[48];
            std::snprintf(b, sizeof b, "%u:%llX ", static_cast<unsigned>(h.code),
                          static_cast<unsigned long long>(h.value));
            handles += b;
        }
    };
    auto skipSpace = [&] {  // one set of paper or model space variables
        skipBd(9);          // INSBASE, EXTMIN, EXTMAX
        skipBd(4);          // LIMMIN, LIMMAX (2RD each)
        r.bd();             // ELEVATION
        skipBd(9);          // UCSORG, UCSXDIR, UCSYDIR
        skipH(2);           // UCSNAME, UCSORTHOREF
        r.bs();             // UCSORTHOVIEW
        skipH(1);           // UCSBASE
        skipBd(18);         // six UCSORG* points
    };
    std::string stages;
    auto stage = [&](const char* name, double value) {
        char b[96];
        std::snprintf(b, sizeof b, "%s=%g@%zu ", name, value, r.bitPos());
        stages += b;
    };
    if (fmt.r2013)
        stage("required", static_cast<double>(r.bll()));  // REQUIRED VERSIONS
    for (int i = 0; i < 4; ++i)
        stage("bd", r.bd());
    for (int i = 0; i < 4; ++i)
        stage("text", static_cast<double>(r.textBytes().size()));
    for (int i = 0; i < 2; ++i)
        stage("bl", r.bl());
    if (!r2004)
        r.handle();  // current viewport entity header
    skipBits(2);     // DIMASO, DIMSHO
    skipBits(7);     // PLINEGEN, ORTHOMODE, REGENMODE, FILLMODE, QTEXTMODE, PSLTSCALE, LIMCHECK
    if (r2004)
        skipBits(1);  // undocumented
    skipBits(4);      // USRTIMER, SKPOLY, ANGDIR, SPLFRAME
    skipBits(2);      // MIRRTEXT, WORLDVIEW
    skipBits(3);      // TILEMODE, PLIMCHECK, VISRETAIN
    skipBits(2);      // DISPSILH, PELLIPSE
    r.bs();           // PROXYGRAPHICS
    skipBs(5);        // TREEDEPTH, LUNITS, LUPREC, AUNITS, AUPREC
    r.bs();           // ATTMODE
    r.bs();           // PDMODE
    if (r2004)
        skipBl(3);
    skipBs(5);  // USERI1..5
    skipBs(14);
    stage("pre", 0);
    v.ltscale = r.bd();
    stage("ltscale", v.ltscale);
    skipBd(20);
    if (!r2007)
        r.textBytes();  // MENUNAME (up to R2004)
    skipBl(4);          // TDCREATE, TDUPDATE
    if (r2004)
        skipBl(3);
    skipBl(4);  // TDINDWG, TDUSRTIMER
    stage("tdindwg", 0);
    r.cmc(r2004);  // CECOLOR
    stage("cecolor", 0);
    {
        const HandleRef seed = r.handle();  // HANDSEED (always in this stream)
        stage("handseed", static_cast<double>(seed.value));
    }
    skipH(r2007 ? 6 : 5);  // CLAYER, TEXTSTYLE, CELTYPE, [CMATERIAL], DIMSTYLE, CMLSTYLE
    stage("layer handles", 0);
    stages += "[" + handles + "] ";
    stage("psvpscale", r.bd());  // PSVPSCALE
    skipSpace();                 // paper space
    skipSpace();                 // model space
    stage("spaces", 0);
    r.textBytes();
    r.textBytes();  // DIMPOST, DIMAPOST
    skipBd(9);
    if (r2007) {
        skipBd(2);  // DIMFXL, DIMJOGANG
        r.bs();     // DIMTFILL
        r.cmc(r2004);
    }
    skipBits(6);
    skipBs(3);
    if (r2007)
        r.bs();  // DIMARCSYM
    skipBd(8);
    r.bd();  // DIMALTRND
    r.b();   // DIMALT
    r.bs();  // DIMALTD
    skipBits(4);
    for (int i = 0; i < 3; ++i)
        r.cmc(r2004);  // DIMCLRD, DIMCLRE, DIMCLRT
    skipBs(11);
    skipBits(2);
    skipBs(4);
    r.b();   // DIMUPT
    r.bs();  // DIMATFIT
    if (r2007)
        r.b();  // DIMFXLON
    if (fmt.r2010) {
        r.b();   // DIMTXTDIRECTION
        r.bd();  // DIMALTMZF
        r.textBytes();
        r.bd();  // DIMMZF
        r.textBytes();
    }
    stage("dimvars", 0);
    handles.clear();
    skipH(r2007 ? 8 : 5);  // DIMTXSTY, DIMLDRBLK, DIMBLK, DIMBLK1, DIMBLK2, [three linetypes]
    skipBs(2);             // DIMLWD, DIMLWE
    skipH(9);              // control objects
    if (!r2004)
        skipH(1);  // viewport entity header control
    skipH(3);      // dictionaries
    skipBs(2);     // TSTACKALIGN, TSTACKSIZE
    r.textBytes();
    r.textBytes();
    skipH(3);  // LAYOUTS, PLOTSETTINGS, PLOTSTYLES
    if (r2004)
        skipH(2);  // MATERIALS, COLORS
    if (r2007)
        skipH(1);  // VISUALSTYLE
    if (fmt.r2013)
        skipH(1);
    stage("control handles", 0);
    stages += "[" + handles + "] ";
    r.bl();  // flags
    const int units = r.bs();
    if (r.failed() || units < 0 || units > 20 || !std::isfinite(v.ltscale) || v.ltscale <= 0 ||
        v.ltscale > 1e9) {
        notes.push_back("the drawing units could not be read from the header; unitless assumed");
        notes.push_back("header diagnostics: ltscale " + std::to_string(v.ltscale) + ", units " +
                        std::to_string(units) + ", failed " + (r.failed() ? "yes" : "no") +
                        ", bit " + std::to_string(r.bitPos()));
        notes.push_back("header stages: " + stages);
        v.valid = false;
        return;
    }
    v.insunits = units;
    v.valid = true;
}

}  // namespace tbcad::dwg::detail
