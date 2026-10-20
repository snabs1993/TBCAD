# Helpers for TBCAD modules.
#
#   tbcad_add_library(<name> [STATIC|SHARED|INTERFACE] SOURCES <files...> [DEPENDS <targets...>])
#     Creates target tbcad_<name> with alias tbcad::<name>, public include dir
#     include/, warnings, C++20.
#
#   tbcad_add_tests(<name> SOURCES <files...> [DEPENDS <targets...>])
#     Creates executable tbcad_<name>_tests (doctest with its own main) and
#     registers its test cases with ctest. No-op when TBCAD_BUILD_TESTS is OFF.

function(tbcad_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    # getenv, fopen, sscanf, strdup and friends are standard or POSIX; MSVC's "use the _s
    # version" advice (C4996) is not portable, so it is turned off rather than worked around.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS
                                                 _CRT_NONSTDC_NO_DEPRECATE)
    if(TBCAD_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    # Clang's -Wshadow matches MSVC's /W4 C4456-C4459 (a declaration hides another), which break
    # the Windows build with /WX. (GCC's -Wshadow also flags constructor parameters; not used.)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(${target} PRIVATE -Wshadow)
    endif()
    if(TBCAD_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(tbcad_add_library name)
  cmake_parse_arguments(ARG "STATIC;SHARED;INTERFACE" "" "SOURCES;DEPENDS" ${ARGN})
  set(target tbcad_${name})
  if(ARG_INTERFACE)
    add_library(${target} INTERFACE)
    target_include_directories(${target} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(${target} INTERFACE cxx_std_20)
    if(ARG_DEPENDS)
      target_link_libraries(${target} INTERFACE ${ARG_DEPENDS})
    endif()
  else()
    if(ARG_SHARED)
      set(kind SHARED)
    else()
      set(kind STATIC)
    endif()
    add_library(${target} ${kind} ${ARG_SOURCES})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(${target} PUBLIC cxx_std_20)
    tbcad_set_warnings(${target})
    if(ARG_DEPENDS)
      target_link_libraries(${target} PUBLIC ${ARG_DEPENDS})
    endif()
  endif()
  add_library(tbcad::${name} ALIAS ${target})
endfunction()

function(tbcad_add_tests name)
  if(NOT TBCAD_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
  set(target tbcad_${name}_tests)
  # ctest cuts a test name at ';' (CMake's list separator): a build-time check (not configure,
  # which would break every developer) fails with a clear message.
  set(_abs_sources "")
  foreach(_src IN LISTS ARG_SOURCES)
    if(NOT IS_ABSOLUTE "${_src}")
      set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
    endif()
    list(APPEND _abs_sources "${_src}")
  endforeach()
  string(REPLACE ";" "|" _joined "${_abs_sources}")
  add_executable(${target} ${ARG_SOURCES})
  add_custom_target(${target}_names
    COMMAND ${CMAKE_COMMAND} -DSOURCES=${_joined} -P "${PROJECT_SOURCE_DIR}/cmake/CheckTestNames.cmake"
    COMMENT "Checking TEST_CASE names in ${target}" VERBATIM)
  add_dependencies(${target} ${target}_names)
  target_compile_features(${target} PRIVATE cxx_std_20)
  tbcad_set_warnings(${target})
  target_link_libraries(${target} PRIVATE tbcad::${name} ${ARG_DEPENDS} doctest::doctest)
  include(doctest)
  doctest_discover_tests(${target})
endfunction()
