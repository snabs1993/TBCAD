# TBCAD

TBCAD is a free, open-source 2D drafting program for Windows, written in C++20.
It aims to be a practical replacement for commercial 2D CAD software. It has
its own engine, its own `.tbcad` file format, and a separate converter
(`tbcad-convert`) for exchanging DXF files.

Anyone may use, modify and redistribute TBCAD, including in closed-source
products, under the [MIT License](LICENSE). You do not have to publish your own
code.

> Status: early development (version 0.0.1). Only the build system exists so
> far. There is nothing to run yet.

## Technology

- C++20, built with CMake and Ninja
- Windows: MSVC (Visual Studio 2022 or Build Tools)
- UI: Dear ImGui + GLFW + OpenGL
- Only permissively licensed dependencies (MIT, BSD, zlib, ISC, Apache-2.0, FTL, Boost)
- Default unit: millimetres. UI language: English.

## Building on Windows

Requirements: Git, CMake 3.28 or newer, Ninja, and Visual Studio 2022 or its
Build Tools (C++ workload).

The presets use the `cl` compiler, so run the commands from an
**x64 Native Tools Command Prompt for VS 2022**, or call `vcvars64.bat` first:

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
git clone https://github.com/snabs1993/TCAD.git
cd TCAD
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

(Adjust the `vcvars64.bat` path if you installed the full IDE. If you look for
Build Tools with `vswhere`, pass `-products *`, otherwise it does not list them.)

## Building on Linux

Libraries and tests also build on Linux (GCC 13 or Clang 17):

```sh
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
ctest --preset linux-gcc-release
```

Presets: `windows-msvc-debug`, `windows-msvc-release`, `linux-clang-debug`,
`linux-gcc-release`.

CMake options: `TBCAD_BUILD_APP` (default ON on Windows), `TBCAD_BUILD_TESTS`
(ON), `TBCAD_WARNINGS_AS_ERRORS` (OFF locally, ON in CI).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

TBCAD is released under the MIT License, see [LICENSE](LICENSE). Third-party
licences are collected in `THIRD_PARTY_NOTICES.txt` (generated from
`third_party/notices/`).

AutoCAD, Autodesk and DWG are trademarks of Autodesk, Inc. TBCAD is not
affiliated with or endorsed by Autodesk, Inc.
