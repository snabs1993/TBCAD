tbcad_add_library(mline_commands
  SOURCES
    src/mline_commands.cpp
    src/mledit.cpp
  DEPENDS tbcad::cmd tbcad::mline tbcad::blocks tbcad::geom tbcad::ui)

if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_sources(tbcad_mline_commands PRIVATE ui/style_panel.cpp)
  target_compile_definitions(tbcad_mline_commands PRIVATE TBCAD_MLINECMD_WITH_IMGUI)
  target_link_libraries(tbcad_mline_commands PRIVATE tbcad::imgui)
endif()

tbcad_add_tests(mline_commands
  SOURCES
    tests/main.cpp
    tests/mline_command_tests.cpp
  DEPENDS tbcad::cmd)
