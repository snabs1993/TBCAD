# Printing (Q83): sends a plotted sheet to a real printer. The drawing code is shared and tested
# everywhere; the Windows backend uses GDI and the system print dialog, other systems get a stub.
set(_print_sources src/canvas.cpp)
if(WIN32)
  list(APPEND _print_sources src/printer_win32.cpp)
else()
  list(APPEND _print_sources src/printer_stub.cpp)
endif()

tbcad_add_library(print SOURCES ${_print_sources} DEPENDS tbcad::core tbcad::export)
if(WIN32)
  target_link_libraries(tbcad_print PRIVATE winspool gdi32 comdlg32 user32)
  target_compile_definitions(tbcad_print PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

tbcad_add_tests(print
  SOURCES
    tests/main.cpp
    tests/canvas_tests.cpp)
