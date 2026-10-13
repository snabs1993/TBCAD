tbcad_add_library(index
  SOURCES
    src/document_indexer.cpp
    src/spatial_index.cpp
  DEPENDS tbcad::core)
tbcad_add_tests(index
  SOURCES
    tests/main.cpp
    tests/spatial_index_tests.cpp
    tests/indexer_tests.cpp
    tests/perf_tests.cpp)
