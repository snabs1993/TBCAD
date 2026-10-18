# Drawing variables (SETVAR)

`libs/commands/sysvars` holds the commands that set what new objects get and the drawing settings:
COLOR, LWEIGHT, CETRANSPARENCY, LTSCALE, UNITS, STATUS, RENAME and **SETVAR**.

**SETVAR** (alias SET) takes a variable name and then its new value, or `?` to list them with their
current values. Every change is one undo step, and the command line shows the value afterwards.

| Variable | Meaning | Same as |
| --- | --- | --- |
| LTSCALE | linetype scale factor | LTSCALE |
| CECOLOR | colour of new objects (name, 1-255, r,g,b, ByLayer, ByBlock) | COLOR |
| CELWEIGHT | lineweight of new objects | LWEIGHT |
| CELTYPE | linetype of new objects | |
| CETRANSPARENCY | transparency of new objects | CETRANSPARENCY |
| CLAYER | current layer (by name) | |
| INSUNITS | length unit (Millimeters, Centimeters, Meters, Inches, Feet); objects are not scaled | UNITS |
