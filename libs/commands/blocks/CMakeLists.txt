# Commands for blocks: BLOCK, INSERT, EXPLODE, ARRAY, plus the block list panel. The folder is
# libs/commands/blocks but the target is tbcad_block_cmds: libs/blocks already owns tbcad_blocks.
set(_block_cmd_sources
  src/array_command.cpp
  src/block_command.cpp
  src/block_list.cpp
  src/block_panel.cpp
  src/commands.cpp
  src/explode_command.cpp
  src/insert_command.cpp)

tbcad_add_library(block_cmds
  SOURCES ${_block_cmd_sources}
  DEPENDS tbcad::cmd tbcad::blocks tbcad::geom tbcad::ui)

# The panel draws with Dear ImGui, which only the application build fetches.
if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_link_libraries(tbcad_block_cmds PUBLIC tbcad::imgui)
  target_compile_definitions(tbcad_block_cmds PRIVATE TBCAD_BLOCK_PANEL_IMGUI=1)
endif()

tbcad_add_tests(block_cmds
  SOURCES
    tests/main.cpp
    tests/block_command_tests.cpp
    tests/insert_command_tests.cpp
    tests/array_command_tests.cpp
    tests/explode_command_tests.cpp
  DEPENDS tbcad_command_registry)
