tbcad_add_library(modify
  SOURCES
    src/break_join.cpp
    src/common.cpp
    src/extra.cpp
    src/fillet.cpp
    src/offset.cpp
    src/trim.cpp
  DEPENDS tbcad::core tbcad::geom tbcad::text)
tbcad_add_tests(modify
  SOURCES
    tests/main.cpp
    tests/accuracy_tests.cpp
    tests/break_join_tests.cpp
    tests/extra_tests.cpp
    tests/pedit_scale_tests.cpp
    tests/far_coords_tests.cpp
    tests/fuzz_tests.cpp
    tests/fillet_tests.cpp
    tests/offset_tests.cpp
    tests/semantic_tests.cpp
    tests/stress_tests.cpp
    tests/test_support.h
    tests/trim_tests.cpp tests/property_tests.cpp)
