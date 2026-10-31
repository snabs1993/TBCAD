tbcad_add_library(hatch_commands
  SOURCES src/hatch_commands.cpp
  DEPENDS tbcad::cmd tbcad::hatch tbcad::geom tbcad::blocks)

# MSVC /O2 stops with C4737 ("unable to perform required tail call") on the long HATCH coroutine,
# as on INSERT (see libs/commands/blocks), so this one file is compiled without optimisation.
if(MSVC)
  set_source_files_properties(src/hatch_commands.cpp PROPERTIES COMPILE_OPTIONS "/Od")
endif()

if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_sources(tbcad_hatch_commands PRIVATE ui/hatch_ui.cpp)
  target_compile_definitions(tbcad_hatch_commands PRIVATE TBCAD_HATCH_UI=1)
  target_link_libraries(tbcad_hatch_commands PRIVATE tbcad::ui tbcad::imgui)
endif()

tbcad_add_tests(hatch_commands
  SOURCES
    tests/main.cpp
    tests/hatch_command_tests.cpp
  DEPENDS tbcad::cmd)
