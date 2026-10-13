#include "tbcad/samples/house.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "tbcad/dim/dim.h"
#include "tbcad/layout/layout.h"

namespace tbcad::samples {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The sample is built from known-good calls; a failure here is a bug in the generator itself.
[[noreturn]] void fail(const Error& e) {
    std::fprintf(stderr, "tbcad-samples: %s\n", e.message.c_str());
    std::abort();
}
template <class T>
T must(Result<T> r) {
    if (!r)
        fail(r.error());
    return std::move(r).value();
}
void must(Result<void> r) {
    if (!r)
        fail(r.error());
}

struct Layers {
    LayerId wall, wallOut, wallIn, door, window, furn, furnBed, furnSofa, stair, text, dim, hatch,
        roof;
};

class Builder {
public:
    Builder() : doc_(std::make_unique<Document>()) {}

    std::unique_ptr<Document> build() {
        setup();
        groundFloor();
        upperFloor();
        roofLevel();
        sheet();
        return std::move(doc_);
    }

private:
    // ---- plumbing ---------------------------------------------------------------------------
    template <class F>
    void edit(const char* name, F&& f) {
        Transaction tx = doc_->begin(name);
        f(tx);
        tx.commit();
    }

    Entity make(EntityData data, LayerId layer) const {
        Entity e;
        e.common.layer = layer;
        e.data = std::move(data);
        return e;
    }

    void add(Transaction& tx, EntityData data, LayerId layer) {
        must(tx.add(make(std::move(data), layer)));
    }

    void line(Transaction& tx, Vec2 a, Vec2 b, LayerId layer) { add(tx, LineData{a, b}, layer); }

    PolylineData rect(double x0, double y0, double x1, double y1) const {
        PolylineData p;
        p.closed = true;
        p.v = {{{x0, y0}, 0, 0, 0}, {{x1, y0}, 0, 0, 0}, {{x1, y1}, 0, 0, 0}, {{x0, y1}, 0, 0, 0}};
        return p;
    }

    HatchData rectHatch(double x0, double y0, double x1, double y1, const char* pattern,
                        double scale, Color fill) const {
        HatchData h;
        h.solid = pattern == nullptr;
        if (pattern)
            h.pattern = pattern;
        h.patternScale = scale;
        h.fill = fill;
        HatchLoop loop;
        loop.edges.push_back(HatchLineEdge{{x0, y0}, {x1, y0}});
        loop.edges.push_back(HatchLineEdge{{x1, y0}, {x1, y1}});
        loop.edges.push_back(HatchLineEdge{{x1, y1}, {x0, y1}});
        loop.edges.push_back(HatchLineEdge{{x0, y1}, {x0, y0}});
        h.loops.push_back(std::move(loop));
        return h;
    }

    // Annotative text: `height` is the height on paper in mm; the drawing's annotation scale
    // (1:100) makes it 100 times larger in model space.
    void text(Transaction& tx, Vec2 at, const std::string& s, double height,
              HAlign h = HAlign::Center, VAlign v = VAlign::Middle) {
        TextData t;
        t.position = at;
        t.alignPoint = at;
        t.height = height;
        t.style = thai_;
        t.h = h;
        t.v = v;
        t.utf8 = s;
        Entity e = make(std::move(t), L_.text);
        e.common.annotative = true;
        must(tx.add(std::move(e)));
    }

    // A linear dimension between two points; `side` is where the dimension line sits.
    void dimension(Transaction& tx, Vec2 p1, Vec2 p2, Vec2 where, bool vertical) {
        DimensionData d;
        d.kind = DimKind::Linear;
        d.rotation = vertical ? kPi / 2 : 0;
        d.defPts = {p1, p2, where, Vec2{}};
        d.style = dimStyle_;
        dim::refresh(*doc_, d);
        Entity e = make(std::move(d), L_.dim);
        e.common.annotative = true;
        must(tx.add(std::move(e)));
    }

