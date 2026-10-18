# Dear ImGui 1.91.9 docking branch (MIT). Fetched file by file from the v1.91.9-docking tag, each
# pinned by SHA-256, and built into a static library together with the GLFW and OpenGL 3 backends.
# License text: third_party/notices/imgui.txt
#
# Provides tbcad::imgui (links tbcad::glfw). Include <imgui.h>, <imgui_impl_glfw.h>,
# <imgui_impl_opengl3.h> and <misc/cpp/imgui_stdlib.h>.
include(FetchContent)
include(deps/glfw)

if(NOT TARGET tbcad_imgui)
  set(_imgui_tag v1.91.9-docking)
  set(_imgui_base "https://raw.githubusercontent.com/ocornut/imgui/${_imgui_tag}")
  set(_imgui_root "${CMAKE_BINARY_DIR}/_deps/imgui_src")
  file(MAKE_DIRECTORY "${_imgui_root}")

  # <path in the repository> <sha256>
  set(_imgui_files
  "imgui.cpp" 4af906e69f7e7649f66449200a0bee531bcd3dbf5aeb09ad261518a88c635101
  "imgui.h" 2840241b6101d629b5804dedff698a25414ee102959120314c6a0299ca25ef9a
  "imgui_internal.h" a18df97203a9ba6a812f35eadd9f5861a0c8d558a59b96c14b68898272fe9e83
  "imgui_draw.cpp" 7fc58d765ba97813c751ba7022393fe2c67f0f51198a388edc7ebd4fcf7b86d3
  "imgui_tables.cpp" d7582d149df53bf625c2706e3313596935add7e0b283d1112edc30db4a7d8e4e
  "imgui_widgets.cpp" 1943c04da4e9abf6ca1ba36134fec708ff12f80eaf1c62bb992b72adaef2a091
  "imstb_rectpack.h" 2efa3d5f7d003c19743b15155dad9f46d9f9fc783a18893d703b431d3c990972
  "imstb_textedit.h" 77010e494caf72c178b573569612aec409d24380409676325a31d02de0ec64b7
  "imstb_truetype.h" 87f08919ae8e7223451d0e9d06b62829075d3e920c9e6ad436db9d7e7427ffce
  "imconfig.h" 0884061cc43e27e1d346fe18dcd1425792eb2532d3d16610a82eb5ac751f7594
  "backends/imgui_impl_glfw.cpp" a836e7bd65976da5621a46679227158b4f5d4972dac282a20d411975b0d19fe1
  "backends/imgui_impl_glfw.h" c9c66d372a7e2a211071e95c28e75e7d18e35c861affbd0c2b052411069ce040
  "backends/imgui_impl_opengl3.cpp" 49dbceecb8dc31a87847469aae6986a200bbec2d2b5089bd1a6f4fad1dc1d943
  "backends/imgui_impl_opengl3.h" 8b41f2d0d8d388ef9aee9c7cb73171ecae202118be3217b80cc1d6743012daca
  "backends/imgui_impl_opengl3_loader.h" 9d17b1679dc876e0ca693631030e70656c372c808c6767644c3b545834483e24
  "misc/cpp/imgui_stdlib.cpp" 677bf60458e13c2eee00b0b49453c3ef0f1f73e38930b8dfe4bff92da75f1e18
  "misc/cpp/imgui_stdlib.h" 36aa98f5b89470cf84fe4d548e42cce124fbd27e1f7e3481bbb972bc5a134585)

  list(LENGTH _imgui_files _n)
  math(EXPR _last "${_n} - 1")
  foreach(_i RANGE 0 ${_last} 2)
    list(GET _imgui_files ${_i} _path)
    math(EXPR _j "${_i} + 1")
    list(GET _imgui_files ${_j} _hash)
    # file(DOWNLOAD) rather than one FetchContent sub-build per file: those sub-build directories
    # have long names and pushed Windows paths past MAX_PATH (260).
    get_filename_component(_dir "${_path}" DIRECTORY)
    file(MAKE_DIRECTORY "${_imgui_root}/${_dir}")
    if(NOT EXISTS "${_imgui_root}/${_path}")
      file(DOWNLOAD "${_imgui_base}/${_path}" "${_imgui_root}/${_path}"
           EXPECTED_HASH SHA256=${_hash} STATUS _st)
      list(GET _st 0 _code)
      if(NOT _code EQUAL 0)
        file(REMOVE "${_imgui_root}/${_path}")
        message(FATAL_ERROR "Cannot download ${_imgui_base}/${_path}: ${_st}")
      endif()
    endif()
  endforeach()

  add_library(tbcad_imgui STATIC
    "${_imgui_root}/imgui.cpp"
    "${_imgui_root}/imgui_draw.cpp"
    "${_imgui_root}/imgui_tables.cpp"
    "${_imgui_root}/imgui_widgets.cpp"
    "${_imgui_root}/backends/imgui_impl_glfw.cpp"
    "${_imgui_root}/backends/imgui_impl_opengl3.cpp"
    "${_imgui_root}/misc/cpp/imgui_stdlib.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/imgui_extra/tbcad_imgui_items.cpp")
  target_include_directories(tbcad_imgui SYSTEM PUBLIC "${_imgui_root}")
  target_include_directories(tbcad_imgui PUBLIC "${CMAKE_CURRENT_LIST_DIR}/imgui_extra")
  # The test hooks (tbcad_imgui_items.h): widgets report their label and rectangle while a GUI
  # test has recording on; otherwise one untaken branch per widget.
  target_compile_definitions(tbcad_imgui PUBLIC IMGUI_ENABLE_TEST_ENGINE=1)
  target_compile_features(tbcad_imgui PUBLIC cxx_std_20)
  target_compile_definitions(tbcad_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1)
  target_link_libraries(tbcad_imgui PUBLIC tbcad::glfw ${CMAKE_DL_LIBS})
  set_target_properties(tbcad_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(tbcad::imgui ALIAS tbcad_imgui)
endif()
