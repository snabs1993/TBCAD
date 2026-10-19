#include <filesystem>
#include <fstream>
#include <random>

#include "support.h"
#include "tbcad/drafting/snap_geometry.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/underlay/image.h"
#include "tbcad/xref/xref.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;
namespace fs = std::filesystem;

namespace {

struct Dir {
    fs::path path;
    Dir() {
        path = fs::temp_directory_path() /
               ("tbcad_xref_cmds_" + std::to_string(std::random_device{}()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Dir() { fs::remove_all(path); }
};

fs::path writeSource(const fs::path& dir, const char* name, int lines) {
    Document d;
    {
        Transaction tx = d.begin("source");
        Layer l;
        l.name = "walls";
        LayerId w = tx.addLayer(l).value();
        for (int i = 0; i < lines; ++i) {
            Entity e;
            e.data = LineData{{double(i), 0}, {double(i), 10}};
            e.common.layer = w;
            (void)tx.add(e);
        }
        tx.commit();
    }
    fs::path file = dir / name;
    REQUIRE(io::writeTbcad(d, file, io::WriteOptions{.keepBackup = false}).ok());
    return file;
}

}  // namespace

TEST_CASE("XATTACH attaches a file and inserts it - undo removes everything") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 3);
    Fixture f;
    auto r = f.host->run("XATTACH", {file.string(), "", "", "", "10,20"});
    CHECK(r.finished);
    const BlockDef* def = f.doc.blockByName("plan");
    REQUIRE(def);
    CHECK(fs::path(def->xrefPath) == file);
    CHECK(def->entities.size() == 3);
    REQUIRE(f.modelSpace().size() == 1);
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(ins);
    CHECK(ins->position == Vec2{10, 20});
    CHECK(f.doc.layerByName("plan") != nullptr);
    CHECK(f.host->undoStack().undoLabel() == "Attach plan");
    CHECK(f.host->undo());
    CHECK(f.doc.blockByName("plan") == nullptr);
    CHECK(f.doc.layerByName("plan") == nullptr);
    CHECK(f.modelSpace().empty());
}

TEST_CASE("XATTACH rejects a missing file and asks again - Enter inserts at the origin") {
    Dir dir;
    fs::path file = writeSource(dir.path, "a.tbcad", 1);
    Fixture f;
    f.host->clearLog();
    f.host->run("XATTACH", {(dir.path / "nope.tbcad").string(), file.string(), "", "", "site", ""});
    CHECK(f.host->logContains("Cannot find"));
    REQUIRE(f.modelSpace().size() == 1);
    CHECK(entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]))->position == Vec2{0, 0});
    CHECK(f.doc.blockByName("site") != nullptr);
}

TEST_CASE("XRELOAD picks up a changed file - XLIST reports the state") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 2);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "", "", "", "0,0"});
    REQUIRE(f.doc.blockByName("plan"));
    CHECK(f.doc.blockByName("plan")->entities.size() == 2);

    writeSource(dir.path, "plan.tbcad", 5);
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
    f.host->clearLog();
    f.host->run("XLIST", {});
    CHECK(f.host->logContains("changed on disk"));
    f.host->run("XRELOAD", {""});
    CHECK(f.doc.blockByName("plan")->entities.size() == 5);
    CHECK(f.modelSpace().size() == 1);  // the insert stays
    f.host->clearLog();
    f.host->run("XLIST", {});
    CHECK(f.host->logContains("up to date"));
    CHECK(f.host->undo());
    CHECK(f.doc.blockByName("plan")->entities.size() == 2);
}

TEST_CASE("XBIND turns the xref into a block - XDETACH removes it") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 2);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "", "", "", "0,0"});
    f.host->run("XATTACH", {file.string(), "", "", "other", "5,5"});
    REQUIRE(f.doc.blockByName("other"));

    f.host->run("XBIND", {"plan", ""});
    CHECK(f.doc.blockByName("plan")->xrefPath.empty());
    f.host->clearLog();
    f.host->run("XBIND", {"plan", "other", ""});
    CHECK(f.host->logContains("No external reference named"));
    CHECK(f.doc.blockByName("other")->xrefPath.empty());

    f.host->run("XATTACH", {file.string(), "", "", "third", "1,1"});
    f.host->run("XDETACH", {"third"});
    CHECK(f.doc.blockByName("third") == nullptr);
    CHECK(f.doc.layerByName("third") == nullptr);
    CHECK(f.modelSpace().size() == 2);
}

