cmake_minimum_required(VERSION 3.28)

project(TBCAD VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(WIN32)
  set(_tbcad_app_default ON)
else()
  set(_tbcad_app_default OFF)
endif()
option(TBCAD_BUILD_APP "Build the tbcad desktop application" ${_tbcad_app_default})
option(TBCAD_BUILD_TESTS "Build unit tests" ON)
option(TBCAD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(TbcadModule)

if(TBCAD_BUILD_TESTS)
  include(deps/doctest)
  enable_testing()
endif()

# Module auto-discovery. Order: libs, then command modules, then apps.
# Within a level the order is alphabetical; targets must be linked by target
# name (tbcad::<name>), never rely on directory order.
file(GLOB _tbcad_libs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/libs/*/CMakeLists.txt")
file(GLOB _tbcad_cmds CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/libs/commands/*/CMakeLists.txt")
file(GLOB _tbcad_apps CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/apps/*/CMakeLists.txt")
list(SORT _tbcad_libs)
list(SORT _tbcad_cmds)
list(SORT _tbcad_apps)

foreach(_cml IN LISTS _tbcad_libs _tbcad_cmds)
  get_filename_component(_dir "${_cml}" DIRECTORY)
  file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_dir}")
  add_subdirectory("${_dir}" "${_rel}")
endforeach()

# Command line tools build everywhere; the desktop application only when TBCAD_BUILD_APP is on.
set(_tbcad_cli_apps tbcad-convert tbcad-mcp)
foreach(_cml IN LISTS _tbcad_apps)
  get_filename_component(_dir "${_cml}" DIRECTORY)
  get_filename_component(_name "${_dir}" NAME)
  if(TBCAD_BUILD_APP OR _name IN_LIST _tbcad_cli_apps)
    file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_dir}")
    add_subdirectory("${_dir}" "${_rel}")
  endif()
endforeach()

include(TbcadNotices)