    // ---- document setup ---------------------------------------------------------------------
    void setup() {
        edit("setup", [&](Transaction& tx) {
            must(tx.modifyHeader([&](Header& h) {
                h.units = LengthUnit::Millimeter;
                h.createdBy = "tbcad-samples";
                h.custom[kAnnoScaleKey] = "100";
            }));
            must(tx.modifyLevel(kLevelDefault, [](Level& l) { l.name = "ชั้น 1"; }));
            Level two;
            two.name = "ชั้น 2";
            two.elevation = 3200;
            level2_ = must(tx.addLevel(two));
            Level roof;
            roof.name = "หลังคา";
            roof.elevation = 6400;
            level3_ = must(tx.addLevel(roof));

            thai_ = must(tx.addTextStyle({.name = "Thai", .fontFamily = "Sarabun"}));
            DimStyle ds;
            ds.name = "Arch-100";
            ds.textHeight = 2.5;
            ds.textStyle = thai_;
            ds.arrowType = ArrowType::ArchTick;
            ds.arrowSize = 2;
            ds.precision = 0;
            ds.extLineOffset = 1.5;
            ds.extLineExtension = 1.5;
            dimStyle_ = must(tx.addDimStyle(ds));
            dashed_ = must(tx.addLinetype(
                {.name = "DASHED", .description = "Dashed", .pattern = {600, -300}}));

            L_.wall = must(tx.addLayer({.name = "A-WALL", .color = Color::index(7)}));
            L_.wallOut = must(tx.addLayer({.name = "exterior",
                                           .parent = L_.wall,
                                           .color = Color::byParent(),
                                           .lineweight = LineWeight::mm(0.5)}));
            L_.wallIn = must(tx.addLayer({.name = "interior",
                                          .parent = L_.wall,
                                          .color = Color::byParent(),
                                          .lineweight = LineWeight::mm(0.25)}));
            L_.door = must(tx.addLayer({.name = "A-DOOR", .color = Color::index(30)}));
            L_.window = must(tx.addLayer({.name = "A-WINDOW", .color = Color::index(4)}));
            L_.furn = must(tx.addLayer({.name = "A-FURN", .color = Color::index(8)}));
            L_.furnBed =
                must(tx.addLayer({.name = "bed", .parent = L_.furn, .color = Color::byParent()}));
            L_.furnSofa = must(
                tx.addLayer({.name = "living", .parent = L_.furn, .color = Color::byParent()}));
            L_.stair = must(tx.addLayer({.name = "A-STAIR", .color = Color::index(2)}));
            L_.text = must(tx.addLayer({.name = "A-TEXT", .color = Color::index(7)}));
            L_.dim = must(tx.addLayer({.name = "A-DIM", .color = Color::index(3)}));
            L_.hatch = must(tx.addLayer({.name = "A-HATCH", .color = Color::index(9)}));
            L_.roof = must(
                tx.addLayer({.name = "A-ROOF", .color = Color::index(6), .linetype = dashed_}));

            // Blocks, drawn at unit size where they scale with the opening.
            door_ = must(tx.addBlock({.name = "DOOR", .base = {0, 0}}));
            must(tx.add(make(LineData{{0, 0}, {0, 1}}, kLayerZero), door_));
            must(tx.add(make(ArcData{{0, 0}, 1, 0, kPi / 2}, kLayerZero), door_));
            window_ = must(tx.addBlock({.name = "WINDOW", .base = {0, 0}}));
            must(tx.add(make(rect(0, 0, 1, 1), kLayerZero), window_));
            must(tx.add(make(LineData{{0, 0.33}, {1, 0.33}}, kLayerZero), window_));
            must(tx.add(make(LineData{{0, 0.66}, {1, 0.66}}, kLayerZero), window_));
            bed_ = must(tx.addBlock({.name = "BED-DOUBLE", .base = {0, 0}}));
            must(tx.add(make(rect(0, 0, 1800, 2000), kLayerZero), bed_));
            must(tx.add(make(rect(150, 1450, 800, 1900), kLayerZero), bed_));
            must(tx.add(make(rect(1000, 1450, 1650, 1900), kLayerZero), bed_));
            must(tx.add(make(LineData{{0, 1300}, {1800, 1300}}, kLayerZero), bed_));
            sofa_ = must(tx.addBlock({.name = "SOFA-3", .base = {0, 0}}));
            must(tx.add(make(rect(0, 0, 2200, 900), kLayerZero), sofa_));
            must(tx.add(make(rect(0, 600, 2200, 900), kLayerZero), sofa_));
            must(tx.add(make(rect(0, 0, 200, 600), kLayerZero), sofa_));
            must(tx.add(make(rect(2000, 0, 2200, 600), kLayerZero), sofa_));
            table_ = must(tx.addBlock({.name = "TABLE-6", .base = {0, 0}}));
            must(tx.add(make(rect(0, 0, 1600, 900), kLayerZero), table_));
            for (double x : {300.0, 800.0, 1300.0}) {
                must(tx.add(make(CircleData{{x, -250}, 200}, kLayerZero), table_));
                must(tx.add(make(CircleData{{x, 1150}, 200}, kLayerZero), table_));
            }
            wc_ = must(tx.addBlock({.name = "WC", .base = {0, 0}}));
            must(tx.add(make(rect(0, 400, 400, 700), kLayerZero), wc_));
            must(tx.add(make(EllipseData{{200, 150}, {0, 250}, 0.8, 0, 2 * kPi}, kLayerZero), wc_));
        });
    }

