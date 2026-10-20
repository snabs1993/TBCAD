tbcad_add_library(template
  SOURCES
    src/template.cpp
  DEPENDS tbcad::core tbcad::io tbcad::layout)

tbcad_add_tests(template
  SOURCES
    tests/main.cpp
    tests/template_tests.cpp
  DEPENDS tbcad::template)
