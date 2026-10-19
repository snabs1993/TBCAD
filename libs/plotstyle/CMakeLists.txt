# Plot style tables (Q80): map the colour of an object to the colour, lineweight and screening it
# is plotted with. Our own text format, built-in tables, and import of .ctb files.
include(deps/miniz)

tbcad_add_library(plotstyle
  SOURCES src/plotstyle.cpp src/ctb.cpp
  DEPENDS tbcad::core)
target_link_libraries(tbcad_plotstyle PRIVATE miniz)

tbcad_add_tests(plotstyle
  SOURCES
    tests/main.cpp
    tests/plotstyle_tests.cpp
  DEPENDS miniz)

# Optional libFuzzer build (clang only): cmake -DTBCAD_FORMATS_FUZZ=ON
if(TBCAD_FORMATS_FUZZ)
  set(_fuzz_flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  target_compile_options(tbcad_plotstyle PRIVATE ${_fuzz_flags})
  add_executable(tbcad_plotstyle_fuzz_ctb tests/fuzz_ctb.cpp)
  target_link_libraries(tbcad_plotstyle_fuzz_ctb PRIVATE tbcad::plotstyle)
  target_compile_options(tbcad_plotstyle_fuzz_ctb PRIVATE ${_fuzz_flags})
  target_link_options(tbcad_plotstyle_fuzz_ctb PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
