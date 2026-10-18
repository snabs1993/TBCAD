#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

#include "tbcad/core/core.h"

using namespace tbcad;

namespace {

struct Step {
    ChangeSet changes;
    std::string before;
    std::string after;
};

Entity randomEntity(std::mt19937& rng) {
    std::uniform_real_distribution<double> c(-100, 100), r(0.5, 20);
    switch (rng() % 4) {
        case 0:
            return Entity{{}, LineData{{c(rng), c(rng)}, {c(rng), c(rng)}}};
        case 1:
            return Entity{{}, CircleData{{c(rng), c(rng)}, r(rng)}};
        case 2:
            return Entity{{}, ArcData{{c(rng), c(rng)}, r(rng), 0.2, 2.5}};
        default:
            return Entity{{}, PointData{{c(rng), c(rng)}}};
    }
}

// One random edit; every kind of change the document supports.
void randomEdit(Document& doc, Transaction& tx, std::mt19937& rng, std::vector<EntityId>& ids) {
    std::uniform_real_distribution<double> c(-100, 100);
    const int ops = 1 + static_cast<int>(rng() % 4);
    for (int k = 0; k < ops; ++k) {
        switch (rng() % 15) {
            case 0:
            case 1: {
                auto id = tx.add(randomEntity(rng));
                if (id)
                    ids.push_back(id.value());
                break;
            }
            case 2:
                if (!ids.empty()) {
                    const EntityId id = ids[rng() % ids.size()];
                    (void)tx.modify(id, [&](Entity& e) {
                        e.common.color = Color::index(static_cast<int>(rng() % 200) + 1);
                        if (auto* l = std::get_if<LineData>(&e.data))
                            l->b = {c(rng), c(rng)};
                    });
                }
                break;
            case 3:
                if (!ids.empty()) {
                    const std::size_t i = rng() % ids.size();
                    (void)tx.erase(ids[i]);
                    ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(i));
                }
                break;
            case 4:
                (void)tx.addLayer(Layer{.name = "L" + std::to_string(rng() % 1000),
                                        .color = Color::index(static_cast<int>(rng() % 200) + 1)});
                break;
            case 5: {
                std::vector<LayerId> layers;
                for (const auto& [id, row] : doc.layers())
                    layers.push_back(id);
                if (layers.size() > 1)
                    (void)tx.modifyLayer(layers[1 + rng() % (layers.size() - 1)],
                                         [&](Layer& l) { l.color = Color::index(7); });
                break;
            }
            case 6:
                if (!ids.empty())
                    (void)tx.bringToFront(ids[rng() % ids.size()]);
                break;
            case 7:
                if (!ids.empty())
                    (void)tx.sendToBack(ids[rng() % ids.size()]);
                break;
            case 8:
                (void)tx.addBlock(BlockDef{.name = "B" + std::to_string(rng() % 1000)});
                break;
            case 9: {
                std::vector<BlockId> blocks;
                for (const auto& [id, row] : doc.blocks())
                    blocks.push_back(id);
                if (!blocks.empty()) {
                    const BlockId b = blocks[rng() % blocks.size()];
                    if (rng() % 2 == 0)
                        (void)tx.eraseBlock(b);
                    else
                        (void)tx.modifyBlock(b, [&](BlockDef& d) { d.base = {c(rng), c(rng)}; });
                }
                break;
            }
            case 10: {
                std::vector<LayerId> layers;
                for (const auto& [id, row] : doc.layers())
                    layers.push_back(id);
                if (layers.size() > 1)
                    (void)tx.eraseLayer(layers[1 + rng() % (layers.size() - 1)]);
                break;
            }
            case 11:
                (void)tx.addTextStyle(TextStyle{.name = "S" + std::to_string(rng() % 1000)});
                break;
            case 12:
                (void)tx.addDimStyle(DimStyle{.name = "D" + std::to_string(rng() % 1000)});
                break;
            case 13:
                (void)tx.addLinetype(Linetype{.name = "T" + std::to_string(rng() % 1000)});
                break;
            default:
                (void)tx.modifyHeader([&](Header& h) { h.ltScale = 0.5 + (rng() % 100) / 50.0; });
                break;
        }
    }
}
}  // namespace

TEST_CASE("property: any run of random edits undoes and redoes through every intermediate state") {
    for (unsigned seed = 1; seed <= 25; ++seed) {
        std::mt19937 rng(seed);
        Document doc;
        std::vector<EntityId> ids;
        std::vector<Step> done;
        for (int n = 0; n < 40; ++n) {
            Step s;
            s.before = canonicalDump(doc);
            Transaction tx = doc.begin("random");
            randomEdit(doc, tx, rng, ids);
            s.changes = tx.commit();
            s.after = canonicalDump(doc);
            done.push_back(std::move(s));
        }
        // Back to the start, checking every state on the way.
        for (std::size_t i = done.size(); i-- > 0;) {
            doc.apply(done[i].changes, Direction::Undo);
            REQUIRE(canonicalDump(doc) == done[i].before);
        }
        // And forward again.
        for (std::size_t i = 0; i < done.size(); ++i) {
            doc.apply(done[i].changes, Direction::Redo);
            REQUIRE(canonicalDump(doc) == done[i].after);
        }
        // Random walk up and down the history.
        std::size_t at = done.size();
        for (int k = 0; k < 200; ++k) {
            if (at > 0 && (at == done.size() || rng() % 2 == 0)) {
                --at;
                doc.apply(done[at].changes, Direction::Undo);
                REQUIRE(canonicalDump(doc) == done[at].before);
            } else {
                doc.apply(done[at].changes, Direction::Redo);
                REQUIRE(canonicalDump(doc) == done[at].after);
                ++at;
            }
        }
    }
}

TEST_CASE("property: a rolled back transaction leaves no trace, whatever it did") {
    std::mt19937 rng(77);
    Document doc;
    std::vector<EntityId> ids;
    for (int n = 0; n < 30; ++n) {
        {
            Transaction tx = doc.begin("keep");
            randomEdit(doc, tx, rng, ids);
            tx.commit();
        }
        const std::string before = canonicalDump(doc);
        std::vector<EntityId> scratch = ids;
        Transaction tx = doc.begin("abandon");
        randomEdit(doc, tx, rng, scratch);
        tx.rollback();
        REQUIRE(canonicalDump(doc) == before);
    }
}
