#include "tbcad/dwg/dwg.h"

#include "map/dxf_text.h"

namespace tbcad::dwg {

Result<dxf::ImportResult> importDwg(std::span<const std::byte> bytes,
                                    const dxf::ImportOptions& options, dxf::Report& report) {
    (void)options;
    VersionInfo v = detectVersion(bytes);
    if (v.code.empty())
        return makeError(ErrorCode::Parse, "this does not look like a DWG file");
    if (!v.supported) {
        std::string msg = "unsupported DWG version " + v.code + " (" + v.name +
                          "); save the drawing as R2000 or later, or as DXF";
        report.error(code::kUnsupportedVersion, msg);
        return makeError(ErrorCode::Parse, msg);
    }
    return makeError(ErrorCode::Parse, "DWG " + v.name + " files are not readable yet");
}

}  // namespace tbcad::dwg
