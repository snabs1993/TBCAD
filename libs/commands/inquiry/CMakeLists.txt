tbcad_add_library(inquiry
  SOURCES
    src/inquiry.cpp
    src/areatable.cpp
    src/audit.cpp
    src/measure.cpp
    src/massprop.cpp
    src/dwgprops.cpp
    src/findstray.cpp
    src/picktrace.cpp
    src/quick.cpp
    src/quickcmd.cpp
    src/calc.cpp
    src/ui.cpp
  DEPENDS tbcad::cmd tbcad::expr tbcad::geom tbcad::layout tbcad::plotstyle tbcad::ui)

tbcad_add_tests(inquiry
  SOURCES
    tests/main.cpp
    tests/inquiry_tests.cpp
    tests/quick_tests.cpp
  DEPENDS tbcad_inquiry tbcad_command_registry)
