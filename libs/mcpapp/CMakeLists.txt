# The MCP server inside the TBCAD program: an AppCore-backed DrawingSession, the controller that
# owns the HTTP server and the permission state, the MCP command and its panel. Dear ImGui is only
# used by the panel and the confirm dialog, and only in the application build.
tbcad_add_library(mcpapp
  SOURCES
    src/app_session.cpp
    src/commands.cpp
    src/controller.cpp
    src/ui.cpp
  DEPENDS tbcad::mcp tbcad::appcore tbcad::ui tbcad::cmd tbcad::platform tbcad::core)
target_include_directories(tbcad_mcpapp PRIVATE src)

if(TBCAD_BUILD_APP)
  include(deps/imgui)
  target_link_libraries(tbcad_mcpapp PUBLIC tbcad::imgui)
  target_compile_definitions(tbcad_mcpapp PRIVATE TBCAD_MCPAPP_IMGUI=1)
endif()

tbcad_add_tests(mcpapp
  SOURCES
    tests/main.cpp
    tests/mcpapp_tests.cpp
  DEPENDS tbcad_command_registry)
