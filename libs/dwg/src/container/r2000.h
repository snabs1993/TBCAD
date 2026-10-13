// The R13-R2000 file layout (ODA spec section 3): the locator table in the file header, the class
// section and the object map. Everything is bounds-checked; damage becomes a message in `notes`.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "format.h"
#include "tbcad/core/core.h"

namespace tbcad::dwg {
class BitReader;
}

namespace tbcad::dwg::detail {

using Bytes = std::span<const std::uint8_t>;

struct Locator {
    std::uint32_t seeker = 0;
    std::uint32_t size = 0;
    bool present = false;
};

struct FileHeader {
    int codepage = 0;
    std::vector<Locator> records{};  // 0 header, 1 classes, 2 object map, ...
};

struct ClassInfo {
    int number = 0;
    std::string app{};
    std::string cppName{};
    std::string dxfName{};
    bool entity = false;
};

struct MapEntry {
    std::uint64_t handle = 0;
    std::int64_t offset = 0;  // absolute file offset of the object
};

// Reads the locator table. Fails when the bytes are too short or the table is implausible.
Result<FileHeader> readFileHeader(Bytes file);

// Class definitions (type numbers 500 and up refer to this list, in order).
std::vector<ClassInfo> readClasses(Bytes file, const Locator& loc, std::vector<std::string>& notes);

// The handle -> offset table.
std::vector<MapEntry> readObjectMap(Bytes file, const Locator& loc,
                                    std::vector<std::string>& notes);

struct HeaderVars {
    bool valid = false;
    int insunits = 0;
    double ltscale = 1;
    int lunits = 0;
};

// The few system variables the importer uses, read from the header section (section 9 of the
// spec lists about two hundred in one bit stream, so everything before INSUNITS is stepped over).
HeaderVars readHeaderVars(Bytes file, const Locator& loc, std::vector<std::string>& notes);

// Steps through the system variables (spec section 9) up to INSUNITS for the given layout.
// `r` starts at the first variable. From R2007 the strings are taken from the reader's string
// stream and the handles (except HANDSEED) are not in this stream.
void parseHeaderVariables(BitReader& r, const Format& fmt, HeaderVars& v,
                          std::vector<std::string>& notes);

// The handle -> offset table from a buffer holding only that table (R2004 and later sections,
// where offsets count from the start of the object section).
std::vector<MapEntry> readObjectMapData(Bytes data, std::size_t start, std::size_t end,
                                        std::vector<std::string>& notes);

}  // namespace tbcad::dwg::detail
