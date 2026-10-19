// DIMSTYLE variables that change how a dimension is measured or laid out: DIMLFAC, DIMATFIT and
// DIMTMOVE. Each shows in the drawing, undoes, and survives .tbcad, DXF and the change journal.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/dim/dim.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, (registerAllCommands(reg), reg)) {
        REQUIRE(host.run("DIMSTYLE", {"New", "S", "Done"}).finished);
        REQUIRE(host.run("DIMSTYLE", {"Current", "S", "Done"}).finished);
    }
    bool edit(const char* key, const char* value) {
        return host.run("DIMSTYLE", {"Edit", "S", key, value, "Done", "Done"}).finished;
    }
    EntityId dimension() {
        EntityId id{};
        doc.forEachEntity([&](const Entity& e) {
            if (entityAs<DimensionData>(e))
                id = e.common.id;
        });
        return id;
    }
    dim::Layout layout(EntityId id = {}) {
        if (!id)
            id = dimension();
        const auto* d = entityAs<DimensionData>(*doc.find(id));
        return dim::layoutDimension(doc, *d);
    }
    std::string text(const Document& d, EntityId id) {
        const auto* dd = entityAs<DimensionData>(*d.find(id));
        return dim::dimensionText(dim::styleOf(d, dd->style), *dd);
    }
};

// Exports to DXF and reads it back.
std::unique_ptr<Document> viaDxf(const Document& doc) {
    dxf::Report er;
    auto out = dxf::exportDxf(doc, dxf::ExportOptions{}, er);
    REQUIRE(out.ok());
    const std::string& t = out.value().bytes;
    std::vector<std::byte> bytes(t.size());
    for (std::size_t i = 0; i < t.size(); ++i)
        bytes[i] = static_cast<std::byte>(t[i]);
    dxf::Report ir;
    auto in = dxf::importDxf(bytes, dxf::ImportOptions{}, ir);
    REQUIRE(in.ok());
    return std::move(in.value().document);
}

std::unique_ptr<Document> viaTbcad(const Document& doc) {
    // Each test case runs as its own process under ctest -j: a shared name would race.
    const auto file = std::filesystem::temp_directory_path() /
                      ("tbcad_dimvars_" + std::to_string(std::random_device{}()) + ".tbcad");
    REQUIRE(io::writeTbcad(doc, file, io::WriteOptions{.keepBackup = false}).ok());
    io::IoReport rep;
    auto back = io::readTbcad(file, rep);
    std::error_code ec;
    std::filesystem::remove(file, ec);
    REQUIRE(back.ok());
    return std::move(back.value());
}

bool textInside(const dim::Layout& L, double lenX) {
    return L.text.center.x > 0 && L.text.center.x < lenX;
}

}  // namespace

TEST_CASE("DIMLFAC scales the measurement shown, never the geometry, it undoes and round trips") {
    Rig r;
    REQUIRE(r.host.run("DIMLINEAR", {"0,0", "100,0", "50,20"}).finished);
    const EntityId id = r.dimension();
    REQUIRE(id);
    CHECK(r.text(r.doc, id) == "100.00");
    REQUIRE(r.edit("LinearFactor", "0.5"));
    CHECK(r.text(r.doc, id) == "50.00");
    CHECK(r.doc.dimStyleByName("S")->linearFactor == doctest::Approx(0.5));
    auto tb = viaTbcad(r.doc);
    CHECK(canonicalDump(*tb) == canonicalDump(r.doc));
    auto dx = viaDxf(r.doc);
    EntityId got{};
    dx->forEachEntity([&](const Entity& e) {
        if (entityAs<DimensionData>(e))
            got = e.common.id;
    });
    REQUIRE(got);
    CHECK(r.text(*dx, got) == "50.00");
    CHECK(dx->dimStyleByName("S")->linearFactor == doctest::Approx(0.5));

    REQUIRE(r.host.undo());
    CHECK(r.text(r.doc, id) == "100.00");
    REQUIRE(r.host.redo());
    CHECK(r.text(r.doc, id) == "50.00");

    // Bad values change nothing.
    const std::string before = canonicalDump(r.doc);
    CHECK(r.host.run("DIMSTYLE", {"Edit", "S", "LinearFactor", "0", "Done", "Done"}).finished);
    CHECK(canonicalDump(r.doc) == before);
    CHECK(r.host.run("DIMSTYLE", {"Edit", "S", "LinearFactor", "abc", "Done", "Done"}).finished);
    CHECK(canonicalDump(r.doc) == before);
}

