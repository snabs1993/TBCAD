# The .tbcad file format

Format version **1.4** (older files read unchanged; 1.1 adds attribute fields to Text and Insert, 1.2 the `XRFS` chunk for external references, 1.3 an array to Insert, 1.4 the `XRFO` chunk for overlay xrefs, below). This document is the specification; the reference implementation is
`libs/io` (`tbcad::io::readTbcad` / `writeTbcad`). `tools/tbcad-dump` (built from
`libs/io/tools`) prints any file as text.

Goals: lossless for everything in `tbcad::core`, safe on damaged input, fast, and forward
compatible (an older program opens a newer file without losing data it does not understand).

All integers are little-endian. Doubles are IEEE 754 binary64 stored as their raw 8 bytes, never
as text. A **varint** is an unsigned LEB128 integer (7 bits per byte, high bit = more bytes,
at most 10 bytes). **Ids** are varints; **0 is the null id**.

## File layout

```
signature   10 bytes   89 54 42 43 41 44 0D 0A 1A 0A      "\x89TBCAD\r\n\x1a\n"
formatMajor u32
formatMinor u32
flags       u32        reserved, written as 0
chunk*                 until the END chunk
```

The signature is PNG-style: the high-bit first byte, the `\r\n` pair and the `\x1a\n` pair make
text-mode transfers and line-ending conversion visible immediately ("the signature is damaged").

A reader refuses a file whose `formatMajor` is higher than it knows, with a message asking the
user to update. A higher `formatMinor` is read normally: minor versions only *add* chunks, entity
kinds, and trailing fields.

## Chunks

```
fourcc   4 bytes
flags    u32       bit 0 = payload is deflated
length   u64       size of the stored payload
payload  length bytes
crc32    u32       CRC-32 (IEEE, as in zlib/PNG) of the payload *as stored*
```

If bit 0 is set the stored payload is `u64 rawLength` followed by a zlib stream. A reader checks
`rawLength` against a sanity limit (1 GiB, and at most about 1100 times the compressed size)
before allocating. Other flag bits are reserved; a known chunk with an unknown flag bit is an
error, an unknown chunk is kept whatever its flags.

| Chunk | Content | Count |
|---|---|---|
| `HEAD` | header variables | exactly 1 |
| `STRS` | string table | exactly 1 |
| `LTYP` | linetypes | 0..1 |
| `TSTY` | text styles | 0..1 |
| `DSTY` | dimension styles | 0..1 |
| `LAYR` | layers, including the sub-layer tree | 0..1 |
| `BLKS` | block definitions | 0..1 |
| `XRFS` | external references of blocks (since 1.2) | 0..1, only when a block is an xref |
| `XRFO` | overlay flags of xrefs (since 1.4) | 0..1, only when an xref is an overlay |
| `ENTS` | entity records | 0..n (each about 8 MiB of raw data) |
| `ORDR` | model-space draw order | 0..1, only when it differs from creation order |
| *anything else* | unknown | kept |
| `END ` | empty chunk, last | exactly 1 |

The writer emits them in the order above (unknown chunks just before `END `). A reader accepts
any order, because it reads every chunk before decoding. Data after `END ` is ignored with a
warning. A file without `END ` is truncated.

**Unknown chunks** are kept as opaque blobs (type, flags and stored bytes) and written back
unchanged. In memory they travel in `Header::custom` under keys that start with
`"\x01tbcad.chunk."` followed by an 8-digit index, so the order is preserved.

## STRS

`varint count`, then `count` strings, each `varint byteLength` + UTF-8 bytes. Strings are
referenced everywhere by `varint index`. Index 0 is always the empty string. The writer interns
in first-use order, so output is deterministic. The reader rejects invalid UTF-8.

## HEAD

`varint count`, then entries: `varint keyIndex` (string), `u8 type`, value. Types: 0 = string
(`varint index`), 1 = int (`i64`), 2 = real (`f64`).

| Key | Type | Meaning |
|---|---|---|
| `units` | int | `LengthUnit` (0 mm, 1 cm, 2 m, 3 in, 4 ft) |
| `workArea` | real | half-extent of the working area in mm |
| `ltScale` | real | global linetype scale |
| `currentLayer`, `currentTextStyle`, `currentDimStyle` | int | row ids |
| `createdBy` | string | free text |
| `nextEntity` | int | next free entity id |
| `nextLayer`, `nextLinetype`, `nextTextStyle`, `nextDimStyle`, `nextBlock` | int | next free row id of each table |
| `c:<key>` | string | an entry of `Header::custom` |

Unknown keys are ignored with a warning. The `next*` keys are optional: ids are never reused, so a
file stores the next free ids, which can be higher than the highest saved id when the last rows
were deleted. A reader raises its counters to at least these values (they never lower them).
Older files without them get the id after the highest saved one.

## Tables

Each table chunk is `varint count` followed by rows **in ascending id order**. Ids are kept
exactly as saved, including gaps left by deleted rows, so ids are never reused after a reload.

