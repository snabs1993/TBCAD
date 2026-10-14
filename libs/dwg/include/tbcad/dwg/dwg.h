// DWG import: decodes a .dwg file and imports it through the DXF importer (docs/dwg-import.md).
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "tbcad/dxf/reader.h"

namespace tbcad::dwg {

enum class Version {
    Unknown,
    R12OrOlder,  // AC1009 and older: not supported
    R13,         // AC1012: not supported
    R14,         // AC1014: not supported
    R2000,       // AC1015
    R2004,       // AC1018
    R2007,       // AC1021
    R2010,       // AC1024
    R2013,       // AC1027
    R2018,       // AC1032
};

struct VersionInfo {
    Version version = Version::Unknown;
    std::string code{};  // "AC1015" or empty
    std::string name{};  // "R2000" ...
    bool supported = false;
};

// Reads the six version characters at the start of the file. Version::Unknown when the file
// does not start with "AC10" / "AC10xx" (it is not a DWG file).
VersionInfo detectVersion(std::span<const std::byte> bytes);

// True when the bytes start with a DWG version signature.
bool looksLikeDwg(std::span<const std::byte> bytes);

// Same contract as dxf::importDxf: fails only when the bytes are not a DWG file or the version
// is not supported; damaged parts of a supported file become diagnostics in `report`. The
// summary's `version` is the DWG release and the JSON report says "format":"dwg".
Result<dxf::ImportResult> importDwg(std::span<const std::byte> bytes,
                                    const dxf::ImportOptions& options, dxf::Report& report);

// A structural description of a file for diagnosing files that do not open: version, container
// steps, section sizes and the import messages. It contains no drawing content (only counts and a
// few raw bytes of each section).
std::string describe(std::span<const std::byte> bytes);

namespace code {
inline constexpr std::string_view kUnsupportedVersion = "dwg.reader.unsupported_version";
inline constexpr std::string_view kBadObject = "dwg.reader.bad_object";
inline constexpr std::string_view kBadSection = "dwg.reader.bad_section";
inline constexpr std::string_view kSkippedObject = "dwg.reader.skipped_object";
}  // namespace code

}  // namespace tbcad::dwg
