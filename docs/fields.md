# Fields

A field is a part of a text that is worked out each time the text is drawn or plotted, so it
stays right when the drawing changes. Put one in with `FIELD`, or type it into any text, mtext or
table cell. A field is written `%<name>%` or `%<name:argument:argument>%`.

| Field | Shows |
| --- | --- |
| `%<date>%` | today, `2026-10-13`; `%<date:dd/MM/BBBB>%` picks the form (`yyyy`, `BBBB` = Buddhist year, `MM`, `dd`, `HH`, `mm`) |
| `%<filename>%`, `%<filepath>%` | the drawing's file name, or with its folder |
| `%<sheet>%` | the sheet number (`FIELD` then `SetSheet`) |
| `%<var:NAME>%` | any setting kept in the drawing |
| `%<length:#ID:2:m>%` | length of a line, arc, circle, ellipse or polyline; 2 decimals; in metres (`m`, `cm`, `mm`, for drawings made in millimetres) |
| `%<perimeter:#ID:2:m>%` | the same as `length` (perimeter of a closed polyline or circle) |
| `%<area:#ID:2:m>%` | area of a circle, full ellipse, closed polyline or hatch (islands taken away), in square metres |
| `%<text:#ID>%` | the text of another text object |
| `%<radius:#ID:2>%`, `%<diameter:#ID:2>%` | of a circle or arc |
| `%<x:#ID:2>%`, `%<y:#ID:2>%` | centre or insertion point of an object |
| `%<layer:#ID>%` | the name of the object's layer |
| `%<block:#ID>%`, `%<attr:#ID:TAG>%` | the block name of an insert, and one of its attribute values |
| `%<scale>%` | the current annotation scale, `1:100` |

A field that cannot be worked out (no such object, unsaved drawing) shows `####`. On screen the
texts with fields are redrawn after every change, so a length or area follows its object live.
`FIELD` asks for the kind of field, the object when needed, and the place for the new text.
Fields also work in the text override of a dimension (`<>` still stands for the measurement).
Limits: the file name changes on the screen only after the next edit; DXF files carry the shown
text of the moment.
