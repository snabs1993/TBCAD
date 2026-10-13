tbcad_add_library(draw2
  SOURCES
    src/boundary.cpp
    src/center.cpp
    src/common.cpp
    src/donut.cpp
    src/draw2.cpp
    src/point.cpp
    src/ray.cpp
    src/regionops.cpp
    src/revcloud.cpp
    src/sketch.cpp
    src/ui.cpp
    src/wipeout.cpp
  DEPENDS tbcad::ui tbcad::cmd tbcad::core tbcad::geom tbcad::hatch)

tbcad_add_tests(draw2
  SOURCES
    tests/main.cpp
    tests/boundary_tests.cpp
    tests/center_tests.cpp
    tests/donut_tests.cpp
    tests/point_tests.cpp
    tests/ray_tests.cpp
    tests/region_tests.cpp
    tests/revcloud_tests.cpp
    tests/sketch_tests.cpp
    tests/wipeout_tests.cpp
    tests/test_util.h
  DEPENDS tbcad::cmd)
