#include "tbcad/dwg/dwg.h"

#include <cstring>

#include "container/r2000.h"
#include "container/r2004.h"
#include "container/sections.h"
#include "map/emit.h"
#include "objects/decode.h"

namespace tbcad::dwg {

namespace {

using detail::Bytes;

// Reads the drawing into `drawing` for an R2000 file.
Result<bool> readR2000(Bytes file, detail::Drawing& drawing, std::vector<std::string>& notes,
                       dxf::Report& report) {
    auto header = detail::readFileHeader(file);
    if (!header)
        return header.error();
    const auto classes = detail::readClasses(file, header.value().records[1], notes);
    const auto map = detail::readObjectMap(file, header.value().records[2], notes);
    if (map.empty()) {
        std::string msg = "the drawing has no object table";
        report.error(code::kBadSection, msg);
        return makeError(ErrorCode::Parse, msg);
    }
    drawing.codepage = header.value().codepage;
    const auto vars = detail::readHeaderVars(file, header.value().records[0], notes);
    drawing.unitsKnown = vars.valid;
    drawing.insunits = vars.insunits;
    drawing.ltscale = vars.ltscale;
    detail::decodeObjects(file, map, classes, drawing, notes);
    return true;
}

// R2004, R2010, R2013 and R2018: compressed sections, objects addressed inside one section.
Result<bool> readR2004Plus(Bytes file, Version version, detail::Drawing& drawing,
                           std::vector<std::string>& notes, dxf::Report& report) {
    auto sections = detail::readSections(file, notes);
    if (!sections) {
        // Keep the reasons the container reader noted: they say which step failed.
        std::string msg = sections.error().message;
        for (std::size_t i = 0; i < notes.size() && i < 3; ++i)
            msg += (i == 0 ? " (" : "; ") + notes[i] +
                   (i + 1 == std::min<std::size_t>(notes.size(), 3) ? ")" : "");
        msg += " [" + std::string(reinterpret_cast<const char*>(file.data()), 6) + ", " +
               std::to_string(file.size()) + " bytes]";
        report.error(code::kBadSection, msg);
        return makeError(ErrorCode::Parse, msg);
    }
    detail::Format fmt;
    fmt.r2004 = true;
    fmt.r2007 = version >= Version::R2010;  // R2007 has its own container and is not read
    fmt.r2010 = version >= Version::R2010;
    fmt.r2013 = version >= Version::R2013;
    fmt.r2018 = version >= Version::R2018;
    auto& sec = sections.value();
    const int maintenance = sec.maintenance;
    auto find = [&](const char* name) -> Bytes {
        auto it = sec.sections.find(name);
        return it == sec.sections.end() ? Bytes{} : Bytes(it->second);
    };
    const Bytes objects = find("AcDb:AcDbObjects");
    const Bytes handles = find("AcDb:Handles");
    if (handles.empty()) {
        std::string msg = "the drawing has no object table";
        report.error(code::kBadSection, msg);
        return makeError(ErrorCode::Parse, msg);
    }
    const auto map = detail::readObjectMapData(handles, 0, handles.size(), notes);
    if (map.empty()) {
        std::string msg = "the drawing has no object table";
        report.error(code::kBadSection, msg);
        return makeError(ErrorCode::Parse, msg);
    }
    const auto classes = detail::parseClasses(find("AcDb:Classes"), fmt, maintenance, notes);
    const auto vars = detail::parseHeader(find("AcDb:Header"), fmt, maintenance, notes);
    drawing.codepage = sec.codepage;
    drawing.unicode = fmt.r2007;
    drawing.unitsKnown = vars.valid;
    drawing.insunits = vars.insunits;
    drawing.ltscale = vars.ltscale;
    detail::decodeObjects(objects, map, classes, drawing, notes, fmt);
    return true;
}

}  // namespace

Result<dxf::ImportResult> importDwg(std::span<const std::byte> bytes,
                                    const dxf::ImportOptions& options, dxf::Report& report) {
    VersionInfo v = detectVersion(bytes);
    if (v.code.empty())
        return makeError(ErrorCode::Parse, "this does not look like a DWG file");
    if (!v.supported) {
        std::string msg = "unsupported DWG version " + v.code + " (" + v.name +
                          "); save the drawing as R2000 or later, or as DXF";
        report.error(code::kUnsupportedVersion, msg);
        return makeError(ErrorCode::Parse, msg);
    }
    if (v.version == Version::R2007) {
        const std::string msg =
            "this drawing is a DWG saved by the 2007-2009 releases (AC1021), which TBCAD cannot "
            "read yet; save it as a 2010 or later (or 2004) DWG, or as DXF, and open that";
        report.error(code::kUnsupportedVersion, msg);
        return makeError(ErrorCode::Parse, msg);
    }

    const Bytes file(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    std::vector<std::string> notes;
    detail::Drawing drawing;
    Result<bool> read = v.version == Version::R2000
                            ? readR2000(file, drawing, notes, report)
                            : readR2004Plus(file, v.version, drawing, notes, report);
    if (!read)
        return read.error();
    for (const std::string& n : notes)
        report.warn(code::kBadObject, n);
    for (const auto& [name, count] : drawing.skippedTypes)
        report.info(code::kSkippedObject,
                    std::to_string(count) + " object(s) of type " + name + " are not read yet");

    const std::string text = detail::emitDxf(drawing);
    auto result = dxf::importDxf(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()),
        options, report);
    if (result) {
        result.value().summary.acadVer = v.code;
        result.value().summary.version = v.name;
    }
    return result;
}

}  // namespace tbcad::dwg
