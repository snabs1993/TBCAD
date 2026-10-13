#include "tbcad/dwg/dwg.h"

#include "map/dxf_text.h"

namespace tbcad::dwg {

dxf::Result<dxf::ImportResult> importDwg(std::span<const std::byte> bytes,
                                         const dxf::ImportOptions& options, dxf::Report& report) {
    (void)options;
    VersionInfo v = detectVersion(bytes);
    if (v.code.empty())
        return dxf::Error{"this does not look like a DWG file", 0};
    if (!v.supported) {
        std::string msg = "unsupported DWG version " + v.code + " (" + v.name +
                          "); save the drawing as R2000 or later, or as DXF";
        report.error(code::kUnsupportedVersion, msg);
        return dxf::Error{msg, 0};
    }
    return dxf::Error{"DWG " + v.name + " files are not readable yet", 0};
}

}  // namespace tbcad::dwg
