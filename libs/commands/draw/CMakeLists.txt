tbcad_add_library(draw
  SOURCES
    src/arc.cpp
    src/circle.cpp
    src/common.cpp
    src/draw.cpp
    src/ellipse.cpp
    src/line.cpp
    src/polygon.cpp
    src/polyline.cpp
    src/rectangle.cpp
    src/spline.cpp
  DEPENDS tbcad::cmd tbcad::core tbcad::geom)

tbcad_add_tests(draw
  SOURCES
    tests/main.cpp
    tests/arc_tests.cpp
    tests/circle_tests.cpp
    tests/ellipse_tests.cpp
    tests/line_tests.cpp
    tests/polygon_tests.cpp
    tests/polyline_tests.cpp
    tests/rectangle_tests.cpp
    tests/spline_tests.cpp
    tests/test_util.h
  DEPENDS tbcad::cmd)
