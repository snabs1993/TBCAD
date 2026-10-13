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
