tbcad_add_library(table_commands
  SOURCES
    src/table_commands.cpp
    src/csv_commands.cpp
  DEPENDS tbcad::cmd tbcad::table tbcad::blocks tbcad::geom tbcad::text tbcad::ui)

if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_sources(tbcad_table_commands PRIVATE ui/style_panel.cpp)
  target_compile_definitions(tbcad_table_commands PRIVATE TBCAD_TABLECMD_WITH_IMGUI)
  target_link_libraries(tbcad_table_commands PRIVATE tbcad::imgui)
endif()

tbcad_add_tests(table_commands
  SOURCES
    tests/main.cpp
    tests/table_command_tests.cpp
  DEPENDS tbcad::cmd)
