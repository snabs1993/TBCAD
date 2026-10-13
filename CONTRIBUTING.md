# Contributing to TBCAD

Thank you for helping. TBCAD is MIT licensed, and every contribution is made under the same licence.

## Sign your commits (DCO)

We use the [Developer Certificate of Origin](https://developercertificate.org/).
Sign off every commit:

```sh
git commit -s -m "geom: add arc intersection"
```

This adds a `Signed-off-by: Your Name <you@example.com>` line, which states that
you wrote the change or have the right to submit it under the MIT licence.

## Licence rules

- Do not add GPL, LGPL, AGPL or MPL code, or code that depends on them. This includes Eigen and Cairo.
- Allowed licences: MIT, BSD, zlib, ISC, Apache-2.0, FTL, Boost. Bundled fonts may be OFL.
- Do not copy files, text, names or data from Autodesk products (for example `acad.lin`, `acad.pat`, SHX fonts, icons, colour tables).
- Do not use "AutoCAD", "Autodesk", "DWG" or "TrustedDWG" in product names, file names or metadata.

## Code conventions

- C++20, namespace `tbcad::<module>`. All geometry in `double`, millimetres. Angles in radians.
- Warnings are errors in CI (`/W4 /permissive-` on MSVC, `-Wall -Wextra -Wpedantic` elsewhere).
- Format with `clang-format` (config in `.clang-format`). CI checks it: `clang-format --dry-run --Werror <files>`.
- Libraries under `libs/` must build and pass tests on Linux and MSVC.

## Module layout and ownership

```
libs/<module>/CMakeLists.txt
libs/<module>/include/tbcad/<module>/*.h   public API
libs/<module>/src/*.cpp
libs/<module>/tests/*.cpp                  doctest, target tbcad_<module>_tests
```

Modules are discovered automatically by the root `CMakeLists.txt`; do not edit it
to add one. Each module has one owner. Change only the paths you own. If you
need a change in another module, describe it in your pull request (what and why,
with a proposed patch) instead of editing it.

Use `tbcad_add_library(<name> ...)` and `tbcad_add_tests(<name> ...)` from
`cmake/TbcadModule.cmake`.

## Adding a dependency

1. Add `cmake/deps/<name>.cmake`, which pulls the dependency with `FetchContent`, pinned to an exact tag or commit and `URL_HASH`.
2. Add `third_party/notices/<name>.txt` with the full licence text.

CI fails if a deps file has no matching notice file.

## Pull requests

One branch and one pull request per piece of work. List what was built, what was left out, and any requests to other module owners. CI must be green on Windows and Linux.
