include(deps/miniz)

set(_mcp_sources
  src/entity_json.cpp
  src/entity_json_doc.cpp
  src/headless.cpp
  src/http_client.cpp
  src/http_server.cpp
  src/json_rpc.cpp
  src/owner_queue.cpp
  src/png.cpp
  src/protocol.cpp
  src/schema.cpp
  src/stdio.cpp
  src/tools.cpp
  src/tools_commands.cpp
  src/tools_edit.cpp
  src/tools_files.cpp
  src/tools_layers.cpp
  src/tools_read.cpp)

tbcad_add_library(mcp
  SOURCES ${_mcp_sources}
  DEPENDS tbcad::core tbcad::cmd tbcad::io tbcad::geom tbcad::index tbcad::platform tbcad::net)
target_include_directories(tbcad_mcp PRIVATE src)
target_link_libraries(tbcad_mcp PRIVATE miniz)

tbcad_add_tests(mcp
  SOURCES
    tests/main.cpp
    tests/http_tests.cpp
    tests/rpc_tests.cpp
    tests/tools_tests.cpp)
if(TBCAD_BUILD_TESTS)
  target_include_directories(tbcad_mcp_tests PRIVATE src)
endif()
