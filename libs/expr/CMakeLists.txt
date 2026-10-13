# Small formula language for parametric assemblies: numbers, text, parameter names, arithmetic,
# comparison, a few functions. No scripting, no side effects.
tbcad_add_library(expr
  SOURCES
    src/expr.cpp
  DEPENDS tbcad::core)
tbcad_add_tests(expr
  SOURCES
    tests/main.cpp
    tests/expr_tests.cpp)
