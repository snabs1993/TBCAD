# Spatial index (`tbcad::index`)

`libs/index` answers "which entities are near here?" for viewport culling, picking and snap
candidate search. It stores one bounding box per entity and knows nothing about geometry.

## Types

- `SpatialIndex`: boxes keyed by `EntityId`.
- `DocumentIndexer`: a `DocumentObserver` that keeps a `SpatialIndex` equal to the model-space
  entities of a `Document`.

## `SpatialIndex`

| Call | Meaning |
| --- | --- |
| `bulkLoad(span<const IdBox>)` | Replace the contents. Hilbert-sorted, packed tree. Repeated ids keep the first; unusable boxes are skipped. |
| `insert(id, box)` | False if the id exists or the box is empty/non-finite. |
| `remove(id)` | False if absent. |
| `update(id, box)` | Move to a new box (inserts when absent). Done in place when the leaf still covers the box. |
| `query(window, cb)` | Every box that intersects `window` (touching counts). |
| `queryPoint(p, r, cb)` | Every box within Euclidean distance `r` of `p`. |
| `nearest(p, k, maxDist)` | The `k` nearest boxes, nearest first, ties by id. |
| `size()`, `clear()`, `contains`, `boxOf`, `bounds`, `rebalance()` | Housekeeping. |

Callbacks take `(EntityId, const BBox2&)`. Return `false` to stop the query early (`true` or
`void` continues). Results come in no particular order. Do not modify the index from a callback.
Reads are thread-safe while nobody writes.

**Single writer.** `SpatialIndex` has no locks. Any number of threads may read at once, but a
write (`bulkLoad`, `insert`, `remove`, `update`, `clear`, `rebalance`) needs the caller to be the only
user of the index at that moment. In the application the `DocumentIndexer` is the only writer and it
runs on the thread that commits the transaction; code on other threads must either not touch the
index or be stopped while a commit is in progress.

**Numbers in CI.** The `perf` job in `.github/workflows/ci.yml` builds a Release configuration and
prints the 1 M box bulk load, the viewport query and the render benchmark into the job summary on
every run (the render numbers come from Mesa's software renderer and only show trends).

## How it works

- **Tree.** An R-tree with 32 entries per node. `bulkLoad` sorts box centres by a 64-bit Hilbert
  key (32 bits per axis, so a 100 km site still resolves to about 25 µm), radix sorts, and packs
  nodes about 88 % full. Single edits use R\*-tree style subtree choice and splits (axis by margin,
  distribution by overlap), and removal re-inserts the entries of underfull nodes. A uniform grid
  is deliberately not used: it fails on drawings that mix a 100 km site with 1 mm details.
- **Large boxes.** A box whose area exceeds 25 % of the tree's area (a background hatch, a
  construction line given a huge extent) goes into a separate list that every query scans linearly.
  Otherwise such boxes overlap many nodes and slow every query. The rule only applies once the
  index holds 64 entries. `rebalance()` re-applies it to current contents.
- **Id map.** A flat open-addressing table maps an id to its leaf, so `remove` and `update` do not
  search.
- **Zero-area boxes.** Horizontal and vertical lines have zero area; subtree choice falls back to
  perimeter growth, so they index well.

## `DocumentIndexer`

```cpp
tbcad::Document doc;
tbcad::index::SpatialIndex idx;
tbcad::index::DocumentIndexer indexer(doc, idx, [](const tbcad::Entity& e) {
    return boundsOf(e);   // any callable returning a BBox2, e.g. geom::bounds
});
```

- Loads every model-space entity on construction and mirrors each commit, undo and redo.
  Entities inside block definitions are not indexed.
- The box callback returns an empty `BBox2` for "not indexed" (unsupported kind, no geometry).
- A single commit that touches more than half the index (and over 1000 entities) is applied as a
  bulk load.
- The callback sees only the entity. When a block definition changes, the boxes of its `Insert`
  entities change, which the indexer cannot see: call `reindex(id)` for those or `rebuild()`.
- The indexer must not outlive the document or the index; it unregisters itself on destruction.

## Numbers

GCC 13 Release build in a Linux container, best of several runs:

| Gate | Target | Measured |
| --- | --- | --- |
| `bulkLoad` 1 M boxes | < 300 ms | about 160-175 ms |
| viewport query returning about 10,000 boxes | < 2 ms | about 0.03 ms |

The tests print these on every Release run (`[index perf]`).
