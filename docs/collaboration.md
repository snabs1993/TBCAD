# Live collaboration: core (Q61) and session engine (Q62)

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

## Session engine (Q62)

Everything above the wire, without sockets: `protocol.h` (one `Message` per frame), `HostSession`, `GuestSession`. The owner connects them to `net::Channel` frames (Q63) or, in tests, to queues.

* **Messages.** Hello, Welcome (peer id, role, sequence, first id block, who is here, the `.tbcad` snapshot with versions), Denied, Op, Accepted, Refused, RangeRequest/RangeGrant, PeerJoined/PeerLeft/RoleChanged, Bye. Decoding never throws; unknown or damaged input ends the connection. A snapshot larger than `net::kMaxFrameBytes` (16 MB) can not be sent yet: chunked snapshots are a follow-up.
* **Host.** `HostSession::open(send)` per connection, `receive(connection, frame, now)`, `close`. The handshake asks `approve(name)` ("Nok wants to join, Allow?"). Accepted edits (the host's own included) are broadcast to everyone; a refused edit goes back to its author only, as `Refused` with the reason and, for a conflict, who changed the thing first (the host remembers the author of the last 4096 edits). `kick`, `setRole`, `shutdown`.
* **Guest.** `GuestSession::start()`, `receive(frame)`, `leave()`, `connectionLost()`. After Welcome `document()` is the guest's copy; every edit is sent optimistically through the `Replica`. Events: `onJoined`, `onPeerJoined/Left`, `onRoleChanged`, `onRemoteChange`, `onConflict` (text ready to show: "Somchai changed this first, so your change was undone."), `onEnded`. More ids are requested automatically when fewer than 1000 are left.
* **Per-user undo.** `GuestSession::undo()/redo()` take back only this user's own accepted edits, by sending the inverse as a new edit. It is refused (and the entry dropped, so the user is not stuck behind it) when anyone has changed any of the things it touched since: the guest checks the versions, the host checks them again. One edit is in flight at a time, so undo asks the user to wait while an earlier change is still being saved.
* **Per-user settings.** The current layer, level, level view, colour, linetype, lineweight, text and dimension style and Active Only are the user's own: a header change that touches only those is not sent, and a header change from somebody else keeps the receiver's values (`keepPersonalSettings`). Shared header fields (units, work area, linetype scale, custom keys) travel and are versioned like everything else.
* **Not here yet.** Layer claim locks, kicking by vote, chat (Q64); offline queue and conflict list (Q65); presence cursors (Q63, separate unreliable channel); sockets and TLS wiring (Q63).

## Tests

`libs/io/tests/delta_tests.cpp` (round trip of an edit series, refused changes, limits, decoder fuzz, optional chunk), `libs/collab/tests/*`: versions, ids, ops (incl. fuzz), host rules, and `session_tests.cpp` (join, deny, edits, conflicts with the winner's name, viewer, kick, id refill, per-user undo and its refusal, personal settings, random edits through the real protocol, message fuzz), and `convergence_tests.cpp`, which runs a host and three guests with random edits and random message delays for 40 seeds and requires identical drawings, versions and sequence numbers at the end (600 seeds were run while developing).
