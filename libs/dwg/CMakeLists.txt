# DWG reader (clean-room, see docs/dwg-cleanroom-log.md). Decodes a .dwg file to DXF-equivalent
# records and hands them to the DXF importer.
file(GLOB_RECURSE _dwg_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

tbcad_add_library(dwg SOURCES ${_dwg_sources} DEPENDS tbcad::core tbcad::dxf)
target_include_directories(tbcad_dwg PRIVATE src)

file(GLOB_RECURSE _dwg_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
tbcad_add_tests(dwg SOURCES ${_dwg_tests})
if(TBCAD_BUILD_TESTS)
  target_compile_definitions(tbcad_dwg_tests PRIVATE
    TBCAD_TEST_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/corpus")
  target_include_directories(tbcad_dwg_tests PRIVATE src)
endif()