    // ---- walls, openings, furniture ---------------------------------------------------------
    struct Opening {
        double a, b;  // along the wall, mm
        bool door;
    };

    void wallPiece(Transaction& tx, double x0, double y0, double x1, double y1, LayerId layer) {
        add(tx, rect(x0, y0, x1, y1), layer);
        add(tx, rectHatch(x0, y0, x1, y1, nullptr, 1, Color::index(8)), L_.hatch);
    }

    // A wall along X between y0 and y1 from xa to xb, broken at the openings.
    void hWall(Transaction& tx, double y0, double y1, double xa, double xb,
               std::vector<Opening> openings, LayerId layer) {
        double x = xa;
        for (const Opening& o : openings) {
            if (o.a > x)
                wallPiece(tx, x, y0, o.a, y1, layer);
            x = o.b;
            opening(tx, true, o, y0, y1);
        }
        if (xb > x)
            wallPiece(tx, x, y0, xb, y1, layer);
    }

    void vWall(Transaction& tx, double x0, double x1, double ya, double yb,
               std::vector<Opening> openings, LayerId layer) {
        double y = ya;
        for (const Opening& o : openings) {
            if (o.a > y)
                wallPiece(tx, x0, y, x1, o.a, layer);
            y = o.b;
            opening(tx, false, o, x0, x1);
        }
        if (yb > y)
            wallPiece(tx, x0, y, x1, yb, layer);
    }

    // Door leaf and swing, or a window, in the gap of a wall.
    void opening(Transaction& tx, bool horizontal, const Opening& o, double lo, double hi) {
        const double w = o.b - o.a, t = hi - lo;
        InsertData ins;
        if (o.door) {
            ins.block = door_;
            ins.scale = {w, w};
            if (horizontal) {
                ins.position = {o.a, hi};
            } else {
                ins.position = {hi, o.a};
                ins.rotation = kPi / 2;
                ins.scale = {w, -w};
            }
            add(tx, std::move(ins), L_.door);
        } else {
            ins.block = window_;
            ins.scale = {w, t};
            if (horizontal) {
                ins.position = {o.a, lo};
            } else {
                ins.position = {hi, o.a};
                ins.rotation = kPi / 2;
            }
            add(tx, std::move(ins), L_.window);
        }
    }

    void insert(Transaction& tx, BlockId b, Vec2 at, double rotation, LayerId layer) {
        InsertData ins;
        ins.block = b;
        ins.position = at;
        ins.rotation = rotation;
        add(tx, std::move(ins), layer);
    }