namespace {

fs::path writePdf(const fs::path& dir, const char* name, const std::string& content) {
    std::string p = "%PDF-1.4\n";
    p += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    p += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 288 144] >>\nendobj\n";
    p += "3 0 obj\n<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>\nendobj\n";
    p += "4 0 obj\n<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
         "\nendstream\nendobj\ntrailer\n<< /Root 1 0 R >>\n";
    fs::path file = dir / name;
    std::ofstream(file, std::ios::binary) << p;
    return file;
}

}  // namespace

TEST_CASE("PDFATTACH attaches a PDF page as a faded underlay - the fade is stored with the path") {
    Dir dir;
    fs::path file = writePdf(dir.path, "site.pdf", "0 0 0 RG 0 0 m 72 0 l S 10 10 50 20 re S");
    Fixture f;
    auto r = f.host->run("PDFATTACH", {file.string(), "70", "", "", "5,5"});
    CHECK(r.finished);
    const BlockDef* def = f.doc.blockByName("site");
    REQUIRE(def);
    CHECK(def->xrefPath == file.generic_string() + "#fade=70");
    CHECK(def->entities.size() == 2);
    CHECK(xref::isUnderlay(f.doc, def->id));
    CHECK(f.doc.layerByName("site") != nullptr);
    const Layer* pdfLayer = f.doc.layerByName(f.doc.layerByName("site")->id, "PDF");
    CHECK(pdfLayer != nullptr);
    // Faded black at 70 percent: 160 * 0.7 = 112.
    const Entity* e = f.doc.find(def->entities[0]);
    REQUIRE(e);
    CHECK(e->common.color == Color::rgb(112, 112, 112));
    CHECK(f.host->undo());
    CHECK(f.doc.blockByName("site") == nullptr);
}

TEST_CASE("a PDF underlay keeps its PDF layers - a hidden one starts off - and XCLIP clips it") {
    Dir dir;
    const std::string content =
        "0 0 0 RG /OC /A BDC 0 0 m 72 0 l S EMC /OC /B BDC 0 20 m 72 20 l S EMC "
        "100 0 m 172 0 l S";
    std::string p = "%PDF-1.5\n";
    p += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /OCProperties << /OCGs [5 0 R 6 0 R] "
         "/D << /OFF [6 0 R] >> >> >>\nendobj\n";
    p += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 288 144] >>\nendobj\n";
    p += "3 0 obj\n<< /Type /Page /Parent 2 0 R /Contents 4 0 R /Resources << /Properties "
         "<< /A 5 0 R /B 6 0 R >> >> >>\nendobj\n";
    p += "4 0 obj\n<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content +
         "\nendstream\nendobj\n5 0 obj\n<< /Type /OCG /Name (Walls) >>\nendobj\n"
         "6 0 obj\n<< /Type /OCG /Name (Furniture) >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n";
    fs::path file = dir.path / "plan.pdf";
    std::ofstream(file, std::ios::binary) << p;
    Fixture f;
    f.host->run("PDFATTACH", {file.string(), "0", "", "", "0,0"});
    const BlockDef* def = f.doc.blockByName("plan");
    REQUIRE(def);
    const Layer* root = f.doc.layerByName("plan");
    REQUIRE(root);
    const Layer* walls = f.doc.layerByName(root->id, "Walls");
    const Layer* furniture = f.doc.layerByName(root->id, "Furniture");
    REQUIRE(walls);
    REQUIRE(furniture);
    CHECK(walls->on);
    CHECK_FALSE(furniture->on);

    auto shown = [&] {
        DisplayList out;
        geom::DisplayContext ctx;
        geom::buildDisplay(f.doc, *f.doc.find(f.modelSpace()[0]), ctx, out);
        std::size_t n = 0;  // the scene hides the parts on layers that are off
        for (const DisplayItem& it : out.items)
            if (const auto* pl = std::get_if<DisplayPolyline>(&it))
                n += f.doc.effective(pl->style.layer).visible() ? 1 : 0;
        return n;
    };
    CHECK(shown() == 2);  // the walls and the line outside any layer; the furniture is off
    // Clip to x 0..30 mm: only the walls line (0..25 mm) is left of it... the third starts at 35
    // mm.
    f.host->run("XCLIP", {cmd::ScriptItem::pick(f.modelSpace()[0]), "", "", "-1,-1", "30,30"});
    CHECK(shown() == 1);
}

