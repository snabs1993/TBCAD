// Display lists must stay inside the object they draw: a stray vertex far outside an entity's own
// bounds shows up on screen and on paper as a long line that cannot be selected.
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "reader_helpers.h"
#include "tbcad/blocks/display.h"
#include "tbcad/dim/dim.h"
#include "tbcad/geom/entity.h"
#include "tbcad/hatch/hatch.h"
#include "tbcad/mline/mline.h"
#include "tbcad/table/table.h"
#include "tbcad/text/text.h"

using namespace tbcad;
using namespace tbcad::dxf;
using namespace tbcad::dxf::testing;

namespace {

void registerAll() {
    static const bool once = [] {
        dim::registerDisplay();
        text::registerDisplay();
        mline::registerDisplay();
        table::registerDisplay();
        hatch::registerHatchDisplay();
        blocks::registerInsertDisplay();
        return true;
    }();
    (void)once;
}

// Entities whose display reaches outside their bounds on purpose.
bool unbounded(const Entity& e) {
    const EntityKind k = kind(e);
    // Annotative objects change size with the viewport scale; their bounds follow the drawing's.
    return k == EntityKind::Ray || k == EntityKind::Proxy || e.common.annotative;
}

// n = 0: fine tessellation; otherwise the coarse one of a layout viewport at 1:n.
std::string strays(const Document& doc, double n) {
    std::ostringstream out;
    int count = 0;
    doc.forEachEntity([&](const Entity& e) {
        if (doc.ownerBlock(e.common.id) || unbounded(e))
            return;
        const BBox2 box = geom::bounds(e, doc);
        if (box.empty())
            return;
        const double diag = std::hypot(box.width(), box.height());
        geom::DisplayContext ctx;
        // Fine tessellation, or the coarse one of a viewport at 1:N (a hairline per pixel).
        ctx.pixelSizeMm = n > 0 ? 0.1 * n : std::max(diag, 1e-3) / 1000.0;
        if (n > 0)
            ctx.chordTolMm = 0.05 * n;
        ctx.annoScale = n;
        DisplayList list;
        geom::buildDisplay(doc, e, ctx, list);
        const BBox2 reach = box.inflated(0.25 * diag + 10 * ctx.pixelSizeMm + 4 * ctx.chordTolMm);
        for (const DisplayItem& item : list.items) {
            std::vector<Vec2> pts;
            if (const auto* pl = std::get_if<DisplayPolyline>(&item))
                pts = pl->pts;
            else if (const auto* f = std::get_if<DisplayFill>(&item))
                for (const auto& ring : f->rings)
                    pts.insert(pts.end(), ring.begin(), ring.end());
            else if (const auto* d = std::get_if<DisplayPoints>(&item))
                pts = d->pts;
            for (const Vec2& p : pts)
                if (!reach.contains(p) && count++ < 20) {
                    out << "entity " << e.common.id.value << " kind " << static_cast<int>(kind(e))
                        << " point (" << p.x << ", " << p.y << ") bounds (" << box.min.x << ", "
                        << box.min.y << ")-(" << box.max.x << ", " << box.max.y << ")\n";
                    return;
                }
        }
    });
    return out.str();
}

}  // namespace

TEST_CASE("no object draws outside its own bounds - every reader corpus file") {
    registerAll();
    std::vector<std::filesystem::path> all;
    for (const char* sub : {"reader", "roundtrip"})
        for (const auto& entry : std::filesystem::directory_iterator(corpusPath(sub)))
            all.push_back(entry.path());
    all.push_back(std::filesystem::path(corpusPath("../../samples/thai-house.dxf")));
    int files = 0;
    for (const std::filesystem::path& file : all) {
        if (file.extension() != ".dxf" || file.filename().string().rfind("broken_", 0) == 0)
            continue;
        std::ifstream in(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Report report;
        auto r = importDxf(bytesOf(text), {}, report);
        if (!r.ok())
            continue;
        ++files;
        for (double n : {0.0, 1.0, 50.0}) {
            const std::string bad = strays(*r.value().document, n);
            CHECK_MESSAGE(bad.empty(), file.filename().string() << " at 1:" << n << ":\n" << bad);
        }
    }
    CHECK(files > 10);
}