    void exteriorShell(Transaction& tx, const std::vector<Opening>& south,
                       const std::vector<Opening>& north, const std::vector<Opening>& west,
                       const std::vector<Opening>& east) {
        hWall(tx, 0, 200, 0, 12000, south, L_.wallOut);
        hWall(tx, 8800, 9000, 0, 12000, north, L_.wallOut);
        vWall(tx, 0, 200, 200, 8800, west, L_.wallOut);
        vWall(tx, 11800, 12000, 200, 8800, east, L_.wallOut);
    }

    void room(Transaction& tx, Vec2 c, const std::string& name, double areaM2) {
        text(tx, {c.x, c.y + 250}, name, 3.5);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1f ตร.ม.", areaM2);
        text(tx, {c.x, c.y - 250}, buf, 2.5);
    }

    void stairs(Transaction& tx, bool up) {
        // Treads of a straight stair in the x 9100..11800 / y 5100..8800 well.
        add(tx, rect(9150, 5150, 11750, 8750), L_.stair);
        for (int i = 1; i < 14; ++i) {
            const double y = 5150 + i * (3600.0 / 14);
            line(tx, {9150, y}, {11750, y}, L_.stair);
        }
        PolylineData arrow;
        arrow.v = {{{10450, 5400}, 0, 0, 0}, {{10450, 8400}, 0, 0, 0}};
        add(tx, arrow, L_.stair);
        PolylineData head;
        head.v = {{{10250, 8100}, 0, 0, 0}, {{10450, 8400}, 0, 0, 0}, {{10650, 8100}, 0, 0, 0}};
        add(tx, head, L_.stair);
        (void)up;
    }

    void floorDims(Transaction& tx, const std::vector<double>& xs, const std::vector<double>& ys) {
        // Overall and chain dimensions along the south and west sides.
        dimension(tx, {0, 0}, {12000, 0}, {0, -1800}, false);
        for (std::size_t i = 0; i + 1 < xs.size(); ++i)
            dimension(tx, {xs[i], 0}, {xs[i + 1], 0}, {0, -900}, false);
        dimension(tx, {0, 0}, {0, 9000}, {-1800, 0}, true);
        for (std::size_t i = 0; i + 1 < ys.size(); ++i)
            dimension(tx, {0, ys[i]}, {0, ys[i + 1]}, {-900, 0}, true);
    }

    // ---- floors -----------------------------------------------------------------------------
    void setCurrent(Transaction& tx, LevelId level) {
        must(tx.modifyHeader([&](Header& h) { h.currentLevel = level; }));
    }

    void groundFloor() {
        edit("ground floor", [&](Transaction& tx) {
            setCurrent(tx, kLevelDefault);
            exteriorShell(
                tx, {{2400, 3400, true}, {8000, 10000, false}},                     // south
                {{1500, 3500, false}, {7000, 7800, false}, {10000, 11000, false}},  // north
                {{3000, 5500, false}, {7000, 8000, false}},                         // west
                {{1500, 3500, false}, {6000, 7000, false}});                        // east
            vWall(tx, 5950, 6050, 200, 8800, {{1000, 2000, true}, {6200, 7000, true}}, L_.wallIn);
            hWall(tx, 4950, 5050, 6050, 11800, {{10200, 11000, true}}, L_.wallIn);
            vWall(tx, 8950, 9050, 5050, 8800, {}, L_.wallIn);

            insert(tx, sofa_, {1000, 7600}, 0, L_.furnSofa);
            insert(tx, table_, {7600, 1400}, 0, L_.furnSofa);
            insert(tx, wc_, {6300, 8150}, 0, L_.furn);
            stairs(tx, true);

            room(tx, {3000, 3800}, "ห้องนั่งเล่น", 52.0);
            room(tx, {9000, 3700}, "ห้องครัวและทานอาหาร", 28.5);
            room(tx, {7500, 6900}, "ห้องน้ำ", 10.5);
            text(tx, {6000, -2700}, "ผังพื้นชั้น 1", 4.0);

            floorDims(tx, {0, 6000, 12000}, {0, 5000, 9000});
        });
    }

