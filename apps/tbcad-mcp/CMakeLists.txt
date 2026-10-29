# tbcad-mcp: MCP over stdio. Bridge to the running TBCAD, or --headless on a .tbcad file.
file(GLOB_RECURSE _mcpcli_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER _mcpcli_sources EXCLUDE REGEX "/src/main\\.cpp$")

tbcad_add_library(mcpcli
  SOURCES ${_mcpcli_sources}
  DEPENDS tbcad::mcp tbcad::platform tbcad::template tbcad_command_registry)

add_executable(tbcad-mcp src/main.cpp)
target_link_libraries(tbcad-mcp PRIVATE tbcad::mcpcli)
target_compile_definitions(tbcad-mcp PRIVATE TBCAD_VERSION="${TBCAD_VERSION_LABEL}")
tbcad_set_warnings(tbcad-mcp)

file(GLOB_RECURSE _mcpcli_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
tbcad_add_tests(mcpcli SOURCES ${_mcpcli_tests})
if(TBCAD_BUILD_TESTS)
  add_dependencies(tbcad_mcpcli_tests tbcad-mcp)
  target_compile_definitions(tbcad_mcpcli_tests PRIVATE TBCAD_MCP_EXE="$<TARGET_FILE:tbcad-mcp>")
endif()
