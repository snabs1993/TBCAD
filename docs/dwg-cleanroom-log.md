# DWG clean-room log

Rules: `/build/dwg-brief.md` §3 (project shared folder). One line per fact source. Every DWG pull request
adds its lines here. Nobody working on `libs/dwg` has opened LibreDWG, libdxfrw, LibreCAD DWG code, ODA
Drawings/Teigha, RealDWG or any other DWG library or its tests, and nobody has disassembled any program.

| Date (UTC) | Thread | Fact learned | Source |
|---|---|---|---|
| 2026-10-13 | Q25 | A DWG file starts with six ASCII characters `AC10xx` that name the version (AC1015 = R2000, AC1018 = R2004, AC1021 = R2007, AC1024 = R2010, AC1027 = R2013, AC1032 = R2018). The same strings are the `$ACADVER` values of DXF. | Autodesk public DXF Reference (`$ACADVER` table) |
| 2026-10-13 | Q25 | Design decision, not a format fact: the DWG reader produces DXF-equivalent records and feeds them to the existing DXF importer as in-memory DXF text, so no change to `libs/dxf` is needed. | own design |
| 2026-10-13 | Q25 | Bit-level data types (B, BB, 3B, BS, BL, BLL, BD, DD, BT, MC, MS, RC/RS/RL/RD, H, CMC, text) and their prefix codes; the R2010 object type encoding. | ODA spec, bit codes and common data types chapters |
| 2026-10-13 | Q25 | Handle references: a code nibble plus a counted absolute value; codes 2-5 absolute, 6 = reference+1, 8 = reference-1, 0xA = reference+offset, 0xC = reference-offset. | ODA spec, handle references |
| 2026-10-13 | Q25 | CRC16 and CRC32 parameters (tables are generated at start-up, not copied). | ODA spec, CRC sections |
| 2026-10-13 | Q25 | R2000 file header: version string, section locators at 0x15 / 0x19, nine bytes each (record number, seeker, size); records 0 header variables, 1 classes, 2 object map. | ODA spec, R13-R15 file structure |
| 2026-10-13 | Q25 | Object map layout: big-endian section size that includes itself, MC handle deltas and location deltas, CRC after each section. | ODA spec, object map |
| 2026-10-13 | Q25 | Object layout: MS size excluding the size field and CRC, then a bit stream (type, object bits, handle, EED, data); the handle stream starts at the object-bits position. Checked against the worked example in the spec. | ODA spec, object layout and R14 example |
| 2026-10-13 | Q25 | Entity common data and handle-stream order (owner, reactors, xdictionary, prev/next, layer, linetype, plotstyle, then type-specific handles). | ODA spec, common entity data |
| 2026-10-13 | Q25 | Record layouts of LAYER, LTYPE, STYLE, BLOCK_HEADER and entities LINE, CIRCLE, ARC, POINT, TEXT, INSERT, LWPOLYLINE (R2000). | ODA spec, per-object sections |
| 2026-10-13 | Q25 | Header variable order up to $INSUNITS (read only under a plausibility check). | ODA spec, header variables |
| 2026-10-13 | Q25 | Assumed, not confirmed by any file: LWPOLYLINE closed flag 512; layer off = negative colour; lineweight index table; DXF code page for output is written as ANSI_1252. To be checked against real files. | general knowledge; Autodesk public DXF Reference |
| 2026-10-13 | Q26 | Record layouts of ELLIPSE, SPLINE (both scenarios), SOLID, TRACE, MTEXT, 2D POLYLINE with VERTEX, ATTRIB, ATTDEF, MINSERT (R2000). | ODA spec, per-object sections 20.4.4, 20.4.5, 20.4.10, 20.4.11, 20.4.16, 20.4.35, 20.4.36, 20.4.39, 20.4.40, 20.4.46 |
| 2026-10-13 | Q26 | HATCH layout (boundary paths of polyline and edge kind, pattern lines, seed points) and that it is a class-based object (type number from 500 up, DXF name in the class table). | ODA spec, 20.4.75; class table (section 2) |
| 2026-10-13 | Q26 | Common dimension data and the seven DIMENSION layouts; DIMSTYLE variable order; LEADER layout. | ODA spec, 20.4.22 to 20.4.29, 20.4.47, 20.4.68 |
| 2026-10-13 | Q26 | Assumed, not confirmed by any file: hatch and pattern angles are radians; DIMSTYLE group 70 bits; dimension type flag composition (flags 1 bit 0 = opposite of 128, bit 1 = 32); polyline and vertex flag bits equal the DXF ones. | general knowledge; Autodesk public DXF Reference |
| 2026-10-13 | Q27 | R2004 file header: 0x6C bytes at 0x80 XORed with a fixed byte sequence from a linear congruential generator; file ID string; page map id and address, section map id. | ODA spec section 4 (R2004 file structure) |
| 2026-10-13 | Q27 | System pages (page map, section map): 0x14-byte header (type, sizes, compression type, checksum) and LZ77 data; page map entries (number, size) with a gap record carrying extra fields; page addresses are cumulative from 0x100. | ODA spec section 4 |
| 2026-10-13 | Q27 | Section map description records and per-page records; data page header 32 bytes, masked by an XOR with a value built from the page address. | ODA spec section 4 |
| 2026-10-13 | Q27 | LZ77 variant used by R2004+ (literal run lengths, the two-byte and long offset opcodes, the end marker). | ODA spec section 4.7 |
| 2026-10-13 | Q27 | Section contents: header variables, class table, handle map (same layout as R2000 object map) with offsets into the objects section. | ODA spec sections 4, 5, 9 |
| 2026-10-13 | Q27 | Object differences for R2004 (colour with colour-book handle, extension-dictionary-missing bit, owned-object lists, hatch gradient prefix), R2007+ (string stream, material and shadow flags, UTF-16), R2010+ (handle-stream size at the start, new type encoding, vertex ids, version bytes), R2013+ (data-store bit, spline flags), R2018+ (attribute kind). | ODA spec per-object sections and the version tables |
| 2026-10-13 | Q27 | Assumed, not confirmed by any file: LZ77 distance bias (tries +1 then +0); where the string stream starts (flag at the last data bit, 16-bit size before it); true-colour value is a 32-bit word; layer off only from a negative colour index; header variable string layout for R2007+; polyline owned-object counts always present; code page mapping for non-Unicode output. To be checked against real files. | general knowledge |
| 2026-10-14 | Q29 | Facts confirmed by byte analysis of permitted real R2018 files (no outside code): R2018 table records carry no xref index; objects have no "no links" bit; INSERT stores an owned-object count only when attributes follow; xref block headers store no owned count; a block's member list is in its handle stream; entity mode 2 means model space (no owner stored), 0 an owner handle, 1 paper space. | byte analysis of owner-supplied files (not copied anywhere) |
| 2026-10-14 | Q29 | LZ77 opcodes 0x18 to 0x1F: length from the low three bits plus 2, and bit 3 adds 0x4000 to the distance; the terminator 0x11 ends the data exactly at the expected size (decides the distance bias 1). | byte analysis of a real compressed section map, ODA spec section 4.7 |
| 2026-10-14 | Q29 | Class section of R2018: after the size word come a high size word and a bit-count word, then the maximum class number, two zero bytes and a true bit, then all class fields (number, proxy flags, zombie bit, item class id, instance count, versions, two zeros) for every class, then one run of UTF-16 strings (application, C++ name, DXF name per class) that starts right after the last field. The class count is maximum number - 499. | byte analysis of a real file's class section; ODA spec section 10.2 |
| 2026-10-14 | Q29 | LAYOUT is the fixed object type 82 (its class entry is a separate, unused number); layouts are named by the ACAD_LAYOUT dictionary (DICTIONARY type 42: count, cloning flag, owner flag; names in the string stream; item handles in the handle stream). The tab order is stored in the layout object. | byte analysis, ODA spec sections 20.4.44, 20.4.84 |
| 2026-10-14 | Q29 | VIEWPORT (type 34) fields as listed in the spec decode correctly on real files; the status flag bit for "off" did not match any real viewport, so viewports are treated as on; a sheet's own overall viewport has target 0,0 and a view centre equal to its centre. | ODA spec section 20.4.38; byte analysis |