    void upperFloor() {
        edit("upper floor", [&](Transaction& tx) {
            setCurrent(tx, level2_);
            add(tx, rectHatch(200, 200, 5950, 4950, "WOOD", 60, Color::index(9)), L_.hatch);
            add(tx, rectHatch(6050, 200, 11800, 4950, "WOOD", 60, Color::index(9)), L_.hatch);
            add(tx, rectHatch(200, 5050, 3450, 8800, "GRID", 60, Color::index(9)), L_.hatch);
            exteriorShell(tx, {{1500, 3500, false}, {8000, 10000, false}},
                          {{1500, 3000, false}, {5000, 7000, false}}, {{2500, 4000, false}},
                          {{2000, 4000, false}, {6000, 7000, false}});
            hWall(tx, 4950, 5050, 200, 11800, {{4000, 4900, true}, {7000, 7900, true}}, L_.wallIn);
            vWall(tx, 5950, 6050, 200, 4950, {}, L_.wallIn);
            vWall(tx, 3450, 3550, 5050, 8800, {{6000, 6800, true}}, L_.wallIn);

            insert(tx, bed_, {3800, 2300}, 0, L_.furnBed);
            insert(tx, bed_, {9400, 2300}, 0, L_.furnBed);
            insert(tx, wc_, {300, 8150}, 0, L_.furn);
            stairs(tx, false);

            room(tx, {3000, 1200}, "ห้องนอนใหญ่", 28.0);
            room(tx, {9000, 1200}, "ห้องนอน 2", 28.0);
            room(tx, {1800, 7000}, "ห้องน้ำ", 11.0);
            room(tx, {6200, 7000}, "โถงชั้น 2", 20.0);
            text(tx, {6000, -2700}, "ผังพื้นชั้น 2", 4.0);

            floorDims(tx, {0, 6000, 12000}, {0, 5000, 9000});
        });
    }

    void roofLevel() {
        edit("roof", [&](Transaction& tx) {
            setCurrent(tx, level3_);
            add(tx, rect(-800, -800, 12800, 9800), L_.roof);
            line(tx, {-800, 4500}, {12800, 4500}, L_.roof);  // ridge
            line(tx, {-800, -800}, {0, 0}, L_.roof);
            add(tx, rectHatch(-800, -800, 12800, 9800, "LINE45", 40, Color::index(9)), L_.hatch);
            text(tx, {6000, -2700}, "ผังหลังคา", 4.0);
            setCurrent(tx, kLevelDefault);
        });
    }

    // ---- the A3 sheet -----------------------------------------------------------------------
    void sheet() {
        edit("layout", [&](Transaction& tx) {
            must(layout::createLayout(tx, "A3 1-100", "A3", true));
            BBox2 left, right;
            left.expand({10, 80});
            left.expand({205, 270});
            right.expand({215, 80});
            right.expand({410, 270});
            const int v1 = must(layout::addViewport(tx, "A3 1-100", left, {6000, 3300}, 100));
            const int v2 = must(layout::addViewport(tx, "A3 1-100", right, {6000, 3300}, 100));
            must(layout::setViewportLevel(tx, "A3 1-100", v1, kLevelDefault));
            must(layout::setViewportLevel(tx, "A3 1-100", v2, level2_));
            layout::TitleBlockFields f;
            f.project = "บ้านพักอาศัย 2 ชั้น (ตัวอย่าง)";
            f.drawing = "ผังพื้นชั้น 1 และชั้น 2";
            f.scale = "1:100";
            f.date = "2026-10-13";
            f.sheet = "A-101";
            must(layout::addTitleBlock(tx, "A3 1-100", f));
        });
    }

    std::unique_ptr<Document> doc_;
    Layers L_{};
    LevelId level2_{}, level3_{};
    TextStyleId thai_{};
    DimStyleId dimStyle_{};
    LinetypeId dashed_{};
    BlockId door_{}, window_{}, bed_{}, sofa_{}, table_{}, wc_{};
};

}  // namespace

std::unique_ptr<Document> buildThaiHouse() {
    return Builder().build();
}

}  // namespace tbcad::samples
