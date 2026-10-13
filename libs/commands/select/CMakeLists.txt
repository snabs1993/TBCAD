tbcad_add_library(select
  SOURCES
    src/isolate.cpp
    src/props.cpp
    src/purge.cpp
    src/qselect.cpp
    src/select.cpp
    src/similar.cpp
  DEPENDS tbcad::cmd tbcad::core)

tbcad_add_tests(select
  SOURCES
    tests/main.cpp
    tests/isolate_tests.cpp
    tests/purge_tests.cpp
    tests/qselect_tests.cpp
    tests/similar_tests.cpp
    tests/test_util.h
  DEPENDS tbcad::cmd)
