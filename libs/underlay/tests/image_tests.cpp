#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "tbcad/underlay/image.h"

using namespace tbcad;
using namespace tbcad::underlay;

namespace {

std::filesystem::path tempDir() {
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() /
               ("tbcad_img_" + std::to_string(rd()) + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return dir;
}

void put32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put16(std::string& s, std::uint32_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}

// A 24-bit BMP; pixel (x, y) with y counted from the top has the colour fn(x, y).
template <class Fn>
void writeBmp(const std::filesystem::path& file, int w, int h, Fn fn) {
    const int stride = (w * 3 + 3) / 4 * 4;
    std::string px;
    for (int row = h - 1; row >= 0; --row) {  // BMP stores the bottom row first
        std::string line;
        for (int x = 0; x < w; ++x) {
            const auto c = fn(x, row);
            line.push_back(static_cast<char>(c[2]));
            line.push_back(static_cast<char>(c[1]));
            line.push_back(static_cast<char>(c[0]));
        }
        line.resize(stride, 0);
        px += line;
    }
    std::string out = "BM";
    put32(out, 54 + static_cast<std::uint32_t>(px.size()));
    put32(out, 0);
    put32(out, 54);
    put32(out, 40);
    put32(out, static_cast<std::uint32_t>(w));
    put32(out, static_cast<std::uint32_t>(h));
    put16(out, 1);
    put16(out, 24);
    put32(out, 0);
    put32(out, static_cast<std::uint32_t>(px.size()));
    put32(out, 2835);
    put32(out, 2835);
    put32(out, 0);
    put32(out, 0);
    out += px;
    std::ofstream(file, std::ios::binary) << out;
}

}  // namespace

TEST_CASE("image paths are recognised by extension") {
    CHECK(isImagePath("a/b/plan.PNG"));
    CHECK(isImagePath("plan.jpeg"));
    CHECK(isImagePath("plan.jpg"));
    CHECK(isImagePath("plan.bmp"));
    CHECK_FALSE(isImagePath("plan.pdf"));
    CHECK_FALSE(isImagePath("plan"));
}

TEST_CASE("a picture is decoded top row first") {
    const auto dir = tempDir();
    writeBmp(dir / "p.bmp", 4, 2,
             [](int x, int y) { return std::array<int, 3>{x * 60, y == 0 ? 200 : 0, 7}; });
    auto info = imageInfo(dir / "p.bmp");
    REQUIRE(info);
    CHECK(info.value().width == 4);
    CHECK(info.value().height == 2);
    auto img = loadImage(dir / "p.bmp");
    REQUIRE(img);
    const ImageData& d = *img.value();
    REQUIRE(d.rgba.size() == 4u * 2u * 4u);
    CHECK(d.rgba[0] == 0);
    CHECK(d.rgba[1] == 200);  // top row, green 200
    CHECK(d.rgba[2] == 7);
    CHECK(d.rgba[3] == 255);
    CHECK(d.rgba[4 * 4 + 1] == 0);  // second row
    CHECK(d.rgba[4 * 3] == 180);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a broken or missing picture is an error") {
    const auto dir = tempDir();
    std::ofstream(dir / "x.png") << "not a picture";
    CHECK_FALSE(imageInfo(dir / "x.png"));
    CHECK_FALSE(loadImage(dir / "x.png"));
    CHECK_FALSE(loadImage(dir / "missing.png"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("the frame is a closed rectangle in pixels") {
    auto doc = imageFrameDocument(30, 20);
    REQUIRE(doc);
    int n = 0;
    doc.value()->forEachEntity([&](const Entity& e) {
        const auto* pl = entityAs<PolylineData>(e);
        REQUIRE(pl);
        CHECK(pl->closed);
        CHECK(pl->v.size() == 4);
        CHECK(pl->v[2].p.x == 30);
        CHECK(pl->v[2].p.y == 20);
        ++n;
    });
    CHECK(n == 1);
    CHECK_FALSE(imageFrameDocument(0, 5));
}

TEST_CASE("display parameters round-trip through the header") {
    Document doc;
    ImageParams p;
    p.fade = 60;
    p.u0 = 0.25;
    p.v1 = 0.5;
    {
        auto tx = doc.begin("set");
        REQUIRE(setImageParams(tx, "plan", p));
        tx.commit();
    }
    CHECK(imageParams(doc.header(), "plan") == p);
    CHECK(imageParams(doc.header(), "other") == ImageParams{});
    {
        auto tx = doc.begin("reset");
        REQUIRE(setImageParams(tx, "plan", ImageParams{}));
        tx.commit();
    }
    CHECK(doc.header().custom.count(kImageParamsKey) == 0);
}

TEST_CASE("bad parameter lines are ignored and ranges are clamped") {
    Header h;
    h.custom[kImageParamsKey] = "a\t200\t0\t0\t1\t1\nbroken line\nb\t10\t0.8\t0\t0.2\t1\n";
    const auto t = readImageParams(h);
    REQUIRE(t.size() == 2);
    CHECK(t.at("a").fade == kMaxImageFade);
    CHECK(t.at("b") == ImageParams{10, 0, 0, 1, 1});  // an empty clip rectangle means no clip
}

TEST_CASE("collecting images places the corners by the insert") {
    const auto dir = tempDir();
    writeBmp(dir / "p.bmp", 10, 4, [](int, int) { return std::array<int, 3>{1, 2, 3}; });
    Document doc;
    BlockId blk;
    {
        auto tx = doc.begin("attach");
        BlockDef def;
        def.name = "p";
        def.xrefPath = "p.bmp";
        def.xrefStamp = 5;
        auto b = tx.addBlock(def);
        REQUIRE(b);
        blk = b.value();
        InsertData ins;
        ins.block = blk;
        ins.position = {100, 50};
        ins.scale = {2, 2};
        ins.rotation = kPi / 2;
        Entity e;
        e.data = Indirect<InsertData>(ins);
        REQUIRE(tx.add(std::move(e)));
        tx.commit();
    }
    {
        auto tx = doc.begin("fade");
        ImageParams p;
        p.fade = 50;
        p.u0 = 0.5;  // right half
        REQUIRE(setImageParams(tx, "p", p));
        tx.commit();
    }
    ImageCache cache;
    auto resolve = [&](std::string_view s) { return dir / std::string(s); };
    auto list = collectImages(doc, cache, resolve);
    REQUIRE(list.size() == 1);
    const ImageInstance& im = list[0];
    CHECK(im.opacity == doctest::Approx(0.5f));
    CHECK(im.uMin == 0.5);
    // Visible part: x 5..10, y 0..4 pixels. Bottom-left (5,0): scaled (10,0), rotated 90 deg.
    CHECK(im.corners[0].x == doctest::Approx(100));
    CHECK(im.corners[0].y == doctest::Approx(60));
    // Top-right (10,4): scaled (20,8), rotated -> (-8,20).
    CHECK(im.corners[2].x == doctest::Approx(92));
    CHECK(im.corners[2].y == doctest::Approx(70));
    // The same file gives the same key; a new stamp a new one.
    auto again = collectImages(doc, cache, resolve);
    REQUIRE(again.size() == 1);
    CHECK(again[0].key == im.key);
    CHECK(again[0].data == im.data);
    std::filesystem::remove_all(dir);
}

TEST_CASE("brightness, contrast, frame and a polygon clip are stored and applied") {
    ImageParams p;
    p.brightness = 80;
    p.contrast = 20;
    p.frame = 1;
    p.clip = {{0, 0}, {1, 0}, {0, 1}};
    p.u0 = p.v0 = 0;
    p.u1 = p.v1 = 1;
    Header h;
    ImageParamTable t;
    t["pic"] = p;
    writeImageParams(h, t);
    CHECK(readImageParams(h).at("pic") == p);

    auto src = std::make_shared<ImageData>();
    src->width = src->height = 4;
    src->rgba.assign(4 * 4 * 4, 128);
    for (std::size_t i = 3; i < src->rgba.size(); i += 4)
        src->rgba[i] = 255;
    ImageCache cache;
    ImageParams bright;
    bright.brightness = 100;
    std::uint64_t key = 0;
    auto lit = cache.adjusted(7, src, bright, &key);
    CHECK(lit->rgba[0] == 255);
    CHECK(key != 7);
    // Nothing to apply: the same picture and key.
    auto same = cache.adjusted(7, src, ImageParams{}, &key);
    CHECK(same == src);
    CHECK(key == 7);
    // The triangle (0,0) (1,0) (0,1) in fractions keeps the top-left half: the pixel at the top
    // left stays, the one at the bottom right goes transparent.
    ImageParams tri;
    tri.clip = {{0, 0}, {1, 0}, {0, 1}};
    auto cut = cache.adjusted(7, src, tri);
    CHECK(cut->rgba[3] == 255);
    CHECK(cut->rgba[(3 * 4 + 3) * 4 + 3] == 0);
    CHECK(cut->rgba[0] == 128);
}

TEST_CASE("transparency clears the white background of a bitonal picture only") {
    auto bw = std::make_shared<ImageData>();
    bw->width = 2;
    bw->height = 1;
    bw->rgba = {0, 0, 0, 255, 255, 255, 255, 255};  // black, white
    ImageCache cache;
    ImageParams p;
    p.transparent = true;
    auto out = cache.adjusted(1, bw, p);
    CHECK(out->rgba[3] == 255);  // black stays
    CHECK(out->rgba[7] == 0);    // white goes
    // Without the flag nothing changes.
    CHECK(cache.adjusted(1, bw, ImageParams{}) == bw);
    // A grey or coloured picture is not bitonal: the flag does nothing.
    auto grey = std::make_shared<ImageData>(*bw);
    grey->rgba[0] = grey->rgba[1] = grey->rgba[2] = 100;
    CHECK(cache.adjusted(2, grey, p) == grey);
    // Saved with the other parameters.
    Header h;
    ImageParamTable t;
    t["pic"] = p;
    writeImageParams(h, t);
    CHECK(readImageParams(h).at("pic").transparent);
}

TEST_CASE("hidden inserts and missing files give no image") {
    const auto dir = tempDir();
    writeBmp(dir / "p.bmp", 2, 2, [](int, int) { return std::array<int, 3>{9, 9, 9}; });
    Document doc;
    EntityId id;
    {
        auto tx = doc.begin("attach");
        BlockDef def;
        def.name = "p";
        def.xrefPath = "p.bmp";
        auto b = tx.addBlock(def);
        REQUIRE(b);
        InsertData ins;
        ins.block = b.value();
        Entity e;
        e.data = Indirect<InsertData>(ins);
        auto a = tx.add(std::move(e));
        REQUIRE(a);
        id = a.value();
        tx.commit();
    }
    ImageCache cache;
    auto resolve = [&](std::string_view s) { return dir / std::string(s); };
    CHECK(collectImages(doc, cache, resolve).size() == 1);
    {
        auto tx = doc.begin("hide");
        REQUIRE(tx.modify(id, [](Entity& e) { e.common.visible = false; }));
        tx.commit();
    }
    CHECK(collectImages(doc, cache, resolve).empty());
    {
        auto tx = doc.begin("show");
        REQUIRE(tx.modify(id, [](Entity& e) { e.common.visible = true; }));
        tx.commit();
    }
    std::filesystem::remove(dir / "p.bmp");
    ImageCache fresh;
    CHECK(collectImages(doc, fresh, resolve).empty());
    std::filesystem::remove_all(dir);
}
