// ImportSummary::toJson - the conversion report written by tbcad-convert --report.
#include <cstdio>
#include <map>

#include "tbcad/dxf/reader.h"

namespace tbcad::dxf {

namespace {

void appendString(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(ch);  // already valid UTF-8 (names come from decodeText)
                }
        }
    }
    out.push_back('"');
}

std::string number(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.12g", v);
    return buf;
}

// The kinds of difference a comparison shows (see DimComparison).
struct DimFaults {
    bool text = false, offset = false, height = false, lines = false, arrows = false;
    bool any() const { return text || offset || height || lines || arrows; }
};

DimFaults faultsOf(const DimComparison& c) {
    DimFaults f;
    f.text = !c.textMatch;
    f.offset = c.textOffset > 0.5;
    f.height = c.heightRatio < 0.95 || c.heightRatio > 1.05;
    f.lines = c.strayStored > 0.05 || c.strayGenerated > 0.05;
    f.arrows = c.arrowsUnmatched > 0 || (c.arrowsStored > 0 && c.arrowsStored != c.arrowsGenerated);
    return f;
}

// {"count":..,"differ":..,"byKind":{kind:{"count","text","offset","height","lines","arrows"}},
//  "samples":[the first differing dimensions of each kind of fault]}
std::string dimCompareJson(const std::vector<DimComparison>& all) {
    struct Tally {
        std::size_t count = 0, differ = 0, text = 0, offset = 0, height = 0, lines = 0, arrows = 0,
                    stale = 0;
    };
    std::map<std::string, Tally> byKind;
    Tally total;
    std::string samples;
    std::size_t sampled = 0;
    std::map<std::string, int> perSignature;
    for (const DimComparison& c : all) {
        // A stale stored block is counted apart and not compared further (see DimComparison).
        const DimFaults f = c.stale ? DimFaults{} : faultsOf(c);
        for (Tally* t : {&byKind[c.kind], &total}) {
            ++t->count;
            t->stale += c.stale;
            t->differ += f.any();
            t->text += f.text;
            t->offset += f.offset;
            t->height += f.height;
            t->lines += f.lines;
            t->arrows += f.arrows;
        }
        // At most three samples of each kind of fault, so one common fault does not hide the
        // others.
        if (!f.any() || sampled >= 60)
            continue;
        const std::string signature = c.kind + (f.text ? "t" : "") + (f.offset ? "o" : "") +
                                      (f.height ? "h" : "") + (f.lines ? "l" : "") +
                                      (f.arrows ? "a" : "");
        if (++perSignature[signature] > 3)
            continue;
        ++sampled;
        if (!samples.empty())
            samples += ",";
        samples += "{\"handle\":\"" + [&] {
            char buf[24];
            std::snprintf(buf, sizeof buf, "%llX", static_cast<unsigned long long>(c.handle));
            return std::string(buf);
        }() + "\",\"line\":" + std::to_string(c.line) + ",\"kind\":";
        appendString(samples, c.kind);
        samples += ",\"stored\":";
        appendString(samples, c.storedText);
        samples += ",\"generated\":";
        appendString(samples, c.generatedText);
        samples += ",\"textOffset\":" + number(c.textOffset) +
                   ",\"heightRatio\":" + number(c.heightRatio) +
                   ",\"strayStored\":" + number(c.strayStored) +
                   ",\"strayGenerated\":" + number(c.strayGenerated) +
                   ",\"arrows\":[" + std::to_string(c.arrowsStored) + "," +
                   std::to_string(c.arrowsGenerated) + "," + std::to_string(c.arrowsUnmatched) +
                   "],\"detail\":";
        appendString(samples, c.detail);
        samples += "}";
    }
    auto tally = [](const Tally& t) {
        return "{\"count\":" + std::to_string(t.count) + ",\"differ\":" + std::to_string(t.differ) +
               ",\"text\":" + std::to_string(t.text) + ",\"offset\":" + std::to_string(t.offset) +
               ",\"height\":" + std::to_string(t.height) + ",\"lines\":" + std::to_string(t.lines) +
               ",\"arrows\":" + std::to_string(t.arrows) +
               ",\"stale\":" + std::to_string(t.stale) + "}";
    };
    std::string o = "{\"total\":" + tally(total) + ",\"byKind\":{";
    bool first = true;
    for (const auto& [k, t] : byKind) {
        if (!first)
            o += ",";
        first = false;
        appendString(o, k);
        o += ":" + tally(t);
    }
    return o + "},\"samples\":[" + samples + "]}";
}

}  // namespace

std::string ImportSummary::toJson(const Report& report) const {
    std::string o = "{\"input\":{\"acadver\":";
    appendString(o, acadVer);
    o += ",\"version\":";
    appendString(o, version);
    o += ",\"binary\":";
    o += binary ? "true" : "false";
    o += ",\"encoding\":";
    appendString(o, encodingName(encoding));
    o += ",\"encodingGuessed\":";
    o += encodingGuessed ? "true" : "false";
    o += "},\"units\":{\"insunits\":" + std::to_string(insunits) + ",\"name\":";
    appendString(o, unitsName);
    o += ",\"scaleToMm\":" + number(unitScale) + ",\"assumed\":";
    o += unitsAssumed ? "true" : "false";
    o += "},\"entities\":{";
    bool first = true;
    for (const auto& [type, c] : entities) {
        if (!first)
            o += ",";
        first = false;
        appendString(o, type);
        o += ":{\"read\":" + std::to_string(c.read) +
             ",\"imported\":" + std::to_string(c.imported) +
             ",\"skipped\":" + std::to_string(c.skipped);
        if (c.empty)
            o += ",\"empty\":" + std::to_string(c.empty);
        o += "}";
    }
    o += "},\"summary\":{\"layers\":" + std::to_string(layers) +
         ",\"blocks\":" + std::to_string(blocks) +
         ",\"outsideWorkArea\":" + std::to_string(outsideWorkArea) +
         ",\"beyondHardCap\":" + std::to_string(beyondHardCap) + "}";
    if (!dimCompare.empty())
        o += ",\"dimCompare\":" + dimCompareJson(dimCompare);
    o += ",\"report\":" + report.toJson() + "}";
    return o;
}

}  // namespace tbcad::dxf
