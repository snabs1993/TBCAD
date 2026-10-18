// MIRRTEXT: backward and upside-down text. MIRROR keeps text readable by default; with
// MIRRTEXT = 1 the text is reflected, and the flags survive .tbcad and DXF files and undo.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>

#include "tbcad/cmd/cmd.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/script_host.h"
#include "tbcad/core/dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/text/text.h"

using namespace tbcad;

namespace {

struct Rig {
    Document doc;
    cmd::Registry reg;
    cmd::ScriptHost host;
    Rig() : host(doc, (registerAllCommands(reg), reg)) {}

    EntityId addText(Vec2 at, double rot, HAlign h = HAlign::Left, double obl = 0) {
        TextData t;
        t.position = at;
        t.alignPoint = at;
        t.height = 4;
        t.rotation = rot;
        t.utf8 = "Flip Me";
        t.h = h;
        t.oblique = obl;
        Entity e;
        e.data = Indirect<TextData>(std::move(t));
        auto tx = doc.begin("text");
        auto id = tx.add(std::move(e)).value();
        tx.commit();
        return id;
    }
    const TextData* text(EntityId id) const { return entityAs<TextData>(*doc.find(id)); }
    std::vector<EntityId> texts() const {
        std::vector<EntityId> out;
        doc.forEachEntity([&](const Entity& e) {
            if (entityAs<TextData>(e))
                out.push_back(e.common.id);
        });
        return out;
    }
};

}  // namespace

TEST_CASE("MIRRTEXT 0 keeps mirrored text readable") {
    Rig r;
    auto id = r.addText({10, 5}, 0.3);
    r.host.setSelection({id});
    r.host.run("MIRROR", {"0,0", "0,10", "Y"});
    const TextData* t = r.text(id);
    REQUIRE(t);
    CHECK_FALSE(t->backward);
    CHECK_FALSE(t->upsideDown);
    CHECK(t->position.x == doctest::Approx(-10));
}

TEST_CASE("MIRRTEXT 1 reflects text exactly like the geometry around it") {
    for (HAlign h : {HAlign::Left, HAlign::Center, HAlign::Right}) {
        for (double rot : {0.0, 0.5, 1.7, 3.0, -1.0}) {
            for (const char* axis : {"0,10", "10,0", "10,10", "10,-3"}) {
                Rig r;
                r.host.run("SETVAR", {"MIRRTEXT", "1"});
                auto id = r.addText({10, 5}, rot, h, 0.2);
                const Transform2 m = Transform2::mirror({0, 0},
                                                        [&] {
                                                            double x = 0, y = 0;
                                                            std::sscanf(axis, "%lf,%lf", &x, &y);
                                                            return Vec2{x, y};
                                                        }())
                                         .value();
                // Expected frame corners: the original corners mapped by the mirror.
                std::vector<Vec2> want;
                for (Vec2 c : text::layoutText(r.doc, *r.text(id)).frameCorners())
                    want.push_back(m.apply(c));
                r.host.setSelection({id});
                r.host.run("MIRROR", {"0,0", axis, "Y"});
                const TextData* t = r.text(id);
                REQUIRE(t);
                CHECK((t->backward || t->upsideDown));
                BBox2 a, b;
                for (Vec2 c : want)
                    a.expand(c);
                for (Vec2 c : text::layoutText(r.doc, *t).frameCorners())
                    b.expand(c);
                CHECK(b.min.x == doctest::Approx(a.min.x).epsilon(1e-6));
                CHECK(b.min.y == doctest::Approx(a.min.y).epsilon(1e-6));
                CHECK(b.max.x == doctest::Approx(a.max.x).epsilon(1e-6));
                CHECK(b.max.y == doctest::Approx(a.max.y).epsilon(1e-6));
                // Mirroring back restores the text.
                r.host.setSelection({id});
                r.host.run("MIRROR", {"0,0", axis, "Y"});
                const TextData* back = r.text(id);
                CHECK_FALSE(back->backward);
                CHECK_FALSE(back->upsideDown);
                CHECK(back->position.x == doctest::Approx(10));
                CHECK(std::remainder(back->rotation - rot, kTwoPi) ==
                      doctest::Approx(0).epsilon(1e-6));
            }
        }
    }
}

TEST_CASE("MIRRTEXT 1 undo restores the text") {
    Rig r;
    r.host.run("SETVAR", {"MIRRTEXT", "1"});
    auto id = r.addText({10, 5}, 0.3);
    const std::string before = canonicalDump(r.doc);
    r.host.setSelection({id});
    r.host.run("MIRROR", {"0,0", "0,10", "Y"});
    CHECK(canonicalDump(r.doc) != before);
    CHECK(r.host.undo());
    CHECK(canonicalDump(r.doc) == before);
}

TEST_CASE("Text generation flags survive .tbcad and DXF files") {
    Rig r;
    r.host.run("SETVAR", {"MIRRTEXT", "1"});
    auto a = r.addText({10, 5}, 0.3);
    auto b = r.addText({10, 25}, 0.0, HAlign::Center);
    r.host.setSelection({a});
    r.host.run("MIRROR", {"0,0", "0,10", "Y"});
    r.host.setSelection({b});
    r.host.run("MIRROR", {"0,0", "10,0", "Y"});
    CHECK(r.text(a)->backward != r.text(a)->upsideDown);
    // Text style flags apply too.
    {
        auto tx = r.doc.begin("style");
        TextStyle ts;
        ts.name = "Rev";
        ts.backward = true;
        (void)tx.addTextStyle(ts);
        tx.commit();
    }
    const std::string want = canonicalDump(r.doc);
    const auto dir = std::filesystem::temp_directory_path() / "tbcad_mirrtext";
    std::filesystem::create_directories(dir);
    {
        const auto file = dir / "a.tbcad";
        REQUIRE(io::writeTbcad(r.doc, file).ok());
        io::IoReport rep;
        auto back = io::readTbcad(file, rep);
        REQUIRE(back.ok());
        CHECK(canonicalDump(*back.value()) == want);
    }
    {
        dxf::Report er;
        auto out = dxf::exportDxf(r.doc, dxf::ExportOptions{}, er);
        REQUIRE(out.ok());
        std::vector<std::byte> bytes(out.value().bytes.size());
        std::memcpy(bytes.data(), out.value().bytes.data(), bytes.size());
        dxf::Report ir;
        auto in = dxf::importDxf(bytes, dxf::ImportOptions{}, ir);
        REQUIRE(in.ok());
        const Document& d2 = *in.value().document;
        int flipped = 0;
        d2.forEachEntity([&](const Entity& e) {
            if (const auto* t = entityAs<TextData>(e))
                flipped += (t->backward || t->upsideDown) ? 1 : 0;
        });
        CHECK(flipped == 2);
        CHECK(mirrText(d2.header()));
        const TextStyle* rev = d2.textStyleByName("Rev");
        REQUIRE(rev);
        CHECK(rev->backward);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
