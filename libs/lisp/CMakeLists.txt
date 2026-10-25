# AutoLISP-style interpreter. Knows nothing about drawings; the lisp command module binds it to one.
tbcad_add_library(lisp
  SOURCES
    src/lisp.cpp)
tbcad_add_tests(lisp
  SOURCES
    tests/main.cpp
    tests/lisp_tests.cpp
  DEPENDS tbcad_lisp)
