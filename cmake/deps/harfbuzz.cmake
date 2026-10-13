# HarfBuzz 10.1.0 ("old MIT" license). Built from the amalgamated source
# src/harfbuzz.cc with only the OpenType shaper; glyph outlines come from
# FreeType, so HarfBuzz's own FreeType, GLib, ICU and Graphite support are off.
# License text: third_party/notices/harfbuzz.txt
include(FetchContent)

FetchContent_Declare(harfbuzz_src
  URL https://github.com/harfbuzz/harfbuzz/releases/download/10.1.0/harfbuzz-10.1.0.tar.xz
  URL_HASH SHA256=6ce3520f2d089a33cef0fc48321334b8e0b72141f6a763719aaaecd2779ecb82
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  # We compile the amalgamated source ourselves; skip HarfBuzz's own CMake.
  SOURCE_SUBDIR tbcad-no-cmake)
FetchContent_MakeAvailable(harfbuzz_src)

if(NOT TARGET harfbuzz)
  add_library(harfbuzz STATIC "${harfbuzz_src_SOURCE_DIR}/src/harfbuzz.cc")
  target_include_directories(harfbuzz SYSTEM PUBLIC "${harfbuzz_src_SOURCE_DIR}/src")
  target_compile_features(harfbuzz PRIVATE cxx_std_14)
  set_target_properties(harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_options(harfbuzz PRIVATE /w)
  else()
    target_compile_options(harfbuzz PRIVATE -w)
  endif()
  add_library(harfbuzz::harfbuzz ALIAS harfbuzz)
endif()
