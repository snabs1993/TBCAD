// ImportSummary::toJson - the conversion report written by tbcad-convert --report.
#include <cstdio>

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
             ",\"skipped\":" + std::to_string(c.skipped) + "}";
    }
    o += "},\"summary\":{\"layers\":" + std::to_string(layers) +
         ",\"blocks\":" + std::to_string(blocks) +
         ",\"outsideWorkArea\":" + std::to_string(outsideWorkArea) +
         ",\"beyondHardCap\":" + std::to_string(beyondHardCap) + "},\"report\":" + report.toJson() +
         "}";
    return o;
}

}  // namespace tbcad::dxf