## ODA specification terms

Source file: "Open Design Specification for .dwg files", version 5.4.1, supplied by the owner as
`/mnt/project-files/research/dwg/ODA-dwg-spec.pdf` (2026-10-13). Provenance: the owner asked for the public
guest-download link (`opendesign.com/files/guestdownloads/...`), downloaded the PDF from it and confirmed
"public link" when asked. The PDF's document properties say "CONFIDENTIAL:" / "Preferred Customer"; that is
metadata of the published file, noted here for completeness. The legal page below has no licence terms and no
restriction on implementing from the text ("furnished for informational use only"). Cover and legal page,
copied verbatim (line breaks as in the PDF):

```text
Open Design Specification for .dwg files
             Version 5.4.1




         Open Design Alliance


        www.opendesign.com
Copyright © 1998-2026 Open Design Alliance, Inc. All rights reserved.

Information in these materials is furnished for informational use only, is subject to change without notice and does not represent
a commitment on the part of Open Design Alliance. Open Design Alliance assumes no responsibility or liability for any errors or
inaccuracies that may appear in these materials. Use these materials at your own risk.

TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, OPEN DESIGN ALLIANCE AND ITS SUPPLIERS
DISCLAIM ANY AND ALL WARRANTIES AND CONDITIONS, EITHER EXPRESS OR IMPLIED, INCLUDING,
WITHOUT LIMITATION, IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE, TITLE, AND NON-INFRINGEMENT, AND THOSE ARISING OUT OF USAGE OF TRADE OR COURSE OF
DEALING, CONCERNING THESE MATERIALS. THESE MATERIALS ARE PROVIDED “AS IS” WITHOUT
WARRANTY OF ANY KIND.

TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, IN NO EVENT SHALL OPEN DESIGN ALLIANCE
OR ITS SUPPLIERS (OR THEIR RESPECTIVE AGENTS, DIRECTORS, EMPLOYEES OR REPRESENTATIVES) BE
LIABLE FOR ANY DAMAGES WHATSOEVER (INCLUDING, WITHOUT LIMITATION, CONSEQUENTIAL,
INCIDENTAL, DIRECT, INDIRECT, SPECIAL, ECONOMIC, PUNITIVE OR SIMILAR DAMAGES, OR DAMAGES FOR
LOSS OF BUSINESS PROFITS, LOSS OF GOODWILL, BUSINESS INTERRUPTION, COMPUTER FAILURE OR
MALFUNCTION, LOSS OF BUSINESS INFORMATION OR ANY AND ALL OTHER COMMERCIAL OR PECUNIARY
DAMAGES OR LOSSES) ARISING OUT OF THE USE OF THESE MATERIALS, HOWEVER CAUSED AND ON ANY
LEGAL THEORY OF LIABILITY (WHETHER IN TORT, CONTRACT OR OTHERWISE), EVEN IF OPEN DESIGN
ALLIANCE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES, OR FOR ANY CLAIM BY ANY OTHER
PARTY. Because some jurisdictions do not allow the exclusion or limitation of liability for consequential or incidental damages,
the above limitation may not apply to you.

Teigha® is a trademark of Open Design Alliance in the United States and/or other countries. All other trademarks, trade names
or company names referenced herein are used for identification only and are the property of their respective owners.

US Government Restricted Rights: These materials are provided with RESTRICTED RIGHTS. Use, duplication or disclosure by
the Government is subject to restrictions as set forth in subparagraph (c)(1)(ii) of The Rights in Technical Data and Computer
Software clause at DFARS 252.227-7013 or subparagraphs (c)(1) and (2) of the Commercial Computer Software-Restricted
Rights at 48 CFR 52.227-19, as applicable. The contractor/manufacturer is Open Design Alliance, 5025 N Central Ave #602,
Phoenix, AZ, USA.

Printed in USA.




* DWG is the native and proprietary file format for AutoCAD® and a trademark of Autodesk, Inc. The Open Design Alliance is
not associated with Autodesk.
```

Implementation rule kept: no text, tables or code from the specification are copied into the repository; code
and comments are written in our own words and cite "ODA spec section N".
