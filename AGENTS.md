# Guide for AI coding agents

TBCAD is a free 2D CAD program (C++20, MIT). Read CONTRIBUTING.md once; this file is the short version.

## Rules

- Do only the task you were given. Touch the fewest files you can. No drive-by refactors or renames.
- C++20, namespace `tbcad::<module>`. Geometry is `double`, millimetres, angles in radians.
- Code lives in `libs/<module>/{include/tbcad/<module>,src,tests}` and `apps/`. Match the style of the
  file you edit (names, comment density, includes). Format with `clang-format -i` on files you change.
- Warnings are errors, also on MSVC: no shadowed names, no silent 64-to-32-bit conversions
  (use `static_cast`), no unused variables.
- Every behaviour change needs a doctest test in the module's `tests/` folder.
- No new dependencies. Never add GPL/LGPL/AGPL/MPL code.
- Clean room: never copy Autodesk files, text, data or names. Never write "AutoCAD", "Autodesk",
  "DWG" or "TrustedDWG" in product names, file names, UI text or metadata.
- UI text is English. Default unit mm.
- Do not edit CMakeLists.txt at the repo root, `.github/`, `third_party/`, or this file.

## Build and test (Linux)

```sh
cmake --preset linux-clang-debug            # once
cmake --build build/linux-clang-debug -j8
ctest --test-dir build/linux-clang-debug --output-on-failure -j8
```

Build only the target you need while iterating (`--target tbcad_<module>_tests`), and run the
full build and tests before you finish. The task is done only when everything builds and every
test passes. Do not disable, skip or delete tests.

## When you finish

Leave the changes in the working tree (do not commit, push or open pull requests). Write a short
summary of what you changed and anything you could not do to `WORKER_REPORT.md` at the repo root.