**LTYP** row: `id`, `name`, `description` (string indexes), `varint n`, `n` × `f64` pattern
(mm; + dash, − gap, 0 dot; empty = continuous).

**TSTY** row: `id`, `name`, `fontFamily`, `fontFile`, `f64 widthFactor`, `f64 oblique`.

**DSTY** row: `id`, `name`, `f64 textHeight`, `varint textStyle`, `f64 textGap`, `u8 arrowType`,
`f64 arrowSize`, `f64 extLineOffset`, `f64 extLineExtension`, `f64 dimLineExtension`,
`u8 textPlacement`, `u8 textAlign`, `u8 units`, `i32 precision`, `prefix`, `suffix`,
`f64 roundOff`, `f64 overallScale`, `u8 flags` (bit 0 suppress leading zeros, bit 1 trailing),
then three colours (dim line, extension line, text).

**LAYR** row: `id`, `name`, `varint parent`, colour, `u32 linetype`, `i16 lineweight`,
`u8 flags` (bit 0 on, 1 frozen, 2 locked, 3 plot).

**BLKS** row: `id`, `name`, `f64 x`, `f64 y` (base point), `u8 anonymous`. Which entities belong
to a block is given by each entity's *owner* field; a block's members are in id order.

**XRFS** row (since 1.2): `varint block` (the id of a `BLKS` row), `string path` (as the user typed
it; relative paths are resolved against the folder of the drawing), `i64 stamp` (last-write time of
the source file in nanoseconds since the Unix epoch when the block was last loaded). The block's
entities are stored as for any block, so a drawing opens and plots without the source file.
1.0 and 1.1 readers keep the chunk as an unknown chunk and write it back unchanged.

**XRFO** (since 1.4): `varint n`, then `n` × `varint block`: the xref blocks that are *overlays* (shown here, but not carried along when this drawing is itself used as an xref). Older readers keep the chunk and treat the xref as an ordinary attach.

A **colour** is 4 bytes: `u8 kind` (0 ByLayer, 1 ByBlock, 2 Index, 3 Rgb, 4 ByParent), then
`r g b` (for Index the palette index is in `r`).

### Sub-layers

A sub-layer is a normal layer row with a non-zero `parent`. The tree is therefore stored
entirely in `LAYR`:

* `parent` is the id of the parent layer (0 = top level). A parent may have a *higher* id than
  its child (a layer can be re-parented after it was created).
* ByParent settings are stored as special field values: colour kind **4**, linetype id
  **`0xFFFFFFFD`**, lineweight **−4**. They are only valid on a layer that has a parent.
* Names are unique among siblings (case-insensitively); the same name may appear under
  different parents. Depth is at most 8; layer "0" (id 1) has no parent and no children.
* Effective on/frozen/locked/plot/colour/linetype/lineweight are *not* stored; they are computed
  from the tree when the file is loaded (`Document::effective`).

A reader that finds a cycle, a missing parent, a bad depth, a ByParent value without a parent,
or duplicate sibling names rejects the file as corrupt.

## ENTS

`varint recordCount`, then records:

```
u16 kind            EntityKind: 0 Line, 1 Circle, 2 Arc, 3 Polyline, 4 Point, 5 Ellipse,
                    6 Spline, 7 Text, 8 MText, 9 Hatch, 10 Insert, 11 Dimension, 12 Leader,
                    13 Proxy
u16 recordVersion   1 for every kind in format 1.0
u32 length          bytes that follow (common properties + payload)
common properties
kind payload
```

Records are in ascending entity id order; ids may have gaps.

**Common properties**: `varint id`, `u8 flags`, `varint layer`, then only the fields whose flag
is set:

| Flag | Field present | Default when absent |
|---|---|---|
| 1 | `u32 linetype` | ByLayer (0) |
| 2 | colour (4 bytes) | ByLayer |
| 4 | `i16 lineweight` | ByLayer (−1) |
| 8 | `f64 linetypeScale` | 1 |
| 16 | — (entity is hidden) | visible |
| 32 | `varint ownerBlock` | model space |

**Unknown kinds** (`kind` > 13, or `recordVersion` 0 or higher than the reader knows) are
decoded as far as the common properties and kept as a `ProxyData` whose `originalType` is
`"\x01tbcad-kind:<kind>:<recordVersion>"` and whose `raw` holds the payload. The writer turns
such a proxy back into the original record, so a program that does not know a kind
loses nothing when it saves. The common properties stay editable. Bytes after the payload of a
*known* kind (fields added by a later minor version) are ignored with a warning.

**Payloads** (`v2` = two `f64`):