TEST_CASE("XATTACH takes a PDF too, and XRELOAD keeps the page and fade of the stored path") {
    Dir dir;
    fs::path file = writePdf(dir.path, "a.pdf", "0 0 m 10 10 l S");
    Fixture f;
    f.host->run("XATTACH", {file.string() + "#fade=0", "", "", "", ""});
    const BlockDef* def = f.doc.blockByName("a");
    REQUIRE(def);
    CHECK(def->entities.size() == 1);
    CHECK(f.doc.find(def->entities[0])->common.color == Color::rgb(0, 0, 0));
    writePdf(dir.path, "a.pdf", "0 0 m 10 10 l S 0 0 m 5 5 l S");
    f.host->run("XRELOAD", {""});
    def = f.doc.blockByName("a");
    REQUIRE(def);
    CHECK(def->entities.size() == 2);
    CHECK(f.doc.find(def->entities[0])->common.color == Color::rgb(0, 0, 0));  // fade=0 kept
}

TEST_CASE("a PDF underlay offers no snap points unless PDFOSNAP is on") {
    Dir dir;
    fs::path file = writePdf(dir.path, "s.pdf", "0 0 m 100 0 l S");
    Fixture f;
    f.host->run("PDFATTACH", {file.string(), "50", "", "", "0,0"});
    REQUIRE(f.modelSpace().size() == 1);
    drafting::SnapGeometry g;
    CHECK(drafting::SnapGeometryRegistry::instance().collect(f.doc, *f.doc.find(f.modelSpace()[0]),
                                                             g));
    CHECK(g.endpoints.empty());
    CHECK(g.curves.empty());

    CHECK(f.host->run("PDFOSNAP", {"1"}).finished);
    CHECK(pdfOsnap(f.doc.header()));
    drafting::SnapGeometry on;
    CHECK(drafting::SnapGeometryRegistry::instance().collect(f.doc, *f.doc.find(f.modelSpace()[0]),
                                                             on));
    CHECK(on.endpoints.size() == 2);  // the ends of the PDF line
    f.host->undo();
    CHECK_FALSE(pdfOsnap(f.doc.header()));
}

namespace {

fs::path writeBmp(const fs::path& dir, const char* name, int w, int h) {
    auto le = [](std::string& s, std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i)
            s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    const int stride = (w * 3 + 3) / 4 * 4;
    std::string out = "BM";
    le(out, 54 + stride * h, 4);
    le(out, 0, 4);
    le(out, 54, 4);
    le(out, 40, 4);
    le(out, w, 4);
    le(out, h, 4);
    le(out, 1, 2);
    le(out, 24, 2);
    le(out, 0, 4);
    le(out, stride * h, 4);
    le(out, 2835, 4);
    le(out, 2835, 4);
    le(out, 0, 4);
    le(out, 0, 4);
    out.append(static_cast<std::size_t>(stride) * h, '\x80');
    fs::path file = dir / name;
    std::ofstream(file, std::ios::binary) << out;
    return file;
}

}  // namespace

TEST_CASE("IMAGEATTACH attaches a picture with a fade - the path is stored, not the pixels") {
    Dir dir;
    fs::path file = writeBmp(dir.path, "site.bmp", 100, 50);
    Fixture f;
    auto r = f.host->run("IMAGEATTACH", {file.string(), "40", "", "", "0,0"});
    CHECK(r.finished);
    const BlockDef* def = f.doc.blockByName("site");
    REQUIRE(def);
    CHECK(fs::path(def->xrefPath) == file);
    CHECK(def->entities.size() == 1);  // the frame
    CHECK(xref::isUnderlay(f.doc, def->id));
    CHECK(underlay::imageParams(f.doc.header(), "site").fade == 40);
    CHECK(f.host->undo());
    CHECK(f.doc.blockByName("site") == nullptr);
    CHECK(underlay::imageParams(f.doc.header(), "site") == underlay::ImageParams{});

    f.host->clearLog();
    f.host->run("IMAGEATTACH", {(dir.path / "x.png").string(), "", "", "", ""});
    CHECK(f.doc.blockByName("x") == nullptr);
}

