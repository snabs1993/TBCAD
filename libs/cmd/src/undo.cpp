#include "tbcad/cmd/undo.h"

#include <algorithm>
#include <unordered_set>

namespace tbcad::cmd {

namespace {

std::size_t entityBytes(const Entity& e) {
    std::size_t n = sizeof(Entity);
    visitEntity(e, [&](const auto& d) {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, PolylineData>) {
            n += d.v.capacity() * sizeof(PolyVertex);
        } else if constexpr (std::is_same_v<T, SplineData>) {
            n += d.knots.capacity() * sizeof(double) + d.control.capacity() * sizeof(Vec2) +
                 d.weights.capacity() * sizeof(double) + d.fit.capacity() * sizeof(Vec2) +
                 sizeof(T);
        } else if constexpr (std::is_same_v<T, TextData> || std::is_same_v<T, MTextData>) {
            n += d.utf8.capacity() + sizeof(T);
        } else if constexpr (std::is_same_v<T, HatchData>) {
            n += sizeof(T) + d.pattern.capacity() + d.sourceBoundary.capacity() * sizeof(EntityId);
            for (const HatchLoop& loop : d.loops)
                n += sizeof(HatchLoop) + loop.edges.capacity() * sizeof(HatchEdge);
        } else if constexpr (std::is_same_v<T, LeaderData>) {
            n += sizeof(T) + d.vertices.capacity() * sizeof(Vec2) + d.utf8.capacity();
        } else if constexpr (std::is_same_v<T, TableData>) {
            n += sizeof(T) + d.colWidths.capacity() * sizeof(double) +
                 d.rowHeights.capacity() * sizeof(double) + d.cells.capacity() * sizeof(TableCell);
            for (const TableCell& c : d.cells)
                n += c.utf8.capacity();
        } else if constexpr (std::is_same_v<T, ProxyData>) {
            n += sizeof(T) + d.originalType.capacity() + d.raw.capacity();
        } else if constexpr (std::is_same_v<T, EllipseData> || std::is_same_v<T, InsertData> ||
                             std::is_same_v<T, DimensionData>) {
            n += sizeof(T);
        }
    });
    return n;
}

}  // namespace

std::size_t estimateBytes(const ChangeSet& cs) {
    std::size_t n = sizeof(ChangeSet) + cs.name.capacity();
    if (cs.snapshots) {
        n += sizeof(*cs.snapshots);
        for (const Entity& e : *cs.snapshots)
            n += entityBytes(e);
    }
    n += cs.entities.capacity() * sizeof(EntityChange);
    constexpr std::size_t kRow = 256;  // table rows are small; a flat guess is enough
    n += (cs.layers.size() + cs.linetypes.size() + cs.textStyles.size() + cs.dimStyles.size() +
          cs.blocks.size()) *
         kRow * 2;
    n += cs.order.capacity() * sizeof(OrderChange) + cs.outOfArea.capacity() * sizeof(EntityId);
    if (cs.header)
        n += sizeof(HeaderChange);
    return n;
}

std::vector<std::string> UndoStack::undoLabels() const {
    std::vector<std::string> out;
    out.reserve(undo_.size());
    for (const Entry& e : undo_)
        out.push_back(e.label);
    return out;
}

void UndoStack::push(std::string label, std::vector<ChangeSet> sets, std::size_t bytes) {
    std::erase_if(sets, [](const ChangeSet& cs) { return cs.empty(); });
    if (sets.empty())
        return;
    if (bytes == 0) {
        std::unordered_set<const void*> seen;  // snapshots are shared between sets
        for (const ChangeSet& cs : sets) {
            std::size_t b = estimateBytes(cs);
            if (!seen.insert(cs.snapshots.get()).second && cs.snapshots)
                b -= std::min(b, sizeof(*cs.snapshots));
            bytes += b;
        }
    }
    if (groupDepth_ > 0) {
        for (ChangeSet& cs : sets)
            group_.sets.push_back(std::move(cs));
        group_.bytes += bytes;
        return;
    }
    record(Entry{std::move(label), std::move(sets), bytes});
}

void UndoStack::record(Entry e) {
    redo_.clear();
    used_ += e.bytes;
    undo_.push_back(std::move(e));
    trim();
}

void UndoStack::trim() {
    std::size_t dropped = 0;
    while (used_ > limit_ && !undo_.empty()) {
        used_ -= undo_.front().bytes;
        undo_.pop_front();
        ++dropped;
    }
    if (dropped > 0) {
        dropped_ += dropped;
        if (sink_)
            sink_("Undo history is full: dropped the " + std::to_string(dropped) +
                  " oldest step(s).");
    }
}

void UndoStack::setLimitBytes(std::size_t limit) {
    limit_ = limit;
    used_ = 0;
    for (const Entry& x : undo_)
        used_ += x.bytes;
    trim();
}

bool UndoStack::undo(Document& doc) {
    if (undo_.empty() || groupDepth_ > 0)
        return false;
    blocked_.clear();
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    used_ -= std::min(used_, e.bytes);
    if (guard_) {
        std::string why = guard_(doc, e.sets, Direction::Undo);
        if (!why.empty()) {
            blocked_ = std::move(why);
            return false;
        }
    }
    for (auto it = e.sets.rbegin(); it != e.sets.rend(); ++it)
        doc.apply(*it, Direction::Undo);
    redo_.push_back(std::move(e));
    return true;
}

bool UndoStack::redo(Document& doc) {
    if (redo_.empty() || groupDepth_ > 0)
        return false;
    blocked_.clear();
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    if (guard_) {
        std::string why = guard_(doc, e.sets, Direction::Redo);
        if (!why.empty()) {
            blocked_ = std::move(why);
            return false;
        }
    }
    for (const ChangeSet& cs : e.sets)
        doc.apply(cs, Direction::Redo);
    used_ += e.bytes;
    undo_.push_back(std::move(e));
    trim();
    return true;
}

void UndoStack::beginGroup(std::string label) {
    if (groupDepth_++ == 0) {
        group_ = Entry{};
        group_.label = std::move(label);
    }
}

bool UndoStack::endGroup() {
    if (groupDepth_ == 0)
        return false;
    if (--groupDepth_ == 0 && !group_.sets.empty()) {
        Entry e = std::move(group_);
        group_ = Entry{};
        record(std::move(e));
    }
    return true;
}

void UndoStack::clear() {
    undo_.clear();
    redo_.clear();
    used_ = 0;
    groupDepth_ = 0;
    group_ = Entry{};
}

}  // namespace tbcad::cmd
