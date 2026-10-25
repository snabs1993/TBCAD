tbcad_add_library(lispcmd
  SOURCES src/lispcmd.cpp
  DEPENDS tbcad::cmd tbcad::core tbcad::geom tbcad::lisp tbcad::sysvars)

tbcad_add_tests(lispcmd
  SOURCES
    tests/main.cpp
    tests/lispcmd_tests.cpp
  DEPENDS tbcad_lispcmd)

find_package(Threads REQUIRED)
target_link_libraries(tbcad_lispcmd PUBLIC Threads::Threads)
