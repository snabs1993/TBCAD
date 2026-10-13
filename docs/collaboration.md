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

## The session engine (Q62)

`libs/collab/include/tbcad/collab/session.h`. It works on whole frames and has no sockets, so it runs the same over TCP/TLS (Q60's `libs/net`, `libs/nettls`) and over the in-memory queues of the tests. It is single threaded: the transport calls `receive()` on the thread that owns the document.

| Message | From | Meaning |
|---|---|---|
| Hello (protocol version, name) | guest | asks to join |
| Welcome (peer id, role, session name, peers, first id block, snapshot) | host | the drawing as a `.tbcad` image with versions, then live |
| Op | guest | one edit (`encodeOp`) |
| Accepted | host to all | the ordered edit, including the author's own (their acknowledgement) |
| Rejected (client seq, reason, text, key, version) | host to the author | the edit did not fit: roll it back |
| GrantRequest / Grant | guest / host | another block of entity ids |
| PeerJoined / PeerLeft / RoleChanged / Kicked / Error / Bye | | who is in, and why a session ended |

* `HostSession` wraps a `Host`: `accept()` a connection, `receive()` its frames, `disconnect()` when it drops. It asks `approve(name, role)` before letting a guest in (the host's "X wants to join, allow?" question; it may admit as viewer), calls `onPeer` and `onRefused` for the host's UI, and offers `kick()` and `setRole()`. A frame that does not decode, an unknown message or a message before Hello ends that connection with an Error frame.
* `GuestSession` joins, builds the `Document` and `Replica` from the snapshot (`onDocument` lets the application hook the new document up before the first edit), sends edits, applies broadcasts and, when fewer than half of its first id block is left, asks for another one. A refusal reaches the user as a `Conflict`: the reason, the host's words, the thing that was changed first and (when still remembered) who changed it, plus the change that left the document again. The last 4096 sequence numbers are remembered with their authors for that.
* **Per-user undo.** Each person's `cmd::UndoStack` holds only their own steps. Undoing "puts back what I had", which is wrong if somebody changed the same thing since. `collab::checkUndoRedo` (set with `UndoStack::setGuard`) compares the drawing with the images the step would restore over and refuses with "Cannot undo: the entity was changed by someone else since."; the refused step is dropped so the next undo works on the older step. The host cannot catch this itself, because the undo is a fresh edit made on the latest versions. The guard is therefore what the application must install when it joins a session (Q63).
* The header is shared as a whole (the host validates it like any other row); `activeOnly` and file-carried chunks are never overwritten.
* Layer locks and enforcement beyond the viewer role are Q64.

## Tests

`libs/io/tests/delta_tests.cpp` (round trip of an edit series, refused changes, limits, decoder fuzz, optional chunk), `libs/collab/tests/*`: versions, ids, ops (incl. fuzz), host rules, `undo_guard_tests.cpp`, `session_tests.cpp` (join, peers, random edits by three guests and the host with frames delivered in random order for 20 seeds, id top-up, conflict messages with the name of who was first, undo, kick, viewer and promotion, refused joins, damaged frames) and `convergence_tests.cpp`, which runs a host and three guests with random edits and random message delays for 40 seeds and requires identical drawings, versions and sequence numbers at the end (600 seeds were run while developing).
