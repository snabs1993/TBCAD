# DWG clean-room log

Rules: `/build/dwg-brief.md` §3 (project shared folder). One line per fact source. Every DWG pull request
adds its lines here. Nobody working on `libs/dwg` has opened LibreDWG, libdxfrw, LibreCAD DWG code, ODA
Drawings/Teigha, RealDWG or any other DWG library or its tests, and nobody has disassembled any program.

| Date (UTC) | Thread | Fact learned | Source |
|---|---|---|---|
| 2026-10-13 | Q25 | A DWG file starts with six ASCII characters `AC10xx` that name the version (AC1015 = R2000, AC1018 = R2004, AC1021 = R2007, AC1024 = R2010, AC1027 = R2013, AC1032 = R2018). The same strings are the `$ACADVER` values of DXF. | Autodesk public DXF Reference (`$ACADVER` table) |
| 2026-10-13 | Q25 | Design decision, not a format fact: the DWG reader produces DXF-equivalent records and feeds them to the existing DXF importer as in-memory DXF text, so no change to `libs/dxf` is needed. | own design |

## ODA specification terms

Pending: the ODA "Open Design Specification for .dwg files" PDF could not be downloaded in the cloud
sandbox (network policy). Its cover/legal page will be copied here verbatim before any format fact from it is
used. Until then no bit-level encoding or container layout is implemented.
