#include "tbcad/dwg/dwg.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <variant>

#include "container/lz21.h"
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
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
    };
    auto sections = detail::readSections(file, notes);
    const auto t1 = Clock::now();
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
    fmt.r2007 = version >= Version::R2007;  // from R2007 on strings and handles have streams
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
    const auto t2 = Clock::now();
    detail::decodeObjects(objects, map, classes, drawing, notes, fmt);
    const auto t3 = Clock::now();
    auto msd = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    drawing.stageMs = {{"sections (decompress)", msd(t0, t1)},
                       {"object map, classes, header", msd(t1, t2)},
                       {"objects", msd(t2, t3)}};
    drawing.walkNotes.push_back(
        "read stages (ms): sections " + ms(t0, t1) + ", map/classes/header " + ms(t1, t2) +
        ", objects " + ms(t2, t3) + " (" + std::to_string(objects.size() / 1024) +
        " KB of object data, " + std::to_string(map.size()) + " map entries)");
    return true;
}

}  // namespace

Result<dxf::ImportResult> importDwg(std::span<const std::byte> bytes,
                                    const dxf::ImportOptions& options, dxf::Report& report) {
    return importDwg(bytes, options, report, nullptr);
}

Result<dxf::ImportResult> importDwg(std::span<const std::byte> bytes,
                                    const dxf::ImportOptions& options, dxf::Report& report,
                                    std::vector<std::pair<std::string, double>>* timings) {
    VersionInfo v = detectVersion(bytes);
    if (v.code.empty())
        return makeError(ErrorCode::Parse, "this does not look like a DWG file");
    if (!v.supported) {
        std::string msg = "unsupported DWG version " + v.code + " (" + v.name +
                          "); save the drawing as R2000 or later, or as DXF";
        report.error(code::kUnsupportedVersion, msg);
        return makeError(ErrorCode::Parse, msg);
    }
    using Clock = std::chrono::steady_clock;
    const auto tStart = Clock::now();
    auto msSince = [](Clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
    };
    const Bytes file(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    std::vector<std::string> notes;
    detail::Drawing drawing;
    Result<bool> read = v.version == Version::R2000
                            ? readR2000(file, drawing, notes, report)
                            : readR2004Plus(file, v.version, drawing, notes, report);
    if (!read)
        return read.error();
    const long long readMs = msSince(tStart);
    const auto tDiag = Clock::now();
    for (const std::string& n : notes)
        report.warn(code::kBadObject, n);
    if (!drawing.walkSummary.empty())
        report.summary(code::kSkippedObject, drawing.walkSummary);
    if (options.diagnostics) {
        {
            std::map<std::string, std::map<long long, std::size_t>> perLayer;
            std::size_t layersWith = 0;
            for (const auto& [h, l] : drawing.layers)
                if (l.transparency != 0)
                    ++layersWith;
            for (const auto& e : drawing.entities)
                if (e.transparency != 0) {
                    const auto l = drawing.layers.find(e.layer);
                    ++perLayer[l == drawing.layers.end() ? std::string("?") : l->second.name]
                              [e.transparency];
                }
            if (layersWith != 0 || !perLayer.empty()) {
                std::string t = "transparency: " + std::to_string(layersWith) + " layers carry it";
                std::size_t listed = 0;
                for (const auto& [h, l] : drawing.layers)
                    if (l.transparency != 0 && listed++ < 40) {
                        char hx[24];
                        std::snprintf(hx, sizeof hx, "%llX",
                                      static_cast<unsigned long long>(l.transparency));
                        t += std::string(listed == 1 ? " [" : ", ") + l.name + "=" + hx;
                    }
                t += (listed ? "]" : "") +
                     std::string(
                         "; entities with their own transparency per layer (value x count):");
                // Every layer with such entities, not only the first few: the value says which.
                for (const auto& [name, values] : perLayer) {
                    t += " " + name + "{";
                    for (const auto& [v, n] : values) {
                        char hx[16];
                        std::snprintf(hx, sizeof hx, "%llX", static_cast<unsigned long long>(v));
                        t += std::string(hx) + "x" + std::to_string(n) + " ";
                    }
                    t += "}";
                }
                report.summary(code::kSkippedObject, t);
            }
        }
        {
            // Layers whose colour is a true colour or a grey index, and the entities that carry a
            // true colour of their own: what AutoCAD may plot as a screened grey.
            std::string t = "layer colours (true colour or grey index, name [words]):";
            std::size_t shown = 0;
            for (const auto& [h, l] : drawing.layers)
                if ((l.trueColor >= 0 || (l.color >= 250 && l.color <= 255)) && shown++ < 150)
                    t += " " + l.name + " [" + l.colorRaw + "]";
            report.summary(code::kSkippedObject, t);
            std::map<std::pair<int, std::string>, std::size_t> own;
            for (const auto& e : drawing.entities)
                if (e.trueColor >= 0) {
                    const auto l = drawing.layers.find(e.layer);
                    ++own[{e.trueColor,
                           l == drawing.layers.end() ? std::string("?") : l->second.name}];
                }
            std::string u = "entities with their own true colour (rgb hex, layer: count):";
            std::size_t n = 0;
            for (const auto& [key, count] : own)
                if (n++ < 100) {
                    char hx[16];
                    std::snprintf(hx, sizeof hx, "%06X", key.first);
                    u += " " + std::string(hx) + " " + key.second + ":" + std::to_string(count);
                }
            report.summary(code::kSkippedObject, u);
        }
        {
            // One column hatch and its layer: every property that could make the fill grey.
            std::size_t shown = 0;
            for (const auto& e : drawing.entities) {
                if (e.dxfType != "HATCH" || shown >= 2)
                    continue;
                const auto l = drawing.layers.find(e.layer);
                if (l == drawing.layers.end() ||
                    l->second.name.find("COLNHATC") == std::string::npos)
                    continue;
                ++shown;
                std::string t =
                    "column hatch trace: handle " + std::to_string(e.handle) + " layer " +
                    l->second.name + " layer colour " + std::to_string(l->second.color) +
                    " layer colour words [" + l->second.colorRaw + "] layer transparency " +
                    std::to_string(l->second.transparency) + " layer eed " + l->second.eed +
                    " | entity colour " + std::to_string(e.color) + " true colour " +
                    std::to_string(e.trueColor) + " transparency " +
                    std::to_string(e.transparency) + " lineweight " + std::to_string(e.lineweight) +
                    " xdic " + std::to_string(e.xdic) + " groups:";
                std::size_t n = 0;
                for (const auto& g : e.groups) {
                    if (n++ >= 40)
                        break;
                    t += " " + std::to_string(g.code) + "=";
                    if (const auto* str = std::get_if<std::string>(&g.value.v))
                        t += *str;
                    else if (const auto* d = std::get_if<double>(&g.value.v))
                        t += std::to_string(*d);
                    else
                        t += std::to_string(std::get<long long>(g.value.v));
                }
                report.summary(code::kSkippedObject, t);
            }
        }
        if (!drawing.entityFailures.empty()) {
            std::string t = "entities left out (damaged / number out of range):";
            for (const auto& [type, c] : drawing.entityFailures)
                t += " " + type + " " + std::to_string(c[0]) + "/" + std::to_string(c[1]);
            report.summary(code::kSkippedObject, t);
        }
        for (const std::string& n : drawing.walkNotes)
            report.summary(code::kSkippedObject, n);
        if (!drawing.relocatedSamples.empty()) {
            std::string t = "moved objects (handle: map offset -> found offset):";
            for (const auto& smp : drawing.relocatedSamples) {
                char b[96];
                std::snprintf(b, sizeof b, " %llX: %lld -> %lld",
                              static_cast<unsigned long long>(smp[0]),
                              static_cast<long long>(smp[1]), static_cast<long long>(smp[2]));
                t += b;
            }
            report.summary(code::kSkippedObject, t);
        }
        if (!drawing.relocated.empty()) {
            std::size_t total = 0;
            std::string first;
            for (const auto& [shift, count] : drawing.relocated) {
                total += count;
                if (first.size() < 120)
                    first += (first.empty() ? "" : ", ") + std::to_string(shift) + " (" +
                             std::to_string(count) + ")";
            }
            report.summary(
                code::kSkippedObject,
                std::to_string(total) +
                    " object(s) were found away from the place the object map gives; shifts in "
                    "bytes: " +
                    first);
        }
        for (const auto& [type, t] : drawing.layoutCheck)
            report.summary(code::kSkippedObject,
                           "layout check, object type " + std::to_string(type) + ": " +
                               std::to_string(t[0]) + " end where expected, " +
                               std::to_string(t[1]) + " do not");
        {
            // All skipped types on one line, the most numerous first, so drawable ones stand out.
            std::vector<std::pair<std::size_t, std::string>> byCount;
            for (const auto& [name, count] : drawing.skippedTypes)
                byCount.emplace_back(count, name);
            std::sort(byCount.rbegin(), byCount.rend());
            std::string t = "object types not read yet (" + std::to_string(byCount.size()) +
                            " types, most numerous first):";
            for (std::size_t i = 0; i < byCount.size() && i < 150; ++i)
                t += " " + byCount[i].second + " x" + std::to_string(byCount[i].first);
            report.summary(code::kSkippedObject, t);
        }
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
            report.summary(code::kSkippedObject, line);
            // Model and paper space: which block headers carry those names and what they own.
            std::string spaces = "spaces:";
            std::map<std::uint64_t, std::size_t> insertsPer;
            for (const detail::EntityRec& e : drawing.entities)
                if (e.entmode == 0 && e.dxfType == "INSERT")
                    ++insertsPer[e.owner];
            for (const auto& [h, bl] : drawing.blocks) {
                if (bl.name.rfind("*Model_Space", 0) != 0 &&
                    bl.name.rfind("*Paper_Space", 0) != 0 &&
                    bl.name.rfind("*MODEL_SPACE", 0) != 0 && bl.name.rfind("*PAPER_SPACE", 0) != 0)
                    continue;
                std::snprintf(b, sizeof b, " %llX", static_cast<unsigned long long>(h));
                const auto c = perOwner.find(h);
                spaces += std::string(b) + " (" + bl.name + ", lists " +
                          std::to_string(bl.ownedCount) + (bl.membersRead ? "" : "?") + ", " +
                          std::to_string(c == perOwner.end() ? 0 : c->second) + " entities, " +
                          std::to_string(insertsPer[h]) + " inserts)";
            }
            spaces += "; member lists changed " + std::to_string(drawing.ownerFixed) +
                      " entity owners, " + std::to_string(drawing.membersMissing) +
                      " listed members were not decoded";
            std::size_t unowned = perOwner.count(0) ? perOwner[0] : 0;
            spaces += "; entities with no owner: " + std::to_string(unowned);
            report.summary(code::kSkippedObject, spaces);
            std::map<std::string, std::size_t> inModel;
            for (const detail::EntityRec& e : drawing.entities)
                if (e.entmode == 2)
                    ++inModel[e.dxfType];
            std::string mc = "model space (entity mode 2) holds:";
            for (const auto& [t, c] : inModel)
                mc += " " + t + " " + std::to_string(c);
            report.summary(code::kSkippedObject, mc);
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
                        const bool paperOwner = o != drawing.blocks.end() &&
                                                o->second.name.rfind("*Paper_Space", 0) == 0;
                        const bool modelOwner = o != drawing.blocks.end() &&
                                                o->second.name.rfind("*Model_Space", 0) == 0;
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
                report.summary(code::kSkippedObject, "reachable from model space: " + reach(false));
                report.summary(code::kSkippedObject, "reachable from paper spaces: " + reach(true));
            }
            {
                // Every viewport: where it sits on its sheet and what it looks at.
                auto val = [](const detail::EntityRec& e, int code) {
                    for (const detail::Group& g : e.groups) {
                        if (g.code != code)
                            continue;
                        if (const double* v = std::get_if<double>(&g.value.v))
                            return *v;
                        if (const long long* i = std::get_if<long long>(&g.value.v))
                            return static_cast<double>(*i);
                    }
                    return 0.0;
                };
                {
                    std::string off = "layers not to plot:", frz = "layers frozen:",
                                hid = "layers off:";
                    std::size_t no = 0, nf = 0, nh = 0;
                    for (const auto& kv : drawing.layers) {
                        if (!kv.second.plot && no++ < 30)
                            off += " " + kv.second.name;
                        if (kv.second.frozen && nf++ < 30)
                            frz += " " + kv.second.name;
                        if (kv.second.color < 0 && nh++ < 30)
                            hid += " " + kv.second.name;
                    }
                    report.summary(code::kSkippedObject, off + " (" + std::to_string(no) + " of " +
                                                             std::to_string(drawing.layers.size()) +
                                                             ")");
                    report.summary(code::kSkippedObject, frz + " (" + std::to_string(nf) + ")");
                    report.summary(code::kSkippedObject, hid + " (" + std::to_string(nh) + ")");
                }
                {
                    // Layers that carry an extension dictionary, and the state of xref layers.
                    std::string xl = "layer extension dictionaries:";
                    std::size_t withDict = 0, shown = 0;
                    std::map<std::string, std::size_t> names;
                    for (const auto& [lh, l] : drawing.layers) {
                        const auto xd = drawing.dictionaries.find(l.xdic);
                        if (xd == drawing.dictionaries.end())
                            continue;
                        ++withDict;
                        for (const auto& [nm, th] : xd->second) {
                            ++names[nm];
                            if (shown++ < 40) {
                                const auto ty = drawing.rawType.find(th);
                                xl += " " + l.name + "[" + nm + "]" + std::to_string(th) + ":" +
                                      (ty == drawing.rawType.end() ? std::string("absent")
                                                                   : std::to_string(ty->second));
                            }
                        }
                    }
                    report.summary(code::kSkippedObject,
                                   xl + " (" + std::to_string(withDict) + " layers with one)");
                    {
                        std::map<std::string, std::pair<std::size_t, std::uint64_t>> all;
                        for (const auto& [dh, items] : drawing.dictionaries)
                            for (const auto& [nm, th] : items) {
                                auto& slot = all[nm];
                                if (slot.first++ == 0)
                                    slot.second = dh;
                            }
                        std::string al =
                            "all dictionary entry names (name x count, first dictionary):";
                        for (const auto& [nm, cs] : all) {
                            char hb[40];
                            std::snprintf(hb, sizeof hb, " x%zu @%llX", cs.first,
                                          static_cast<unsigned long long>(cs.second));
                            al += " [" + nm + "]" + hb;
                        }
                        report.summary(code::kSkippedObject, al);
                    }
                    {
                        std::string el = "layers with extended data:";
                        std::size_t cnt = 0;
                        for (const auto& [lh, l] : drawing.layers)
                            if (!l.eed.empty() && cnt++ < 12)
                                el += " " + l.name + " {" + l.eed + "}";
                        report.summary(code::kSkippedObject, el + " (" + std::to_string(cnt) + ")");
                        std::map<std::string, std::pair<std::size_t, std::string>> kinds;
                        for (const auto& [lh, l] : drawing.layers)
                            if (!l.eed.empty()) {
                                auto& k = kinds[l.eed];
                                if (k.first++ == 0)
                                    k.second = l.name;
                            }
                        std::string kl = "distinct layer extended data (hex x count, first layer):";
                        for (const auto& [hx, k] : kinds)
                            kl += " {" + hx + "} x" + std::to_string(k.first) + " " + k.second;
                        report.summary(code::kSkippedObject, kl);
                    }
                    std::string nl = "layer extension dictionary entry names:";
                    for (const auto& [nm, c] : names)
                        nl += " " + nm + " x" + std::to_string(c);
                    report.summary(code::kSkippedObject, nl);
                    std::string xr = "xref layers (name flags colour):";
                    std::size_t k = 0;
                    for (const auto& [lh, l] : drawing.layers)
                        if (l.name.find('|') != std::string::npos && k++ < 60)
                            xr += " " + l.name + " " + std::to_string(l.flags) +
                                  (l.frozen ? "F" : "") + (l.plot ? "" : "N") + " " +
                                  std::to_string(l.color);
                    report.summary(code::kSkippedObject, xr + " (" + std::to_string(k) + ")");
                }
                int n = 0;
                for (const detail::EntityRec& e : drawing.entities) {
                    if (e.dxfType != "VIEWPORT" || ++n > 40)
                        continue;
                    const auto o = drawing.blocks.find(e.owner);
                    char t[480];
                    std::snprintf(
                        t, sizeof t,
                        "viewport %llX on %s: centre %.1f,%.1f size %.1f x %.1f; target "
                        "%.1f,%.1f view centre %.1f,%.1f height %.1f; direction %.3f,%.3f,%.3f "
                        "twist "
                        "%.3f; %u frozen stored, %zu handles read; %s",
                        static_cast<unsigned long long>(e.handle),
                        e.entmode == 0 && o != drawing.blocks.end() ? o->second.name.c_str()
                                                                    : "paper space",
                        val(e, 10), val(e, 20), val(e, 40), val(e, 41), val(e, 17), val(e, 27),
                        val(e, 12), val(e, 22), val(e, 45), val(e, 16), val(e, 26), val(e, 36),
                        val(e, 51), static_cast<unsigned>(e.frozenCount), e.frozenLayers.size(),
                        val(e, 68) != 0 ? "on" : "off");
                    report.summary(code::kSkippedObject, t);
                    {
                        char xb[64];
                        std::snprintf(xb, sizeof xb, "viewport %llX extension dictionary %llX:",
                                      static_cast<unsigned long long>(e.handle),
                                      static_cast<unsigned long long>(e.xdic));
                        std::string xl = xb;
                        const auto xd = drawing.dictionaries.find(e.xdic);
                        if (xd == drawing.dictionaries.end())
                            xl += " not read";
                        else
                            for (const auto& [nm, th] : xd->second) {
                                const auto ty = drawing.rawType.find(th);
                                char hb[48];
                                std::snprintf(hb, sizeof hb, " %llX type ",
                                              static_cast<unsigned long long>(th));
                                xl += " [" + nm + "]" + hb +
                                      (ty == drawing.rawType.end() ? std::string("absent")
                                                                   : std::to_string(ty->second));
                            }
                        report.summary(code::kSkippedObject, xl);
                    }
                    if (!e.frozenLayers.empty()) {
                        std::string f = "viewport " + std::to_string(e.handle) + " frozen layers:";
                        for (std::size_t i = 0; i < e.frozenLayers.size() && i < 24; ++i) {
                            const auto l = drawing.layers.find(e.frozenLayers[i]);
                            f += " " +
                                 (l != drawing.layers.end() ? l->second.name : std::string("?"));
                        }
                        report.summary(code::kSkippedObject, f);
                    }
                    if (val(e, 40) > 0 && val(e, 45) > 0 && val(e, 17) != 0) {
                        // How much model space data lies inside what this viewport shows.
                        const double h = val(e, 45), w = h * val(e, 40) / val(e, 41);
                        const double cx = val(e, 17) + val(e, 12), cy = val(e, 27) + val(e, 22);
                        std::size_t inside = 0, total = 0, elsewhere = 0;
                        double bx0 = 1e300, by0 = 1e300, bx1 = -1e300, by1 = -1e300;
                        for (const detail::EntityRec& m : drawing.entities) {
                            const auto mo = drawing.blocks.find(m.owner);
                            const bool model =
                                m.entmode == 2 || (m.entmode == 0 && mo != drawing.blocks.end() &&
                                                   mo->second.name == "*Model_Space");
                            if (m.dxfType == "VIEWPORT")
                                continue;
                            const double px = val(m, 10), py = val(m, 20);
                            if (!model) {
                                if ((px != 0 || py != 0) && std::abs(px - cx) <= w / 2 &&
                                    std::abs(py - cy) <= h / 2)
                                    ++elsewhere;
                                continue;
                            }
                            if (px == 0 && py == 0)
                                continue;
                            ++total;
                            bx0 = std::min(bx0, px);
                            by0 = std::min(by0, py);
                            bx1 = std::max(bx1, px);
                            by1 = std::max(by1, py);
                            if (std::abs(px - cx) <= w / 2 && std::abs(py - cy) <= h / 2)
                                ++inside;
                        }
                        char u[300];
                        std::snprintf(
                            u, sizeof u,
                            "  viewport shows model %.0f,%.0f size %.0f x %.0f: %zu of %zu "
                            "model entities start inside (%zu more in other blocks); they span "
                            "%.0f,%.0f to %.0f,%.0f",
                            cx, cy, w, h, inside, total, elsewhere, bx0, by0, bx1, by1);
                        report.summary(code::kSkippedObject, u);
                    }
                }
            }
            {
                std::string ls = "layout objects:";
                for (const detail::LayoutRec& l : drawing.layoutObjects) {
                    char t[520];
                    std::snprintf(
                        t, sizeof t,
                        " %llX block %llX tab %d %s[%s] paper %.1f x %.1f rot %d margins %.1f %.1f "
                        "%.1f %.1f origin %.1f %.1f limits %.1f %.1f %.1f %.1f base %.1f %.1f "
                        "extents %.1f %.1f %.1f %.1f;",
                        static_cast<unsigned long long>(l.handle),
                        static_cast<unsigned long long>(l.block), l.tab,
                        l.damaged ? "(dictionary) " : "", l.name.c_str(), l.width, l.height,
                        l.rotation, l.margins[0], l.margins[1], l.margins[2], l.margins[3],
                        l.plotOrigin[0], l.plotOrigin[1], l.limits[0], l.limits[1], l.limits[2],
                        l.limits[3], l.insBase[0], l.insBase[1], l.extents[0], l.extents[1],
                        l.extents[3], l.extents[4]);
                    ls += t;
                    ls += " style sheet [" + l.styleSheet + "];";
                }
                for (const auto& [dh, items] : drawing.dictionaries)
                    for (const auto& [name, target] : items)
                        if (name == "ACAD_LAYOUT") {
                            const auto d = drawing.dictionaries.find(target);
                            if (d == drawing.dictionaries.end())
                                continue;
                            std::string entriesLine = "layout dictionary entries:";
                            for (const auto& [ln, lh] : d->second) {
                                const auto t = drawing.rawType.find(lh);
                                char hb[64];
                                std::snprintf(hb, sizeof hb, " %llX type ",
                                              static_cast<unsigned long long>(lh));
                                entriesLine +=
                                    " [" + ln + "]" + hb +
                                    (t == drawing.rawType.end() ? std::string("absent")
                                                                : std::to_string(t->second));
                            }
                            report.summary(code::kSkippedObject, entriesLine);
                        }
                for (const std::string& d : drawing.layoutDiag)
                    report.summary(code::kSkippedObject, d);
                ls += "; layout objects met: " + std::to_string(drawing.layoutSeen) +
                      "; dictionaries read: " + std::to_string(drawing.dictionaries.size());
                report.summary(code::kSkippedObject, ls);
            }
            {
                // The entities with the widest extent (what dominates "zoom extents").
                struct Wide {
                    double span = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                    const detail::EntityRec* e = nullptr;
                };
                std::vector<Wide> wide;
                for (const detail::EntityRec& e : drawing.entities) {
                    if (e.dxfType == "VIEWPORT" || e.dxfType == "INSERT")
                        continue;
                    double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
                    bool any = false;
                    for (const detail::Group& g : e.groups) {
                        const double* dv = std::get_if<double>(&g.value.v);
                        if (!dv || g.code < 10 || g.code > 23 || (g.code > 13 && g.code < 20))
                            continue;
                        if ((g.code == 12 || g.code == 22) && e.dxfType == "DIMENSION")
                            continue;  // the clone insertion point, not geometry
                        // Codes 11 and 21 are points only for a few types (SOLID, LINE, ...).
                        if ((g.code == 11 || g.code == 21) &&
                            (e.dxfType == "MTEXT" || e.dxfType == "TEXT" || e.dxfType == "ATTRIB"))
                            continue;
                        const int axis = g.code >= 20 ? 1 : 0;
                        lo[axis] = std::min(lo[axis], *dv);
                        hi[axis] = std::max(hi[axis], *dv);
                        any = true;
                    }
                    if (!any || lo[0] > hi[0] || lo[1] > hi[1])
                        continue;
                    wide.push_back(
                        {std::max(hi[0] - lo[0], hi[1] - lo[1]), lo[0], lo[1], hi[0], hi[1], &e});
                }
                std::sort(wide.begin(), wide.end(),
                          [](const Wide& lhs, const Wide& rhs) { return lhs.span > rhs.span; });
                // The widest filled shapes get their own lines: a huge fill is easy to see on
                // screen.
                std::vector<Wide> fills;
                for (const Wide& w : wide)
                    if (w.e->dxfType == "SOLID" || w.e->dxfType == "TRACE" ||
                        w.e->dxfType == "HATCH")
                        fills.push_back(w);
                if (fills.size() > 4)
                    fills.resize(4);
                std::vector<Wide> shown(wide.begin(),
                                        wide.begin() + static_cast<std::ptrdiff_t>(
                                                           std::min<std::size_t>(wide.size(), 6)));
                shown.insert(shown.end(), fills.begin(), fills.end());
                for (std::size_t i = 0; i < shown.size(); ++i) {
                    const Wide& w = shown[i];
                    const auto o = drawing.blocks.find(w.e->owner);
                    char t[300];
                    std::snprintf(
                        t, sizeof t,
                        "wide entity %llX %s in %s: %.1f,%.1f to %.1f,%.1f; colour %d%s",
                        static_cast<unsigned long long>(w.e->handle), w.e->dxfType.c_str(),
                        w.e->entmode == 0 && o != drawing.blocks.end() ? o->second.name.c_str()
                                                                       : "a space",
                        w.x0, w.y0, w.x1, w.y1, w.e->color,
                        w.e->trueColor >= 0 ? " (true colour)" : "");
                    report.summary(code::kSkippedObject, t);
                    if (i < 3 || i >= 6) {
                        std::string gl = "  groups:";
                        for (const detail::Group& g : w.e->groups) {
                            gl += " " + std::to_string(g.code) + "=";
                            if (const double* dv = std::get_if<double>(&g.value.v))
                                gl += std::to_string(*dv);
                            else if (const long long* n = std::get_if<long long>(&g.value.v))
                                gl += std::to_string(*n);
                            else
                                gl += "\"" + std::get<std::string>(g.value.v).substr(0, 24) + "\"";
                        }
                        report.summary(code::kSkippedObject, gl);
                    }
                }
            }
            {
                // Text with the largest height (a misread height would draw giant glyphs).
                std::vector<std::pair<double, const detail::EntityRec*>> tall;
                for (const detail::EntityRec& e : drawing.entities) {
                    if (e.dxfType != "TEXT" && e.dxfType != "MTEXT" && e.dxfType != "ATTRIB" &&
                        e.dxfType != "ATTDEF")
                        continue;
                    for (const detail::Group& g : e.groups)
                        if (g.code == 40)
                            if (const double* h = std::get_if<double>(&g.value.v))
                                tall.emplace_back(*h, &e);
                }
                std::sort(tall.begin(), tall.end(),
                          [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
                for (std::size_t i = 0; i < tall.size() && i < 5; ++i) {
                    const detail::EntityRec& e = *tall[i].second;
                    const auto o = drawing.blocks.find(e.owner);
                    std::string txt =
                        "text " + std::to_string(i) + " " + e.dxfType + " in " +
                        (e.entmode == 0 && o != drawing.blocks.end() ? o->second.name : "a space") +
                        ", height " + std::to_string(tall[i].first) + ", groups:";
                    for (const detail::Group& g : e.groups) {
                        txt += " " + std::to_string(g.code) + "=";
                        if (const double* dv = std::get_if<double>(&g.value.v))
                            txt += std::to_string(*dv);
                        else if (const long long* n = std::get_if<long long>(&g.value.v))
                            txt += std::to_string(*n);
                        else
                            txt += "\"" + std::get<std::string>(g.value.v).substr(0, 30) + "\"";
                    }
                    report.summary(code::kSkippedObject, txt);
                }
            }
            {
                // Dimension styles (text height, scale, arrow size decide how dimensions draw) and
                //
                std::size_t shown = 0;
                for (const auto& [dh, ds] : drawing.dimStyles) {
                    if (++shown > 6)
                        break;
                    std::string dsLine = "dimstyle \"" + ds.name + "\":";
                    for (const detail::Group& g : ds.groups) {
                        if (g.code != 40 && g.code != 41 && g.code != 42 && g.code != 43 &&
                            g.code != 44 && g.code != 45 && g.code != 140 && g.code != 141 &&
                            g.code != 142 && g.code != 143 && g.code != 147 && g.code != 144)
                            continue;
                        dsLine += " " + std::to_string(g.code) + "=";
                        if (const double* dv = std::get_if<double>(&g.value.v))
                            dsLine += std::to_string(*dv);
                        else if (const long long* n = std::get_if<long long>(&g.value.v))
                            dsLine += std::to_string(*n);
                    }
                    const auto ts = drawing.styles.find(ds.textStyle);
                    if (ts != drawing.styles.end())
                        dsLine += " text style \"" + ts->second.name +
                                  "\" height=" + std::to_string(ts->second.height);
                    report.summary(code::kSkippedObject, dsLine);
                }
            }
            std::string ins = "inserts per owner:";
            std::vector<std::pair<std::size_t, std::uint64_t>> topIns;
            for (const auto& [h, c] : insertsPer)
                topIns.emplace_back(c, h);
            std::sort(topIns.rbegin(), topIns.rend());
            for (std::size_t i = 0; i < topIns.size() && i < 6; ++i) {
                std::snprintf(b, sizeof b, " %llX",
                              static_cast<unsigned long long>(topIns[i].second));
                const auto it = drawing.blocks.find(topIns[i].second);
                ins += std::string(b) + " (" +
                       (it == drawing.blocks.end() ? "no block" : it->second.name) + ", " +
                       std::to_string(topIns[i].first) + ")";
            }
            report.summary(code::kSkippedObject, ins);
        }
    }  // options.diagnostics
    const long long diagMs = msSince(tDiag);
    const auto tEmit = Clock::now();
    const dxf::GroupBuffer groups = detail::emitGroups(drawing);
    const long long emitMs = msSince(tEmit);
    const auto tImport = Clock::now();
    auto result = dxf::importDxfGroups(groups, options, report);
    if (timings) {
        auto msd = [](Clock::time_point a, Clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const auto now = Clock::now();
        for (const auto& st : drawing.stageMs)
            timings->push_back(st);
        if (drawing.stageMs.empty())
            timings->emplace_back("read", msd(tStart, tDiag));
        timings->emplace_back("notes and diagnostics", msd(tDiag, tEmit));
        timings->emplace_back("convert to DXF groups", msd(tEmit, tImport));
        timings->emplace_back("import the groups", msd(tImport, now));
    }
    report.summary(code::kSkippedObject,
                   "timing (ms): read " + std::to_string(readMs) + ", diagnostics " +
                       std::to_string(diagMs) + ", to DXF text " + std::to_string(emitMs) + " (" +
                       std::to_string(groups.size()) + " groups), DXF import " +
                       std::to_string(msSince(tImport)) + ", total " +
                       std::to_string(msSince(tStart)));
    if (result) {
        report.summary(code::kSkippedObject,
                       "layouts imported: " + std::to_string(result.value().summary.layouts));
        result.value().summary.acadVer = v.code;
        result.value().summary.version = v.name;
    }
    return result;
}

std::string readerRevision() {
    return detail::readerRevision();
}

std::string searchCuts(const std::vector<std::span<const std::byte>>& files) {
    std::string out;
    auto& table = detail::lz21Cuts();
    // (imported entities, -warnings) summed over the files.
    auto score = [&]() {
        long long imported = 0, warnings = 0;
        for (const auto& f : files) {
            dxf::Report report;
            auto r = importDwg(f, dxf::ImportOptions{}, report);
            warnings += static_cast<long long>(report.count(dxf::Severity::Warning));
            if (r)
                for (const auto& [name, c] : r.value().summary.entities)
                    imported += static_cast<long long>(c.imported);
        }
        return std::make_pair(imported, -warnings);
    };
    // Every way to cut the length into blocks of 1, 4 and 8 bytes (at most seven blocks), plus the
    // other orders of the blocks the table has now.
    auto arrangements = [](const std::vector<std::uint8_t>& base) {
        std::vector<std::vector<std::uint8_t>> all;
        std::size_t total = 0;
        for (auto b : base)
            total += b;
        std::vector<std::uint8_t> cur;
        auto grow = [&](auto&& self, std::size_t left) -> void {
            if (left == 0) {
                all.push_back(cur);
                return;
            }
            if (cur.size() >= 7)
                return;
            for (std::uint8_t part : {8, 4, 1})
                if (part <= left) {
                    cur.push_back(part);
                    self(self, left - part);
                    cur.pop_back();
                }
        };
        grow(grow, total);
        std::vector<std::uint8_t> v = base;
        std::sort(v.begin(), v.end());
        do {
            if (std::find(all.begin(), all.end(), v) == all.end())
                all.push_back(v);
        } while (std::next_permutation(v.begin(), v.end()));
        return all;
    };
    auto text = [](const std::vector<std::uint8_t>& v) {
        std::string t = "[";
        for (std::size_t i = 0; i < v.size(); ++i)
            t += (i ? "," : "") + std::to_string(v[i]);
        return t + "]";
    };
    auto best = score();
    out += "start: imported " + std::to_string(best.first) + ", warnings " +
           std::to_string(-best.second) + "\n";
    for (int pass = 0; pass < 1; ++pass) {
        for (std::size_t len : {21u, 25u, 29u, 31u, 26u, 28u, 19u, 11u}) {
            const std::vector<std::uint8_t> keep = table[len];
            std::vector<std::uint8_t> winner = keep;
            std::string line = "length " + std::to_string(len) + ":";
            for (const auto& cand : arrangements(keep)) {
                table[len] = cand;
                const auto s = score();
                line += " " + text(cand) + "=" + std::to_string(s.first) + "/" +
                        std::to_string(-s.second);
                if (s > best) {
                    best = s;
                    winner = cand;
                }
            }
            table[len] = winner;
            out += line + " -> " + text(winner) + "\n";
        }
    }
    out += "final table:";
    for (std::size_t len = 1; len < table.size(); ++len)
        out += " " + std::to_string(len) + ":" + text(table[len]);
    out += "\nfinal: imported " + std::to_string(best.first) + ", warnings " +
           std::to_string(-best.second) + "\n";
    return out;
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
    if (v.version >= Version::R2004) {
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
            if (const auto hdr = secs.find("AcDb:Header"); hdr != secs.end()) {
                // The whole header: its layout differs between releases and is worked out from it.
                std::string all;
                char b[4];
                for (std::size_t i = 0; i < hdr->second.size() && i < 4000; ++i) {
                    std::snprintf(b, sizeof b, "%02X", hdr->second[i]);
                    all += b;
                }
                line("header bytes: " + all);
            }
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
                fmt.r2007 = v.version >= Version::R2007;
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
    for (int pass = 0; pass < 1; ++pass) {
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
