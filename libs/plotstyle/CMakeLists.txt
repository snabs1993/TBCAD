# Plot style tables (Q80): map the colour of an object to the colour, lineweight and screening it
# is plotted with. Our own text format, built-in tables, and import of .ctb files.
include(deps/miniz)

tbcad_add_library(plotstyle
  SOURCES src/plotstyle.cpp src/ctb.cpp
  DEPENDS tbcad::core)
target_link_libraries(tbcad_plotstyle PRIVATE miniz)

tbcad_add_tests(plotstyle
  SOURCES
    tests/main.cpp
    tests/plotstyle_tests.cpp
  DEPENDS miniz)
