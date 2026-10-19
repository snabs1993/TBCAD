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
  src/tools_blocks.cpp
  src/tools_constraints.cpp
  src/tools_layers.cpp
  src/tools_assembly.cpp
  src/tools_layout.cpp
  src/tools_modify.cpp
  src/tools_levels.cpp
  src/tools_ops.cpp
  src/tools_read.cpp)

tbcad_add_library(mcp
  SOURCES ${_mcp_sources}
  DEPENDS tbcad::core tbcad::cmd tbcad::io tbcad::geom tbcad::blocks tbcad::expr tbcad::dim tbcad::layout tbcad::constraints tbcad::modify tbcad::index tbcad::platform tbcad::net tbcad::hatch)
target_include_directories(tbcad_mcp PRIVATE src)
target_link_libraries(tbcad_mcp PRIVATE miniz)

# Optional libFuzzer build (clang only): cmake -DTBCAD_MCP_FUZZ=ON
option(TBCAD_MCP_FUZZ "Build the MCP server fuzz target with libFuzzer + ASan/UBSan" OFF)
if(TBCAD_MCP_FUZZ)
  set(_fuzz_flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  target_compile_options(tbcad_mcp PRIVATE ${_fuzz_flags})
  add_executable(tbcad_mcp_fuzz tests/fuzz_mcp.cpp)
  target_link_libraries(tbcad_mcp_fuzz PRIVATE tbcad::mcp)
  target_compile_options(tbcad_mcp_fuzz PRIVATE ${_fuzz_flags})
  target_link_options(tbcad_mcp_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

tbcad_add_tests(mcp
  SOURCES
    tests/main.cpp
    tests/http_tests.cpp
    tests/rpc_tests.cpp
    tests/tools_tests.cpp
    tests/modify_tests.cpp
    tests/entity_json_tests.cpp)
if(TBCAD_BUILD_TESTS)
  target_include_directories(tbcad_mcp_tests PRIVATE src)
endif()
