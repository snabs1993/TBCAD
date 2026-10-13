# Sheet lists (Q46): layouts from several .tbcad files plotted into one multi-page PDF.
tbcad_add_library(sheets
  SOURCES src/sheets.cpp
  DEPENDS tbcad::core tbcad::export tbcad::layout tbcad::io)

tbcad_add_tests(sheets
  SOURCES
    tests/main.cpp
    tests/sheets_tests.cpp
  DEPENDS tbcad::pdf)
