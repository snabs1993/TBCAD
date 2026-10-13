# Levels (floors): create levels, work on one at a time, same XY on different levels
# (build/levels-brief.md). Headless operations, LEVEL / -LEVEL / CHLEVEL and the level panel.

set(_levels_sources src/level_ops.cpp src/level_commands.cpp src/register.cpp)
set(_levels_deps tbcad::cmd tbcad::ui tbcad::core)

# The panel draws with Dear ImGui, which only exists when the application is built.
if(TBCAD_BUILD_APP)
  include(deps/imgui)
  list(APPEND _levels_sources ui/levels_panel.cpp)
  list(APPEND _levels_deps tbcad::imgui)
else()
  list(APPEND _levels_sources src/panels_stub.cpp)
endif()

tbcad_add_library(levels SOURCES ${_levels_sources} DEPENDS ${_levels_deps})
target_include_directories(tbcad_levels PRIVATE src)
if(TBCAD_BUILD_APP)
  target_include_directories(tbcad_levels PRIVATE ui)
endif()

tbcad_add_tests(levels
  SOURCES
    tests/main.cpp
    tests/level_tests.cpp
  DEPENDS tbcad_levels tbcad_command_registry)
