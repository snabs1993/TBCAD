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
| `%<area:#ID:2:m>%` | area of a circle or closed polyline, in square metres |
| `%<text:#ID>%` | the text of another text object |

A field that cannot be worked out (no such object, unsaved drawing) shows `####`. On screen the
texts with fields are redrawn after every change, so a length or area follows its object live.
`FIELD` asks for the kind of field, the object when needed, and the place for the new text.
Fields also work in the text override of a dimension (`<>` still stands for the measurement).
Limits: the file name changes on the screen only after the next edit; DXF files carry the shown
text of the moment.
