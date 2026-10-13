# Parametric constraints (Q54): constraint store in the document header and our own solver.
# See docs/constraints.md.
tbcad_add_library(constraints
  SOURCES
    src/store.cpp
    src/solver.cpp
  DEPENDS tbcad::core)

tbcad_add_tests(constraints
  SOURCES
    tests/main.cpp
    tests/store_tests.cpp
    tests/solver_tests.cpp)