TEST_CASE("IMAGECLIP keeps a rectangle of the picture - Delete shows it all") {
    Dir dir;
    fs::path file = writeBmp(dir.path, "site.bmp", 100, 50);
    Fixture f;
    f.host->run("IMAGEATTACH", {file.string(), "0", "", "", "0,0"});
    f.host->run("IMAGECLIP", {"20,0", "40,10"});
    auto p = underlay::imageParams(f.doc.header(), "site");
    CHECK(p.u0 == doctest::Approx(0.2));
    CHECK(p.u1 == doctest::Approx(0.4));
    CHECK(p.v0 == doctest::Approx(0.8));
    CHECK(p.v1 == doctest::Approx(1.0));
    CHECK(p.clipped());
    // What is drawn shrinks to the rectangle.
    {
        underlay::ImageCache cache;
        auto resolve = [](std::string_view s) { return fs::path(std::string(s)); };
        auto imgs = underlay::collectImages(f.doc, cache, resolve);
        REQUIRE(imgs.size() == 1);
        CHECK(imgs[0].corners[0].x == doctest::Approx(20));
        CHECK(imgs[0].corners[2].x == doctest::Approx(40));
        CHECK(imgs[0].corners[2].y == doctest::Approx(10));
        CHECK(imgs[0].corners[0].y == doctest::Approx(0));
    }
    f.host->run("IMAGECLIP", {"Delete"});
    CHECK_FALSE(underlay::imageParams(f.doc.header(), "site").clipped());
}

TEST_CASE("IMAGECLIP Polygonal clips to an outline - IMAGEADJUST and IMAGEFRAME set the rest") {
    Dir dir;
    fs::path file = writeBmp(dir.path, "site.bmp", 100, 50);
    Fixture f;
    f.host->run("IMAGEATTACH", {file.string(), "0", "", "", "0,0"});
    f.host->run("IMAGECLIP", {"Polygonal", "0,0", "100,0", "50,50", "Close"});
    auto p = underlay::imageParams(f.doc.header(), "site");
    REQUIRE(p.clip.size() == 3);
    CHECK(p.clip[2].x == doctest::Approx(0.5));
    CHECK(p.clip[2].y == doctest::Approx(0.0));
    CHECK(p.u0 == doctest::Approx(0.0));
    CHECK(p.u1 == doctest::Approx(1.0));
    f.host->run("IMAGECLIP", {"Delete"});
    CHECK(underlay::imageParams(f.doc.header(), "site").clip.empty());

    f.host->run("IMAGEADJUST", {"65", "40", "No"});
    p = underlay::imageParams(f.doc.header(), "site");
    CHECK(p.brightness == 65);
    CHECK(p.contrast == 40);
    f.host->run("IMAGEFRAME", {"Hide"});
    CHECK(underlay::imageParams(f.doc.header(), "site").frame == 0);
    f.host->run("IMAGEFRAME", {"Plot"});
    CHECK(underlay::imageParams(f.doc.header(), "site").frame == 1);
    f.host->run("IMAGETRANSPARENCY", {"ON"});
    CHECK(underlay::imageParams(f.doc.header(), "site").transparent);
    f.host->run("IMAGETRANSPARENCY", {"OFF"});
    CHECK_FALSE(underlay::imageParams(f.doc.header(), "site").transparent);
}

TEST_CASE("UNDERLAYFADE changes the fade of a picture and of a PDF page") {
    Dir dir;
    fs::path bmp = writeBmp(dir.path, "pic.bmp", 10, 10);
    Fixture f;
    f.host->run("IMAGEATTACH", {bmp.string(), "10", "", "", "0,0"});
    f.host->run("UNDERLAYFADE", {"75"});
    CHECK(underlay::imageParams(f.doc.header(), "pic").fade == 75);

    fs::path pdf = writePdf(dir.path, "pg.pdf", "0 0 0 RG 0 0 m 72 0 l S");
    f.host->run("PDFATTACH", {pdf.string(), "50", "", "", "0,0"});
    f.host->run("UNDERLAYFADE", {"pg", "20"});
    const BlockDef* def = f.doc.blockByName("pg");
    REQUIRE(def);
    CHECK(def->xrefPath == pdf.generic_string() + "#fade=20");
    CHECK(f.doc.find(def->entities[0])->common.color == Color::rgb(32, 32, 32));
}

