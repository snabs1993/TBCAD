# Plot / export (W5-6): plot layout, display lists at plot resolution, PDF and SVG output.
tbcad_add_library(export
  SOURCES
    src/plot.cpp
    src/plot_pdf.cpp
    src/plot_svg.cpp
  DEPENDS tbcad::core tbcad::geom tbcad::pdf tbcad::plotstyle)

tbcad_add_tests(export
  SOURCES
    tests/main.cpp
    tests/plot_tests.cpp
  DEPENDS tbcad::pdf)
