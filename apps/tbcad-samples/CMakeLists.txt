# tbcad-samples: builds the demo drawings shipped in samples/ (a two-storey Thai house plan).
# `tbcad-samples <folder>` writes thai-house.tbcad, thai-house.dxf and thai-house-A3.pdf; the output is the same on
# every run.
tbcad_add_library(samples
  SOURCES src/house.cpp
  DEPENDS tbcad::core tbcad::dim tbcad::layout tbcad::dxf tbcad::io tbcad::export
    tbcad::hatch tbcad::blocks tbcad::text)

add_executable(tbcad-samples src/main.cpp)
target_link_libraries(tbcad-samples PRIVATE tbcad::samples)
tbcad_set_warnings(tbcad-samples)

tbcad_add_tests(samples
  SOURCES
    tests/main.cpp
    tests/house_tests.cpp
  DEPENDS tbcad::samples)