| Kind | Payload |
|---|---|
| Line | `v2 a`, `v2 b` |
| Circle | `v2 center`, `f64 radius` |
| Arc | `v2 center`, `f64 radius`, `f64 start`, `f64 end` |
| Polyline | `u8 closed`, `f64 constantWidth`, vertices |
| Point | `v2` |
| Ellipse | `v2 center`, `v2 majorAxis`, `f64 ratio`, `f64 startParam`, `f64 endParam` |
| Spline | spline body |
| Text | `v2 position`, `v2 alignPoint`, `f64 height`, `f64 rotation`, `f64 widthFactor`, `f64 oblique`, `varint style`, `u8 hAlign`, `u8 vAlign`, `string`; since 1.1, optional trailing attribute definition: `u8 flags` (1 invisible, 2 constant, 4 verify, 8 preset, 128 always set), `string tag`, `string prompt`, `string default` |
| MText | `v2 position`, `f64 height`, `f64 width`, `f64 rotation`, `u8 attachment`, `varint style`, `string` |
| Hatch | `u8 solid`, `string pattern`, `f64 scale`, `f64 angle`, `v2 origin`, colour fill, `varint n` + `n` × `varint` source boundary ids, `varint loops` × (`u8 outer`, `varint edges` × edge) |
| Insert | `varint block`, `v2 position`, `v2 scale`, `f64 rotation`; since 1.1, optional trailing attribute values: `varint count`, then per attribute `string tag`, `string value`; since 1.3, after them (count may be 0), an optional associative array: `u8 1`, `u8 kind` (0 rectangular, 1 polar, 2 path), `varint rows`, `varint columns`, `f64 rowSpacing`, `f64 columnSpacing`, `f64 angle`, `v2 center`, `varint count`, `f64 fillAngle`, `u8 rotateItems`, `v2 reference`, `u8 pathClosed`, `f64 pathSpacing`, then the path vertices as for a polyline |
| Dimension | `u8 kind`, `f64 rotation`, 4 × `v2` defining points, `v2 textPos`, `u8 textPosUser`, `string textOverride`, `varint style`, `f64 measurement` |
| Leader | `varint n` × `v2` vertices, `varint style`, `string`, `v2 textPos` |
| Proxy | `string originalType`, `varint n` + raw bytes, `varint items` × display item |

*vertices*: `varint n` × (`v2 p`, `f64 bulge`, `f64 startWidth`, `f64 endWidth`).
*spline body*: `i32 degree`, `u8 flags` (bit 0 closed, bit 1 periodic), then four lists, each
`varint n` + elements: knots (`f64`), control points (`v2`), weights (`f64`), fit points (`v2`).
*hatch edge*: `u8 type` then 0 line (`v2 a`, `v2 b`); 1 arc (`v2 center`, `f64 radius`,
`f64 start`, `f64 end`, `u8 ccw`); 2 ellipse (`v2 center`, `v2 major`, `f64 ratio`,
`f64 startParam`, `f64 endParam`, `u8 ccw`); 3 spline (spline body); 4 polyline (`u8 closed`,
vertices).
*display item* (Proxy graphics): `u8 type` (0 polyline: `u8 closed`, points; 1 fill:
`varint rings` × points; 2 points: points), then style = RGBA 4 bytes, `f64 lineweight`,
`f64 patternScale`, `varint layer`. A dash pattern is not saved (it points into the linetype
table).
*points*: `varint n` × `v2`. *string*: `varint` index into `STRS`.

Entities are validated by `Document` while loading. An entity the document refuses (for example
a negative radius or a coordinate beyond ±1e10 mm) is **skipped with a warning** and the rest of
the file loads; its id stays unused.

## ORDR

Present only when model-space draw order (back to front) differs from id order:
`varint n` + `n` × `varint entityId`, a permutation of the model-space entities. On load the
longest ascending run is left alone, entities before it are sent to the back and entities after
it are brought to the front, which reproduces the stored order exactly. A list that is not a
permutation is ignored with a warning.

## Reading safely

* Every length, count and index is checked against the bytes that remain; an element count that
  cannot fit is an error before anything is allocated.
* Deflated chunks are bounded as described above and must inflate to exactly `rawLength`.
* Ids are reproduced by adding and erasing placeholders in the document; a gap of more than
  2^24 ids is treated as corruption.
* Any failure returns an `Error`; nothing crashes or throws out of `readTbcad`. The test suite
  truncates a file at every byte offset and applies thousands of random mutations, under
  AddressSanitizer and UBSan in CI.

## Writing safely

`writeTbcad(doc, path)`:

1. serialise to memory;
2. write `<path>.tmp`, flush and sync it (`fsync` / `FlushFileBuffers`);
3. keep the old file as `<path>.bak` (`keepBackup`, default on);
4. replace `<path>` with the temporary file (`rename` on POSIX, `ReplaceFileW` on Windows).

If the process dies before step 4 the original file is untouched and still opens; a stale
`.tmp` is overwritten by the next save. The output is deterministic: the same document and
options give the same bytes, and `write(read(write(d)))` equals `write(d)`.

## Not stored

Undo history, selection and view state, the effective layer cache, and the linetype pointer
inside proxy display styles.
