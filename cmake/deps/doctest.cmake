# doctest 2.4.11 (MIT). Header-only test framework, fetched as single files,
# each pinned by SHA-256. License text: third_party/notices/doctest.txt
include(FetchContent)

set(_doctest_tag v2.4.11)
set(_doctest_base "https://raw.githubusercontent.com/doctest/doctest/${_doctest_tag}")

function(_tbcad_doctest_file var name path hash)
  FetchContent_Declare(${name}
    URL "${_doctest_base}/${path}"
    URL_HASH SHA256=${hash}
    DOWNLOAD_NO_EXTRACT TRUE)
  FetchContent_MakeAvailable(${name})
  set(${var} "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

_tbcad_doctest_file(_dt_header doctest_header doctest/doctest.h
  44faa038e9c3f9728efbda143748d01124ea0a27f4bf78f35a15d8fab2e039fb)
_tbcad_doctest_file(_dt_cmake doctest_cmake scripts/cmake/doctest.cmake
  38b354a72dd86a22919a9c71aebdd367e368281e4036f1e2e9150b76e16b5b4b)
_tbcad_doctest_file(_dt_add doctest_addtests scripts/cmake/doctestAddTests.cmake
  68ff6641f7fed897e8579be58b96d01cd5a3cae3abe9ecb185e2d0a94ff7cce2)

# doctest.h must be reachable as <doctest/doctest.h>.
set(_dt_inc "${CMAKE_BINARY_DIR}/_deps/doctest_include/doctest")
file(MAKE_DIRECTORY "${_dt_inc}")
file(COPY "${_dt_header}/doctest.h" DESTINATION "${_dt_inc}")

if(NOT TARGET doctest::doctest)
  add_library(doctest INTERFACE)
  add_library(doctest::doctest ALIAS doctest)
  target_include_directories(doctest SYSTEM INTERFACE "${CMAKE_BINARY_DIR}/_deps/doctest_include")
endif()

# Provide doctest_discover_tests() (needs doctestAddTests.cmake next to doctest.cmake).
file(COPY "${_dt_cmake}/doctest.cmake" "${_dt_add}/doctestAddTests.cmake"
     DESTINATION "${CMAKE_BINARY_DIR}/_deps/doctest_cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_BINARY_DIR}/_deps/doctest_cmake")
set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
