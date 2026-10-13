#include "helpers.h"
#include "tbcad/collab/undo_guard.h"

using namespace tbcad;
using namespace tbcad::collab;
using namespace tbcad::collab::testing;

namespace {

// The "other person": changes arrive through applyChecked, as in a Replica.
void otherMovesLine(Document& doc, EntityId id, Vec2 to) {
    Transaction tx = doc.begin("other");
    (void)tx.modify(id, [&](Entity& e) {
        if (auto* l = std::get_if<LineData>(&e.data))
            l->b = to;
    });
    // A commit by someone else would reach this document as applyChecked of their change set.
    tx.commit();
}

}  // namespace

TEST_CASE("undo guard: nobody else touched it, so undo and redo are allowed") {
    Document doc;
    Transaction tx = doc.begin("line");
    EntityId id = must(tx.add(line(kLayerZero, {0, 0}, {10, 0})));
    ChangeSet add = tx.commit();
    std::vector<ChangeSet> entry{add};
    CHECK(checkUndoRedo(doc, entry, Direction::Undo).empty());
    doc.apply(add, Direction::Undo);
    CHECK(!doc.contains(id));
    CHECK(checkUndoRedo(doc, entry, Direction::Redo).empty());
}

TEST_CASE("undo guard: undo is refused when someone changed the entity afterwards") {
    Document doc;
    Transaction tx = doc.begin("line");
    EntityId id = must(tx.add(line(kLayerZero, {0, 0}, {10, 0})));
    ChangeSet add = tx.commit();
    otherMovesLine(doc, id, {20, 5});
    std::vector<ChangeSet> entry{add};
    std::string why = checkUndoRedo(doc, entry, Direction::Undo);
    CHECK(why.find("Cannot undo") == 0);
    CHECK(why.find("entity") != std::string::npos);
}

TEST_CASE("undo guard: undoing a move is refused when the entity was erased meanwhile") {
    Document doc;
    Transaction t1 = doc.begin("line");
    EntityId id = must(t1.add(line(kLayerZero, {0, 0}, {10, 0})));
    t1.commit();
    Transaction t2 = doc.begin("move");
    (void)t2.modify(id, [](Entity& e) {
        if (auto* l = std::get_if<LineData>(&e.data))
            l->b = {5, 5};
    });
    ChangeSet move = t2.commit();
    Transaction t3 = doc.begin("erase");
    (void)t3.erase(id);
    t3.commit();
    std::vector<ChangeSet> entry{move};
    CHECK(!checkUndoRedo(doc, entry, Direction::Undo).empty());
}

TEST_CASE("undo guard: undoing an erase is fine while the entity is still gone") {
    Document doc;
    Transaction t1 = doc.begin("line");
    EntityId id = must(t1.add(line(kLayerZero, {0, 0}, {10, 0})));
    t1.commit();
    Transaction t2 = doc.begin("erase");
    (void)t2.erase(id);
    ChangeSet erase = t2.commit();
    std::vector<ChangeSet> entry{erase};
    CHECK(checkUndoRedo(doc, entry, Direction::Undo).empty());
}

TEST_CASE("undo guard: a step with several change sets checks the last one touching each thing") {
    Document doc;
    Transaction t1 = doc.begin("line");
    EntityId id = must(t1.add(line(kLayerZero, {0, 0}, {10, 0})));
    ChangeSet first = t1.commit();
    Transaction t2 = doc.begin("tweak");
    (void)t2.modify(id, [](Entity& e) {
        if (auto* l = std::get_if<LineData>(&e.data))
            l->b = {7, 7};
    });
    ChangeSet second = t2.commit();
    std::vector<ChangeSet> entry{first, second};
    CHECK(checkUndoRedo(doc, entry, Direction::Undo).empty());
    otherMovesLine(doc, id, {1, 1});
    CHECK(!checkUndoRedo(doc, entry, Direction::Undo).empty());
}
