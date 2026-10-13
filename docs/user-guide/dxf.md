# Opening DXF files

TBCAD stores drawings in its own `.tbcad` format. DXF files are converted with `tbcad-convert`,
which is installed next to `tbcad.exe`.

    tbcad-convert drawing.dxf -o drawing.tbcad --report drawing-report.json

Options:

| Option | Meaning |
|--------|---------|
| `-o <file>` | Output file (default: input name with `.tbcad`) |
| `--report <file>` | Write the import report (JSON) |
| `--units <unit>` | Units to assume when the file has none |
| `--encoding <name>` | Text encoding for old DXF files |
| `--split-layers <sep>` | Turn layer names such as `S-FLOOR~edge` into sub-layers (separator `sep`) |
| `--no-dimension-graphics` | Do not draw imported dimensions |
| `-q` | Quiet: no warnings on the console |

Exit codes: `0` success, `1` converted with warnings, `2` failed (not a DXF file, unreadable input or bad options).

The report lists every entity kind that was converted, approximated or skipped, so you can see what
changed. With the installer's "Open with" entry, right-click a `.dxf` file and choose
**TBCAD (import DXF)**: it converts the file beside the original and opens the result.
