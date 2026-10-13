# Generate THIRD_PARTY_NOTICES.txt in the build dir from third_party/notices/*.txt.
file(GLOB _notice_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/third_party/notices/*.txt")
list(SORT _notice_files)

add_custom_target(notices ALL
  COMMAND ${CMAKE_COMMAND}
          -DNOTICE_DIR=${CMAKE_SOURCE_DIR}/third_party/notices
          -DOUTPUT=${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.txt
          -P ${CMAKE_SOURCE_DIR}/cmake/GenerateNotices.cmake
  DEPENDS ${_notice_files}
  COMMENT "Generating THIRD_PARTY_NOTICES.txt"
  VERBATIM)

install(CODE "execute_process(COMMAND \"${CMAKE_COMMAND}\" --build \"${CMAKE_BINARY_DIR}\" --target notices)")
install(FILES "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.txt" "${CMAKE_SOURCE_DIR}/LICENSE"
        DESTINATION .)
