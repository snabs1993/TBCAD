set(_layoutcmd_sources src/commands.cpp src/ui.cpp)
set(_layoutcmd_extra)
# The viewport panel draws with Dear ImGui, which only exists when the application is built.
if(TBCAD_BUILD_APP)
  include(deps/imgui)
  list(APPEND _layoutcmd_sources ui/viewport_panel.cpp)
  set(_layoutcmd_extra tbcad::imgui)
else()
  list(APPEND _layoutcmd_sources src/viewport_panel_stub.cpp)
endif()

tbcad_add_library(layoutcmd
  SOURCES ${_layoutcmd_sources}
  DEPENDS ${_layoutcmd_extra} tbcad::cmd tbcad::core tbcad::geom tbcad::layout tbcad::export tbcad::ui tbcad::render tbcad::clipboard tbcad::dxf tbcad::io)

tbcad_add_tests(layoutcmd
  SOURCES
    tests/main.cpp
    tests/layout_command_tests.cpp
  DEPENDS tbcad_layoutcmd)
