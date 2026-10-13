# Underlays (Q47): vector PDF pages, and images (see docs/underlay.md). They are attached through
# libs/xref, so this library only produces Documents and parameters.
include(deps/miniz)
include(deps/stb)

tbcad_add_library(underlay
  SOURCES
    src/pdf_parse.cpp
    src/pdf_import.cpp
    src/image.cpp
  DEPENDS tbcad::core)
target_link_libraries(tbcad_underlay PRIVATE miniz stb_image)

tbcad_add_tests(underlay
  SOURCES
    tests/main.cpp
    tests/pdf_tests.cpp
    tests/image_tests.cpp
  DEPENDS miniz stb_image)
