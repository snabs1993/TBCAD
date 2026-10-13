tbcad_add_library(inquiry
  SOURCES
    src/inquiry.cpp
    src/measure.cpp
    src/ui.cpp
  DEPENDS tbcad::cmd tbcad::geom tbcad::ui)

tbcad_add_tests(inquiry
  SOURCES
    tests/main.cpp
    tests/inquiry_tests.cpp
  DEPENDS tbcad_inquiry tbcad_command_registry)
