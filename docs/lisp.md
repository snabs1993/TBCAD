# AutoLISP programs

TBCAD runs a subset of AutoLISP, so routines written for other CAD programs can be reused.

- `LISP` evaluates one expression. Typing an expression in parentheses on the command line does the same: `(+ 1 2)`.
- `APPLOAD` loads a `.lsp` file. Add `,S` after the path to put the file in the startup suite.
- `acad.lsp` in the settings folder, and every file listed in `startup-suite.txt` there, load when the program starts.
- `(defun c:NAME () ...)` makes `NAME` a command you can type, with its own undo step.

## What works

- Language: `defun`, `lambda`, `setq`, `if`, `cond`, `progn`, `while`, `repeat`, `foreach`, `and`, `or`, `quote`, local variables after `/`, dynamic scope, `load`, `*error*`, `vl-catch-all-apply`.
- Lists and strings: `car`/`cdr` and the `cXXr` forms, `cons`, `list`, `append`, `reverse`, `nth`, `assoc`, `subst`, `member`, `mapcar`, `apply`, `strcat`, `substr`, `strlen`, `strcase`, `itoa`, `atoi`, `atof`, `rtos`, `wcmatch`.
- Numbers: `+ - * / 1+ 1- rem abs min max sqrt sin cos atan exp log expt fix float`, `distance`, `angle`, `polar`.
- Drawing: `entget`, `entmod`, `entmake`, `entdel`, `entlast`, `entnext`, `ssget` (interactive, `"X"` with a filter, `"P"`, `"W"` and `"C"` with two corners), `getvar`/`setvar` (the variables SETVAR lists; `setvar` is one undo step), `sslength`, `ssname`, `ssadd`, `ssdel`, `ssmemb`. Entities: LINE, CIRCLE, ARC, POINT, TEXT, LWPOLYLINE; other kinds can be read (type, layer, colour) but not made.
- Input: `getpoint`, `getdist`, `getangle`, `getreal`, `getint`, `getstring`, `getkword` with `initget`, `entsel`. Esc stops the program.
- `(command "LINE" "0,0" "10,0" "")` types its words on the command line after the program ends, so code that follows it does not see the result yet.

A program that runs too long, or recurses too deep, stops with an error.
