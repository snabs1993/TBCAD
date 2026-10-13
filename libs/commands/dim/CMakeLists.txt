tbcad_add_library(dim_commands
  SOURCES
    src/dim_commands.cpp
    src/dim_helpers.cpp
    src/smart_commands.cpp
    src/chain_commands.cpp
    src/dimstyle_command.cpp
  DEPENDS tbcad::cmd tbcad::dim tbcad::blocks tbcad::geom tbcad::text)

if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_sources(tbcad_dim_commands PRIVATE ui/dim_ui.cpp)
  target_link_libraries(tbcad_dim_commands PRIVATE tbcad::ui tbcad::imgui)
endif()

tbcad_add_tests(dim_commands
  SOURCES
    tests/main.cpp
    tests/dim_command_tests.cpp
    tests/dimstyle_command_tests.cpp
    tests/smart_command_tests.cpp
    tests/chain_command_tests.cpp
  DEPENDS tbcad::cmd)