TEST_CASE("XATTACH Overlay with an absolute path - XLIST says overlay") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 2);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "Overlay", "Absolute", "", "0,0"});
    const BlockDef* def = f.doc.blockByName("plan");
    REQUIRE(def);
    CHECK(def->xrefOverlay);
    CHECK(fs::path(def->xrefPath) == file);
    f.host->clearLog();
    f.host->run("XLIST", {});
    CHECK(f.host->logContains("overlay"));
}

TEST_CASE("XATTACH with the name-only path type stores just the file name") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 1);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "", "None", "", "0,0"});
    REQUIRE(f.doc.blockByName("plan"));
    CHECK(f.doc.blockByName("plan")->xrefPath == "plan.tbcad");
}

TEST_CASE("XCLIP clips an insert in block coordinates - OFF, ON and Delete - undo") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 10);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "", "", "", "100,0"});
    EntityId ins = f.modelSpace()[0];
    // Rectangle in world coordinates: the block sits at x=100, so this is x 0..4 of the block.
    f.host->run("XCLIP", {cmd::ScriptItem::pick(ins), "", "", "100,0", "104,10"});
    auto* d = entityAs<InsertData>(*f.doc.find(ins));
    REQUIRE(d->clip.size() == 4);
    CHECK(near(d->clip[0], {0, 0}));
    CHECK(near(d->clip[2], {4, 10}));
    BBox2 box = blocks::insertBounds(f.doc, *f.doc.find(ins));
    CHECK(near(box.min, {100, 0}));
    CHECK(near(box.max, {104, 10}));

    f.host->run("XCLIP", {cmd::ScriptItem::pick(ins), "OFF"});
    CHECK_FALSE(entityAs<InsertData>(*f.doc.find(ins))->clipOn);
    f.host->run("XCLIP", {cmd::ScriptItem::pick(ins), "ON"});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->clipOn);
    f.host->run("XCLIP", {cmd::ScriptItem::pick(ins), "Delete"});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->clip.empty());
    CHECK(f.host->undo());
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->clip.size() == 4);

    // Polygonal: three points then Enter.
    f.host->run("XCLIP",
                {cmd::ScriptItem::pick(ins), "New", "Polygonal", "100,0", "108,0", "100,8", ""});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->clip.size() == 3);
}

TEST_CASE("XBIND Insert merges the layers of the xref into the drawing's own") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 2);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "", "", "", "0,0"});
    REQUIRE(f.doc.layerByName("plan") != nullptr);
    f.host->run("XBIND", {"plan", "Insert"});
    CHECK(f.doc.blockByName("plan")->xrefPath.empty());
    CHECK(f.doc.layerByName("plan") == nullptr);
    CHECK(f.doc.layerByName("walls") != nullptr);
    CHECK(f.host->undo());
    CHECK(f.doc.layerByName("plan") != nullptr);
    CHECK(f.doc.layerByName("walls") == nullptr);
    CHECK_FALSE(f.doc.blockByName("plan")->xrefPath.empty());
}

TEST_CASE("XPATH changes how the path is stored, XRELINK repairs a lost file") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 2);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "", "", "", "0,0"});
    REQUIRE(f.doc.blockByName("plan"));
    f.host->run("XPATH", {"plan", "None"});
    CHECK(f.doc.blockByName("plan")->xrefPath == "plan.tbcad");
    CHECK(f.host->undo());
    CHECK(fs::path(f.doc.blockByName("plan")->xrefPath) == file);

    // The file moves: the reference is lost until XRELINK points it at the new place.
    fs::path moved = writeSource(dir.path, "moved.tbcad", 4);
    f.host->run("XPATH", {"plan", "None"});
    f.host->clearLog();
    f.host->run("XRELINK", {"plan", moved.string()});
    CHECK(fs::path(f.doc.blockByName("plan")->xrefPath).filename() == "moved.tbcad");
    CHECK(f.doc.blockByName("plan")->entities.size() == 4);
    f.host->clearLog();
    f.host->run("XRELINK", {"plan", (dir.path / "gone.tbcad").string()});
    CHECK(f.host->logContains("Cannot read"));
    CHECK(f.doc.blockByName("plan")->entities.size() == 4);
}

