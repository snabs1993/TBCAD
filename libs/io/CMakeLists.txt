include(deps/miniz)

tbcad_add_library(io
  SOURCES
    src/chunks.cpp
    src/crc32.cpp
    src/entity_codec.cpp
    src/read.cpp
    src/save.cpp
    src/write.cpp
  DEPENDS tbcad::core)
target_link_libraries(tbcad_io PRIVATE miniz)

# Prints a .tbcad file as readable text (used by tests and for debugging).
add_executable(tbcad-dump tools/tbcad-dump.cpp)
target_link_libraries(tbcad-dump PRIVATE tbcad::io)
tbcad_set_warnings(tbcad-dump)

tbcad_add_tests(io
  SOURCES
    tests/main.cpp
    tests/helpers.cpp
    tests/roundtrip_tests.cpp
    tests/robustness_tests.cpp
    tests/save_tests.cpp
    tests/performance_tests.cpp
  DEPENDS miniz)
if(TBCAD_BUILD_TESTS)
  add_dependencies(tbcad_io_tests tbcad-dump)
  target_compile_definitions(tbcad_io_tests PRIVATE TBCAD_DUMP_EXE="$<TARGET_FILE:tbcad-dump>")
endif()
