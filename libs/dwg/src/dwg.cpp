#include "tbcad/dwg/dwg.h"

#include <cstdio>
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
    const auto map = detail::readObjectMapData(handles, 0, handles.size(), notes, objects.size());
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
    for (const auto& [type, t] : drawing.layoutCheck)
        report.info(code::kSkippedObject, "layout check, object type " + std::to_string(type) +
                                              ": " + std::to_string(t[0]) +
                                              " end where expected, " + std::to_string(t[1]) +
                                              " do not");
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

std::string describe(std::span<const std::byte> bytes) {
    std::string out;
    auto line = [&](const std::string& s) { out += s + "\n"; };
    auto hex16 = [](const std::vector<std::uint8_t>& v) {
        std::string h;
        char b[4];
        for (std::size_t i = 0; i < v.size() && i < 16; ++i) {
            std::snprintf(b, sizeof b, "%02X ", v[i]);
            h += b;
        }
        return h;
    };
    const VersionInfo v = detectVersion(bytes);
    line("file size: " + std::to_string(bytes.size()) + " bytes");
    line("version: " + (v.code.empty() ? std::string("(not a DWG file)") : v.code + " " + v.name) +
         (v.supported ? "" : " (not supported)"));
    if (v.code.empty())
        return out;
    const Bytes file(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    if (v.version >= Version::R2004 && v.version != Version::R2007) {
        std::vector<std::string> notes, trace;
        auto sections = detail::readSections(file, notes, &trace);
        for (const std::string& t : trace)
            line(t);
        for (const std::string& n : notes)
            line("note: " + n);
        if (!sections) {
            line("container failed: " + sections.error().message);
        } else {
            for (const auto& [name, data] : sections.value().sections)
                line("decoded section " + name + ": " + std::to_string(data.size()) +
                     " bytes, starts " + hex16(data));
            const auto& secs = sections.value().sections;
            const auto objs = secs.find("AcDb:AcDbObjects");
            const auto hnd = secs.find("AcDb:Handles");
            if (objs != secs.end() && hnd != secs.end()) {
                std::vector<std::string> n2;
                {
                    // The framing of the first handle-map sections: size, first bytes, the bytes
                    // just before the next section (CRC) and the section after.
                    const auto& hd = hnd->second;
                    std::size_t at = 0;
                    for (int si = 0; si < 4 && at + 2 <= hd.size(); ++si) {
                        const std::size_t sz = (std::size_t(hd[at]) << 8) | hd[at + 1];
                        std::string l = "handle map section " + std::to_string(si) + " at " +
                                        std::to_string(at) + ": size " + std::to_string(sz) +
                                        ", starts";
                        char b[8];
                        for (std::size_t k = 2; k < 14 && at + k < hd.size(); ++k) {
                            std::snprintf(b, sizeof b, " %02X", hd[at + k]);
                            l += b;
                        }
                        l += ", ends";
                        for (std::size_t k = (sz >= 6 ? sz - 4 : 0);
                             k < sz + 6 && at + k < hd.size(); ++k) {
                            std::snprintf(b, sizeof b, " %02X", hd[at + k]);
                            l += b;
                        }
                        line(l);
                        if (sz <= 2)
                            break;
                        at += sz + 2;
                    }
                }
                const auto map = detail::readObjectMapData(hnd->second, 0, hnd->second.size(), n2,
                                                           objs->second.size());
                for (const std::string& n : n2)
                    line("note: " + n);
                detail::Format fmt;
                fmt.r2004 = true;
                fmt.r2007 = v.version >= Version::R2010;
                fmt.r2010 = v.version >= Version::R2010;
                fmt.r2013 = v.version >= Version::R2013;
                fmt.r2018 = v.version >= Version::R2018;
                std::vector<std::string> lines;
                detail::describeObjects(objs->second, map, fmt, lines);
                for (const std::string& l : lines)
                    line(l);
            }
        }
    }
    dxf::Report report;
    auto r = importDwg(bytes, dxf::ImportOptions{}, report);
    line(r ? "import: ok" : "import failed: " + r.error().message);
    std::size_t shown = 0;
    for (const dxf::Diagnostic& d : report.diagnostics()) {
        if (shown++ >= 80)
            break;
        line(std::string(d.severity == dxf::Severity::Error     ? "error: "
                         : d.severity == dxf::Severity::Warning ? "warning: "
                                                                : "info: ") +
             d.message);
    }
    return out;
}

}  // namespace tbcad::dwg
