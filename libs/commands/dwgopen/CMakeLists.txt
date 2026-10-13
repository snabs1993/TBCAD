# Glue between the application and the file-format backends: opens .dwg read-only. Save As
# .tbcad/.dxf is AppCore itself. Backends are registered through AppCore::setForeignReader, so
# another DWG reader can replace libs/dwg here without touching the application.
tbcad_add_library(dwgopen
  SOURCES src/dwgopen.cpp
  DEPENDS tbcad::appcore tbcad::dwg tbcad::dxf tbcad::xref)

tbcad_add_tests(dwgopen
  SOURCES
    tests/main.cpp
    tests/dwgopen_tests.cpp)
