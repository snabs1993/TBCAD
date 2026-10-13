tbcad_add_library(draw2
  SOURCES
    src/boundary.cpp
    src/center.cpp
    src/common.cpp
    src/donut.cpp
    src/draw2.cpp
    src/point.cpp
    src/revcloud.cpp
  DEPENDS tbcad::cmd tbcad::core tbcad::geom tbcad::hatch)

tbcad_add_tests(draw2
  SOURCES
    tests/main.cpp
    tests/boundary_tests.cpp
    tests/center_tests.cpp
    tests/donut_tests.cpp
    tests/point_tests.cpp
    tests/revcloud_tests.cpp
    tests/test_util.h
  DEPENDS tbcad::cmd)
