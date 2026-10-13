#include <doctest/doctest.h>

#include "tbcad/cmd/cmd.h"

using namespace tbcad;
using namespace tbcad::cmd;

namespace {

ChangeSet addLine(Document& doc, double x) {
    Transaction tx = doc.begin("add");
    (void)tx.add(Entity{{}, LineData{{x, 0}, {x + 1, 1}}});
    return tx.commit();
}

}  // namespace

TEST_CASE("undo stack: undo, redo, labels, redo cleared by a new push") {
    Document doc;
    UndoStack st;
    st.push("one", {addLine(doc, 0)});
    st.push("two", {addLine(doc, 1)});
    CHECK(st.undoLabel() == "two");
    CHECK(doc.entityCount() == 2);

    CHECK(st.undo(doc));
    CHECK(doc.entityCount() == 1);
    CHECK(st.redoLabel() == "two");
    CHECK(st.undoLabels() == std::vector<std::string>{"one"});
    CHECK(st.redo(doc));
    CHECK(doc.entityCount() == 2);

    st.undo(doc);
    st.push("three", {addLine(doc, 2)});
    CHECK_FALSE(st.canRedo());
    CHECK_FALSE(st.redo(doc));
    CHECK(st.undoCount() == 2);
}

TEST_CASE("undo stack: empty change sets are not recorded") {
    Document doc;
    UndoStack st;
    Transaction tx = doc.begin("nothing");
    st.push("nothing", {tx.commit()});
    CHECK_FALSE(st.canUndo());
}

TEST_CASE("undo stack: groups collapse to one step") {
    Document doc;
    UndoStack st;
    st.beginGroup("macro");
    st.push("a", {addLine(doc, 0)});
    st.beginGroup("inner");  // nested groups are fine
    st.push("b", {addLine(doc, 1)});
    CHECK(st.endGroup());
    CHECK_FALSE(st.undo(doc));  // refused while the outer group is open
    CHECK(st.endGroup());
    CHECK_FALSE(st.endGroup());
    CHECK(st.undoCount() == 1);
    CHECK(st.undoLabel() == "macro");
    st.undo(doc);
    CHECK(doc.entityCount() == 0);
    st.redo(doc);
    CHECK(doc.entityCount() == 2);
}

TEST_CASE("undo stack: memory bound drops the oldest steps and says so") {
    Document doc;
    UndoStack st(512u * 1024 * 1024);
    std::vector<std::string> messages;
    st.setMessageSink([&](std::string m) { messages.push_back(std::move(m)); });

    constexpr std::size_t kMiB = 1024 * 1024;
    // 2 GB of snapshots in 1 MiB steps.
    for (int i = 0; i < 2048; ++i) {
        st.push("big " + std::to_string(i), {addLine(doc, i)}, kMiB);
        CHECK(st.usedBytes() <= st.limitBytes());
    }
    CHECK(st.usedBytes() <= 512 * kMiB);
    CHECK(st.undoCount() == 512);
    CHECK(st.undoLabel() == "big 2047");  // the newest survive
    CHECK_FALSE(messages.empty());

    // Lowering the limit trims immediately.
    st.setLimitBytes(10 * kMiB);
    CHECK(st.undoCount() == 10);
    CHECK(st.usedBytes() <= 10 * kMiB);

    // A single step larger than the whole budget cannot be kept.
    st.push("huge", {addLine(doc, 5000)}, 64 * kMiB);
    CHECK(st.usedBytes() <= st.limitBytes());
}

TEST_CASE("undo stack: size estimate grows with the data") {
    Document doc;
    Transaction tx = doc.begin("poly");
    PolylineData pd;
    pd.v.resize(100000);
    (void)tx.add(Entity{{}, pd});
    ChangeSet big = tx.commit();
    ChangeSet small = addLine(doc, 0);
    CHECK(estimateBytes(big) > 100000 * sizeof(PolyVertex));
    CHECK(estimateBytes(big) > estimateBytes(small));
}