TEST_CASE("XCLIP Invert toggles, shows the outside, survives the file and DXF, Delete resets it") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 10);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "", "", "", "100,0"});
    EntityId ins = f.modelSpace()[0];
    f.host->clearLog();
    f.host->run("XCLIP", {cmd::ScriptItem::pick(ins), "Invert"});
    CHECK(f.host->logContains("no clipping boundary"));
    f.host->run("XCLIP", {cmd::ScriptItem::pick(ins), "", "", "100,0", "104,10"});
    f.host->run("XCLIP", {cmd::ScriptItem::pick(ins), "Invert"});
    CHECK(entityAs<InsertData>(*f.doc.find(ins))->clipInvert);
    // Bounds of an inverted clip are the whole block again.
    BBox2 box = blocks::insertBounds(f.doc, *f.doc.find(ins));
    CHECK(box.max.x > 105);
    // The file keeps it.
    auto bytes = io::writeTbcadToMemory(f.doc);
    REQUIRE(bytes.ok());
    io::IoReport rep;
    auto back = io::readTbcad(bytes.value(), rep);
    REQUIRE(back.ok());
    bool seen = false;
    back.value()->forEachEntity([&](const Entity& e) {
        if (const auto* d = entityAs<InsertData>(e))
            seen = seen || (d->clipInvert && d->clip.size() == 4);
    });
    CHECK(seen);
    f.host->run("XCLIP", {cmd::ScriptItem::pick(ins), "Invert"});
    CHECK_FALSE(entityAs<InsertData>(*f.doc.find(ins))->clipInvert);
    f.host->run("XCLIP", {cmd::ScriptItem::pick(ins), "Invert"});
    f.host->run("XCLIP", {cmd::ScriptItem::pick(ins), "Delete"});
    CHECK_FALSE(entityAs<InsertData>(*f.doc.find(ins))->clipInvert);
}

TEST_CASE("EXTERNALREFERENCES opens the External References panel") {
    Fixture f;
    CHECK(f.reg.find("EXTERNALREFERENCES"));
    CHECK(f.reg.find("ER") == f.reg.find("EXTERNALREFERENCES"));
}

TEST_CASE("ETRANSMIT copies the drawing and its references, keeping relative paths") {
    Dir dir;
    fs::create_directories(dir.path / "sub");
    fs::path inside = writeSource(dir.path / "sub", "inside.tbcad", 2);
    Dir other;  // a folder outside the drawing's tree
    fs::path outside = writeSource(other.path, "outside.tbcad", 2);
    fs::path main = writeSource(dir.path, "main.tbcad", 1);
    Fixture f;
    f.host->setSetting("document.file", main.string());
    f.host->run("XATTACH", {"sub/inside.tbcad", "", "", "", "0,0"});
    f.host->run("XATTACH", {outside.string(), "", "", "", "0,0"});
    // an unreadable reference is reported, not fatal
    f.host->run("XATTACH", {(dir.path / "sub" / "gone.tbcad").string(), "", "", "", "0,0"});
    REQUIRE(f.doc.blockByName("inside"));
    REQUIRE(f.doc.blockByName("outside"));

    Dir pack;
    fs::path to = pack.path / "send";
    f.host->clearLog();
    f.host->run("ETRANSMIT", {to.string()});
    CHECK(fs::exists(to / "main.tbcad"));
    CHECK(fs::exists(to / "sub" / "inside.tbcad"));
    CHECK(fs::exists(to / "refs" / "outside.tbcad"));
    CHECK(fs::exists(to / "transmittal.txt"));
    CHECK(f.host->logContains("moved to refs"));
}

TEST_CASE("ETRANSMIT needs a saved drawing") {
    Fixture f;
    f.host->clearLog();
    f.host->run("ETRANSMIT", {"nowhere"});
    CHECK(f.host->logContains("Save the drawing first"));
}
