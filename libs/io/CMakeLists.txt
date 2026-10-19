include(deps/miniz)

tbcad_add_library(io
  SOURCES
    src/chunks.cpp
    src/crc32.cpp
    src/delta.cpp
    src/entity_codec.cpp
    src/read.cpp
    src/save.cpp
    src/write.cpp
  DEPENDS tbcad::core tbcad::platform)
find_package(Threads REQUIRED)
target_link_libraries(tbcad_io PRIVATE miniz Threads::Threads)

# Prints a .tbcad file as readable text (used by tests and for debugging).
add_executable(tbcad-dump tools/tbcad-dump.cpp)
target_link_libraries(tbcad-dump PRIVATE tbcad::io)
tbcad_set_warnings(tbcad-dump)

# Optional libFuzzer build (clang only): cmake -DTBCAD_IO_FUZZ=ON
option(TBCAD_IO_FUZZ "Build the .tbcad reader fuzz target with libFuzzer + ASan/UBSan" OFF)
if(TBCAD_IO_FUZZ)
  set(_fuzz_flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  target_compile_options(tbcad_io PRIVATE ${_fuzz_flags})
  add_executable(tbcad_io_fuzz_tbcad tests/fuzz_tbcad.cpp)
  target_link_libraries(tbcad_io_fuzz_tbcad PRIVATE tbcad::io)
  target_compile_options(tbcad_io_fuzz_tbcad PRIVATE ${_fuzz_flags})
  target_link_options(tbcad_io_fuzz_tbcad PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

tbcad_add_tests(io
  SOURCES
    tests/main.cpp
    tests/helpers.cpp
    tests/roundtrip_tests.cpp
    tests/robustness_tests.cpp
    tests/save_tests.cpp
    tests/delta_tests.cpp
    tests/performance_tests.cpp
  DEPENDS miniz)
if(TBCAD_BUILD_TESTS)
  add_dependencies(tbcad_io_tests tbcad-dump)
  target_compile_definitions(tbcad_io_tests PRIVATE TBCAD_DUMP_EXE="$<TARGET_FILE:tbcad-dump>")
endif()
