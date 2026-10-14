#include "tbcad/dwg/dwg.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

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
    {
        std::string cl = "classes read: " + std::to_string(classes.size());
        for (std::size_t i = 0; i < classes.size(); ++i)
            if (i < 3 || classes[i].dxfName == "LAYOUT" || classes[i].dxfName == "HATCH")
                cl += "; #" + std::to_string(i) + " number " + std::to_string(classes[i].number) +
                      " [" + classes[i].dxfName + "]";
        notes.push_back(cl);
    }
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
    if (!drawing.walkSummary.empty())
        report.info(code::kSkippedObject, drawing.walkSummary);
    if (!drawing.relocated.empty()) {
        std::size_t total = 0;
        std::string first;
        for (const auto& [shift, count] : drawing.relocated) {
            total += count;
            if (first.size() < 120)
                first += (first.empty() ? "" : ", ") + std::to_string(shift) + " (" +
                         std::to_string(count) + ")";
        }
        report.info(code::kSkippedObject,
                    std::to_string(total) +
                        " object(s) were found away from the place the object map gives; shifts in "
                        "bytes: " +
                        first);
    }
    for (const auto& [type, t] : drawing.layoutCheck)
        report.info(code::kSkippedObject, "layout check, object type " + std::to_string(type) +
                                              ": " + std::to_string(t[0]) +
                                              " end where expected, " + std::to_string(t[1]) +
                                              " do not");
    std::size_t skippedShown = 0;
    for (const auto& [name, count] : drawing.skippedTypes)
        if (skippedShown++ < 30)
            report.info(code::kSkippedObject,
                        std::to_string(count) + " object(s) of type " + name + " are not read yet");

    {
        // Where the entities live: the biggest owners, to see whether model space is found.
        std::map<std::uint64_t, std::size_t> perOwner;
        std::size_t paper = 0;
        for (const detail::EntityRec& e : drawing.entities) {
            if (e.entmode == 0)
                ++perOwner[e.owner];
            else
                ++paper;
        }
        std::vector<std::pair<std::size_t, std::uint64_t>> top;
        for (const auto& [h, c] : perOwner)
            top.emplace_back(c, h);
        std::sort(top.rbegin(), top.rend());
        std::string line = std::to_string(drawing.entities.size()) + " entities, " +
                           std::to_string(drawing.blocks.size()) +
                           " blocks; entity mode 1/2: " + std::to_string(paper) +
                           "; biggest owners:";
        char b[32];
        for (std::size_t i = 0; i < top.size() && i < 6; ++i) {
            std::snprintf(b, sizeof b, " %llX", static_cast<unsigned long long>(top[i].second));
            const auto it = drawing.blocks.find(top[i].second);
            line += std::string(b) + " (" +
                    (it == drawing.blocks.end() ? "no block" : it->second.name) + ", " +
                    std::to_string(top[i].first) + ")";
        }
        report.info(code::kSkippedObject, line);
        // Model and paper space: which block headers carry those names and what they own.
        std::string spaces = "spaces:";
        std::map<std::uint64_t, std::size_t> insertsPer;
        for (const detail::EntityRec& e : drawing.entities)
            if (e.entmode == 0 && e.dxfType == "INSERT")
                ++insertsPer[e.owner];
        for (const auto& [h, bl] : drawing.blocks) {
            if (bl.name.rfind("*Model_Space", 0) != 0 && bl.name.rfind("*Paper_Space", 0) != 0 &&
                bl.name.rfind("*MODEL_SPACE", 0) != 0 && bl.name.rfind("*PAPER_SPACE", 0) != 0)
                continue;
            std::snprintf(b, sizeof b, " %llX", static_cast<unsigned long long>(h));
            const auto c = perOwner.find(h);
            spaces += std::string(b) + " (" + bl.name + ", lists " + std::to_string(bl.ownedCount) +
                      (bl.membersRead ? "" : "?") + ", " +
                      std::to_string(c == perOwner.end() ? 0 : c->second) + " entities, " +
                      std::to_string(insertsPer[h]) + " inserts)";
        }
        spaces += "; member lists changed " + std::to_string(drawing.ownerFixed) +
                  " entity owners, " + std::to_string(drawing.membersMissing) +
                  " listed members were not decoded";
        std::size_t unowned = perOwner.count(0) ? perOwner[0] : 0;
        spaces += "; entities with no owner: " + std::to_string(unowned);
        report.info(code::kSkippedObject, spaces);
        std::map<std::string, std::size_t> inModel;
        for (const detail::EntityRec& e : drawing.entities)
            if (e.entmode == 2)
                ++inModel[e.dxfType];
        std::string mc = "model space (entity mode 2) holds:";
        for (const auto& [t, c] : inModel)
            mc += " " + t + " " + std::to_string(c);
        report.info(code::kSkippedObject, mc);
        {
            // Reachability: which blocks (and how many entities) a space reaches through nested
            // inserts.
            std::unordered_map<std::uint64_t, std::vector<std::size_t>> byOwner;
            for (std::size_t i = 0; i < drawing.entities.size(); ++i)
                if (drawing.entities[i].entmode == 0)
                    byOwner[drawing.entities[i].owner].push_back(i);
            auto reach = [&](bool paperSpaces) {
                std::set<std::uint64_t> seen;
                std::vector<std::uint64_t> todo;
                std::size_t missing = 0, ents = 0;
                auto visitEntity = [&](const detail::EntityRec& e) {
                    ++ents;
                    if (e.dxfType == "INSERT" && e.blockHeader) {
                        if (!drawing.blocks.count(e.blockHeader))
                            ++missing;
                        else if (seen.insert(e.blockHeader).second)
                            todo.push_back(e.blockHeader);
                    }
                };
                for (const detail::EntityRec& e : drawing.entities) {
                    const auto o = drawing.blocks.find(e.owner);
                    const bool paperOwner =
                        o != drawing.blocks.end() && o->second.name.rfind("*Paper_Space", 0) == 0;
                    const bool modelOwner =
                        o != drawing.blocks.end() && o->second.name.rfind("*Model_Space", 0) == 0;
                    if (paperSpaces ? (e.entmode == 1 || (e.entmode == 0 && paperOwner))
                                    : (e.entmode == 2 || (e.entmode == 0 && modelOwner)))
                        visitEntity(e);
                }
                while (!todo.empty()) {
                    const std::uint64_t bh = todo.back();
                    todo.pop_back();
                    for (std::size_t i : byOwner[bh])
                        visitEntity(drawing.entities[i]);
                }
                return std::to_string(seen.size()) + " blocks, " + std::to_string(ents) +
                       " entities reached, " + std::to_string(missing) +
                       " inserts name a block that was not read";
            };
            report.info(code::kSkippedObject, "reachable from model space: " + reach(false));
            report.info(code::kSkippedObject, "reachable from paper spaces: " + reach(true));
        }
        {
            // Every viewport: where it sits on its sheet and what it looks at.
            auto val = [](const detail::EntityRec& e, int code) {
                for (const detail::Group& g : e.groups)
                    if (g.code == code)
                        if (const double* v = std::get_if<double>(&g.value.v))
                            return *v;
                        else if (const long long* i = std::get_if<long long>(&g.value.v))
                            return static_cast<double>(*i);
                return 0.0;
            };
            int n = 0;
            for (const detail::EntityRec& e : drawing.entities) {
                if (e.dxfType != "VIEWPORT" || ++n > 40)
                    continue;
                const auto o = drawing.blocks.find(e.owner);
                char t[320];
                std::snprintf(t, sizeof t,
                              "viewport %llX on %s: centre %.1f,%.1f size %.1f x %.1f; target "
                              "%.1f,%.1f view centre %.1f,%.1f height %.1f; %zu frozen; %s",
                              static_cast<unsigned long long>(e.handle),
                              e.entmode == 0 && o != drawing.blocks.end() ? o->second.name.c_str()
                                                                          : "paper space",
                              val(e, 10), val(e, 20), val(e, 40), val(e, 41), val(e, 17),
                              val(e, 27), val(e, 12), val(e, 22), val(e, 45), e.frozenLayers.size(),
                              val(e, 68) != 0 ? "on" : "off");
                report.info(code::kSkippedObject, t);
            }
        }
        {
            std::string ls = "layout objects:";
            for (const detail::LayoutRec& l : drawing.layoutObjects) {
                char t[160];
                std::snprintf(t, sizeof t, " %llX block %llX tab %d %s[%s]",
                              static_cast<unsigned long long>(l.handle),
                              static_cast<unsigned long long>(l.block), l.tab,
                              l.damaged ? "(dictionary) " : "", l.name.c_str());
                ls += t;
            }
            ls += "; dictionaries read: " + std::to_string(drawing.dictionaries.size());
            report.info(code::kSkippedObject, ls);
        }
        std::string ins = "inserts per owner:";
        std::vector<std::pair<std::size_t, std::uint64_t>> topIns;
        for (const auto& [h, c] : insertsPer)
            topIns.emplace_back(c, h);
        std::sort(topIns.rbegin(), topIns.rend());
        for (std::size_t i = 0; i < topIns.size() && i < 6; ++i) {
            std::snprintf(b, sizeof b, " %llX", static_cast<unsigned long long>(topIns[i].second));
            const auto it = drawing.blocks.find(topIns[i].second);
            ins += std::string(b) + " (" +
                   (it == drawing.blocks.end() ? "no block" : it->second.name) + ", " +
                   std::to_string(topIns[i].first) + ")";
        }
        report.info(code::kSkippedObject, ins);
    }
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

std::string readerRevision() {
    return detail::readerRevision();
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
    line(std::string("object reader revision: ") + detail::readerRevision());
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
    // Information lines first (they carry the layout and recovery summaries), then the warnings.
    for (int pass = 0; pass < 2; ++pass) {
        std::size_t shown = 0;
        for (const dxf::Diagnostic& d : report.diagnostics()) {
            const bool info =
                d.severity != dxf::Severity::Error && d.severity != dxf::Severity::Warning;
            if (info != (pass == 0))
                continue;
            if (shown++ >= 200)
                break;
            line(std::string(d.severity == dxf::Severity::Error     ? "error: "
                             : d.severity == dxf::Severity::Warning ? "warning: "
                                                                    : "info: ") +
                 d.message);
        }
    }
    return out;
}

}  // namespace tbcad::dwg