TEST_CASE("DIMATFIT decides what leaves the extension lines first when nothing fits") {
    Rig r;
    // Find a length where the text fits between the lines but the arrows then do not.
    double len = 0;
    for (double l = 6; l < 60; l += 0.5) {
        Rig probe;
        REQUIRE(probe.host.run("DIMLINEAR", {"0,0", std::to_string(l) + ",0", "1,10"}).finished);
        const auto L = probe.layout();
        if (L.text.center.x > 0 && L.text.center.x < l) {
            // inside; the first length that is only just big enough for the text
            len = l;
            break;
        }
    }
    REQUIRE(len > 0);
    // One step below that length, the text no longer fits.
    const double tight = len - 0.5;
    REQUIRE(r.host.run("DIMLINEAR", {"0,0", std::to_string(tight) + ",0", "1,10"}).finished);

    CHECK(r.edit("Fit", "Best"));
    const bool bestInside = textInside(r.layout(), tight);
    CHECK(r.edit("Fit", "ArrowsFirst"));
    CHECK(textInside(r.layout(), tight) == bestInside);  // the same rule as Best
    CHECK(r.edit("Fit", "BothOutside"));
    CHECK_FALSE(textInside(r.layout(), tight));
    CHECK(r.edit("Fit", "TextFirst"));
    const auto textFirst = r.layout();
    CHECK(r.doc.dimStyleByName("S")->fit == DimFit::TextFirst);

    // A length between "arrows fit" and "text fits": TextFirst puts the text out, Best the arrows.
    double mid = 0;
    for (double l = 5; l < 60 && mid == 0; l += 0.25) {
        Rig a, b;
        REQUIRE(a.host.run("DIMLINEAR", {"0,0", std::to_string(l) + ",0", "1,10"}).finished);
        REQUIRE(b.host.run("DIMLINEAR", {"0,0", std::to_string(l) + ",0", "1,10"}).finished);
        REQUIRE(a.edit("Fit", "Best"));
        REQUIRE(b.edit("Fit", "TextFirst"));
        if (textInside(a.layout(), l) && !textInside(b.layout(), l))
            mid = l;
    }
    CHECK(mid > 0);  // there is a band where the two rules differ

    // The setting survives the file and DXF (DIMATFIT 2).
    auto tb = viaTbcad(r.doc);
    CHECK(tb->dimStyleByName("S")->fit == DimFit::TextFirst);
    CHECK(canonicalDump(*tb) == canonicalDump(r.doc));
    auto dx = viaDxf(r.doc);
    CHECK(dx->dimStyleByName("S")->fit == DimFit::TextFirst);
    for (DimFit f : {DimFit::Best, DimFit::ArrowsFirst, DimFit::BothOutside}) {
        REQUIRE(r.host
                    .run("DIMSTYLE", {"Edit", "S", "Fit",
                                      f == DimFit::Best          ? "Best"
                                      : f == DimFit::ArrowsFirst ? "ArrowsFirst"
                                                                 : "BothOutside",
                                      "Done", "Done"})
                    .finished);
        CHECK(viaDxf(r.doc)->dimStyleByName("S")->fit == f);
    }
    CHECK(r.host.run("DIMSTYLE", {"Edit", "S", "Fit", "Sideways", "Done", "Done"}).finished);
    CHECK(r.doc.dimStyleByName("S")->fit == DimFit::BothOutside);  // unchanged by the bad word
    REQUIRE(r.host.undo());
}

TEST_CASE("DIMTMOVE Free drops the leader of text dragged away, the setting round trips") {
    Rig r;
    REQUIRE(r.host.run("DIMLINEAR", {"0,0", "100,0", "50,20"}).finished);
    const EntityId id = r.dimension();
    // Drag the text far to the right of the extension lines.
    {
        Transaction tx = r.doc.begin("drag");
        REQUIRE(tx.modify(id, [](Entity& e) {
                      auto* d = entityAs<DimensionData>(e);
                      d->textPos = {160, 40};
                      d->textPosUser = true;
                  }).ok());
        tx.commit();
    }
    auto dimLines = [&] {
        std::size_t n = 0;
        for (const auto& l : r.layout().lines)
            n += l.role == dim::Role::DimLine ? 1 : 0;
        return n;
    };
    REQUIRE(r.edit("TextMove", "Leader"));
    const std::size_t withLeader = dimLines();
    REQUIRE(r.edit("TextMove", "WithLine"));
    const std::size_t withLine = dimLines();
    // Leader: AutoCAD's leader from the middle of the dimension line plus a landing under the
    // text; WithLine keeps its short leader from the line's end.
    CHECK(withLeader == withLine + 1);
    REQUIRE(r.edit("TextMove", "Free"));
    CHECK(dimLines() + 1 == withLine);  // no leader line
    CHECK(r.doc.dimStyleByName("S")->textMove == DimTextMove::Free);

    CHECK(canonicalDump(*viaTbcad(r.doc)) == canonicalDump(r.doc));
    for (const char* mode : {"Leader", "WithLine", "Free"}) {
        CAPTURE(mode);
        REQUIRE(r.edit("TextMove", mode));
        const DimTextMove want = r.doc.dimStyleByName("S")->textMove;
        CHECK(viaDxf(r.doc)->dimStyleByName("S")->textMove == want);
    }
    CHECK(r.host.run("DIMSTYLE", {"Edit", "S", "TextMove", "Up", "Done", "Done"}).finished);
    CHECK(r.doc.dimStyleByName("S")->textMove == DimTextMove::Free);
    REQUIRE(r.host.undo());
    REQUIRE(r.host.redo());
    CHECK(r.doc.dimStyleByName("S")->textMove == DimTextMove::Free);
}
