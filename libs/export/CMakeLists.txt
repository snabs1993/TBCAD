# Plot / export (W5-6): plot layout, display lists at plot resolution, PDF, SVG, PNG and JPEG output.
include(deps/miniz)

tbcad_add_library(export
  SOURCES
    src/plot.cpp
    src/plot_pdf.cpp
    src/plot_svg.cpp
    src/plot_raster.cpp
    src/plot_image.cpp
  DEPENDS tbcad::core tbcad::geom tbcad::pdf tbcad::plotstyle tbcad::underlay)
target_link_libraries(tbcad_export PRIVATE miniz)

tbcad_add_tests(export
  SOURCES
    tests/main.cpp
    tests/plot_tests.cpp
  DEPENDS tbcad::pdf tbcad::blocks miniz)
