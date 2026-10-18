# Tables (Q71): grid of text cells with merges, row/column editing, display and explode.
# See docs/table.md.
tbcad_add_library(table
  SOURCES
    src/formula.cpp
    src/style.cpp
    src/table.cpp
    src/display.cpp
    src/csv.cpp
  DEPENDS tbcad::core tbcad::geom tbcad::text)

tbcad_add_tests(table
  SOURCES
    tests/main.cpp
    tests/table_tests.cpp)
