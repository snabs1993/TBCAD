#!/usr/bin/env bash
# One-time setup so a Linux/cloud session can build the full tbcad window and run the GUI tests
# headless. Idempotent; installs only what is missing. Usage:
#   tools/cloud-setup.sh            install packages and configure build/linux-clang-debug
#   then: cmake --build build/linux-clang-debug -j4
#         TBCAD_REQUIRE_GL=1 TBCAD_REQUIRE_GUI=1 xvfb-run -a -s "-screen 0 1600x1000x24" \
#           ctest --test-dir build/linux-clang-debug -j4 --output-on-failure
# Without X11 headers GLFW fails at configure ("RandR headers not found") and the app is not built,
# so gui.cpp and the ImGui panels cannot be compiled.
set -e
pkgs="ninja-build clang xvfb libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
  libgl-dev libxkbcommon-dev libegl1 libegl-mesa0 libgl1-mesa-dri"
missing=""
for p in $pkgs; do dpkg -s "$p" >/dev/null 2>&1 || missing="$missing $p"; done
if [ -n "$missing" ]; then
  sudo=""; [ "$(id -u)" -eq 0 ] || sudo=sudo
  $sudo apt-get update -qq && $sudo apt-get install -y -qq $missing
fi
cd "$(dirname "$0")/.."
cmake --preset linux-clang-debug -DTBCAD_BUILD_APP=ON -Wno-dev >/dev/null
echo "ready: cmake --build build/linux-clang-debug -j4  (app + GUI tests included)"
