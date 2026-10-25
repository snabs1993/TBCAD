# Underlays (Q47): vector PDF pages, and images (see docs/underlay.md). They are attached through
# libs/xref, so this library only produces Documents and parameters.
include(deps/miniz)
include(deps/stb)

tbcad_add_library(underlay
  SOURCES
    src/pdf_parse.cpp
    src/pdf_import.cpp
    src/image.cpp
    src/tiff.cpp
  DEPENDS tbcad::core)
target_link_libraries(tbcad_underlay PRIVATE miniz stb_image)

tbcad_add_tests(underlay
  SOURCES
    tests/main.cpp
    tests/pdf_tests.cpp
    tests/image_tests.cpp
    tests/tiff_tests.cpp
  DEPENDS miniz stb_image)

# Optional libFuzzer build (clang only): cmake -DTBCAD_FORMATS_FUZZ=ON
if(TBCAD_FORMATS_FUZZ)
  set(_fuzz_flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  target_compile_options(tbcad_underlay PRIVATE ${_fuzz_flags})
  add_executable(tbcad_underlay_fuzz_pdf tests/fuzz_pdf.cpp)
  target_link_libraries(tbcad_underlay_fuzz_pdf PRIVATE tbcad::underlay)
  target_compile_options(tbcad_underlay_fuzz_pdf PRIVATE ${_fuzz_flags})
  target_link_options(tbcad_underlay_fuzz_pdf PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
