# Live collaboration: core (Q61)

This is the library part of the plan in `build/collab-brief.md`: everything that must be exactly right before sockets and UI are added. It has no networking. Layout:

| Where | What |
|---|---|
| `libs/core` | `Document::setEntityIdWindow`, `applyChecked`, `drawOrderOverride` |
| `libs/io` | `io::Delta` (the "after" half of a `ChangeSet` on the wire), `findOptionalChunk` / `ExtraChunk` for the `VERS` chunk |
| `libs/collab` | versions, id ranges, `Op` / `Accepted`, `Host`, `Replica`, snapshot helpers |

## The model in one page

The host owns the authoritative document. Every edit is a `ChangeSet`; what travels is its **after image** (`io::Delta`), encoded with the same entity and row codecs as `.tbcad`. The receiver rebuilds the *before* image from its own document (`io::materialize`), so a sender can not lie about it, and observers (renderer, spatial index) get a complete `ChangeSet`.

* **Versions.** Every entity, table row, the header and each draw-order entry has a version: the sequence number of the last accepted op that touched it. An `Op` carries, for each thing it touches, the version its author saw and whether the thing existed (`Base`). The host accepts it only if all of them are still current. A failure is `RejectReason::Stale` and names the thing that was changed first. Entries of erased things are kept, so a late edit of an erased entity is stale, not "new".
* **Ids.** `Host::grantRange` gives a guest a block of entity ids (default 65536) taken from the document's counter; the host's own ids start above every block. The guest's document draws from that window (`Document::setEntityIdWindow`), so ids stay plain integers and unique. A guest that creates an entity outside its blocks is refused (`OutsideRange`). Table rows are not ranged: two guests creating the same row id are told apart by the existence check (the second is refused).
* **Validation.** `Host::submit` checks, in order: peer and role, author, rate limit, bases against the edit, versions and existence, id ranges, then `Document::applyChecked`, which applies the change only if every entity and row passes the rules a `Transaction` enforces (known layer / linetype / style / block, unique names, no dangling references, block membership, header sanity, finite geometry) and leaves the document and its observers untouched otherwise.
* **Decoding hostile input.** `io::decodeDelta`, `decodeOp`, `decodeAccepted` and `decodeVersions` never throw, bound every allocation by the bytes left and by `io::DeltaLimits`, and are fuzzed in the tests.
* **Guest side.** `Replica` watches its document: each local commit becomes an `Op` (`onLocalOp`), one in flight at a time (the next is released when the host answers, with its bases moved on by `rebasePending`). The document is always "confirmed state plus pending edits". A broadcast from someone else takes the pending edits off, applies the broadcast and puts the pending edits back by replaying their after images; a refusal (`Replica::reject`) takes that edit out and replays the ones after it. An edit that no longer fits after a replay is kept as dead: the host refuses it too.
* **Snapshot.** `saveSnapshot` / `loadSnapshot` write and read a `.tbcad` image plus the versions and sequence number in an optional `VERS` chunk. Programs that do not know the chunk ignore it and write it back unchanged.

## What the session engine (Q62) still has to do

* Move `Op` / `Accepted` / refusals over a connection and call `Host::submit`, `Replica::apply`, `Replica::reject`.
* Ask for another id block when `Replica::idsLeft()` runs low.
* Tell the user why an edit was refused (`Rejection::message`, `Rejection::key`).
* Per-user undo (undo is a new local edit, so it goes through the same version check).
* Decide which header fields are shared: `Delta::header` is the whole header; `activeOnly` and file-carried chunks are never overwritten, but things like the current layer may be better kept per peer.
* Layer locks and viewer enforcement beyond the role flag (Q64).

## Tests

`libs/io/tests/delta_tests.cpp` (round trip of an edit series, refused changes, limits, decoder fuzz, optional chunk), `libs/collab/tests/*`: versions, ids, ops (incl. fuzz), host rules, and `convergence_tests.cpp`, which runs a host and three guests with random edits and random message delays for 40 seeds and requires identical drawings, versions and sequence numbers at the end (600 seeds were run while developing).
