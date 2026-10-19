#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "tbcad/dxf/reader.h"

namespace tbcad::dxf::testing {

// Builds a DXF text from (code, value) pairs.
struct Pairs {
    std::string text;
    Pairs& g(int code, const std::string& value) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%3d\n", code);
        text += buf;
        text += value + "\n";
        return *this;
    }
    Pairs& g(int code, double value) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.17g", value);
        return g(code, std::string(buf));
    }
    Pairs& g(int code, int value) { return g(code, std::to_string(value)); }
    Pairs& g(int code, const char* value) { return g(code, std::string(value)); }
    Pairs& section(const std::string& name) { return g(0, "SECTION").g(2, name); }
    Pairs& end() { return g(0, "ENDSEC"); }
};

// A DXF with only an ENTITIES section (units assumed mm).
inline std::string entitiesOnly(const std::string& body) {
    Pairs p;
    p.section("ENTITIES");
    return p.text + body + Pairs().end().g(0, "EOF").text;
}

struct Loaded {
    ImportResult result;
    Report report;
    const Document& doc() const { return *result.document; }
};

inline Loaded load(const std::string& text, const ImportOptions& options = {}) {
    Loaded l;
    Result<ImportResult> r = importDxf(bytesOf(text), options, l.report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    l.result = std::move(r.value());
    return l;
}

inline Loaded loadCorpus(const std::string& name, const ImportOptions& options = {}) {
    std::vector<std::byte> bytes = readFile(corpusPath("reader/" + name));
    REQUIRE_MESSAGE(!bytes.empty(), "missing corpus file " << name);
    Loaded l;
    Result<ImportResult> r = importDxf(bytes, options, l.report);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    l.result = std::move(r.value());
    return l;
}

// Model space entities of one kind, in draw order.
template <class T>
std::vector<const T*> modelEntities(const Document& doc) {
    std::vector<const T*> out;
    for (EntityId id : doc.modelSpace()) {
        if (const T* d = entityAs<T>(*doc.find(id)))
            out.push_back(d);
    }
    return out;
}

// Entities of one kind in a block definition, in order.
template <class T>
std::vector<const T*> blockEntities(const Document& doc, BlockId block) {
    std::vector<const T*> out;
    if (const BlockDef* def = doc.block(block)) {
        for (EntityId id : def->entities) {
            if (const T* d = entityAs<T>(*doc.find(id)))
                out.push_back(d);
        }
    }
    return out;
}

inline const Entity* firstOfKind(const Document& doc, EntityKind k) {
    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        if (kind(*e) == k)
            return e;
    }
    return nullptr;
}

inline bool near(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}
inline bool near(Vec2 a, Vec2 b, double eps = 1e-9) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps);
}

}  // namespace tbcad::dxf::testing
