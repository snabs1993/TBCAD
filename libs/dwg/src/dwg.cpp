#include "tbcad/dwg/dwg.h"

#include <cstring>

#include "container/r2000.h"
#include "map/emit.h"
#include "objects/decode.h"

namespace tbcad::dwg {

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
    if (v.version != Version::R2000)
        return makeError(ErrorCode::Parse, "DWG " + v.name + " files are not readable yet");

    const std::span<const std::uint8_t> file(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                             bytes.size());
    auto header = detail::readFileHeader(file);
    if (!header)
        return header.error();
    std::vector<std::string> notes;
    const auto classes = detail::readClasses(file, header.value().records[1], notes);
    const auto map = detail::readObjectMap(file, header.value().records[2], notes);
    if (map.empty()) {
        std::string msg = "the drawing has no object table";
        report.error(code::kBadSection, msg);
        return makeError(ErrorCode::Parse, msg);
    }
    detail::Drawing drawing;
    drawing.codepage = header.value().codepage;
    const auto vars = detail::readHeaderVars(file, header.value().records[0], notes);
    drawing.unitsKnown = vars.valid;
    drawing.insunits = vars.insunits;
    drawing.ltscale = vars.ltscale;
    detail::decodeObjects(file, map, classes, drawing, notes);
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
