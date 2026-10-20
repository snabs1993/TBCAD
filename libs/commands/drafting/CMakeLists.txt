# W4-1 drafting aids, application side: the OSNAP / GRID / SNAP / ORTHO / POLAR commands, the F-key
# toggles, the point filters, the typed-input parser and the grid and snap-marker overlay. The
# logic itself is in libs/drafting.
tbcad_add_library(drafting_cmds
  SOURCES
    src/commands.cpp
    src/module_state.cpp
    src/ui.cpp
    src/ucs_command.cpp
  DEPENDS tbcad::drafting tbcad::cmd tbcad::ui tbcad::render tbcad::core)
target_include_directories(tbcad_drafting_cmds PRIVATE src)

if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_sources(tbcad_drafting_cmds PRIVATE ui/ucs_panel.cpp)
  target_compile_definitions(tbcad_drafting_cmds PRIVATE TBCAD_DRAFTINGCMDS_WITH_IMGUI)
  target_link_libraries(tbcad_drafting_cmds PRIVATE tbcad::imgui)
endif()

tbcad_add_tests(drafting_cmds
  SOURCES
    tests/main.cpp
    tests/command_tests.cpp
    tests/appcore_tests.cpp
  DEPENDS tbcad::appcore)
