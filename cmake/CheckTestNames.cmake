# Build-time guard: run as `cmake -DSOURCES=a|b|c -P CheckTestNames.cmake`.
# ctest cuts a test name at ';' (CMake's list separator), so such a test would run as nothing and
# pass. Failing here (at build) keeps CMake configure alive for everyone else.
string(REPLACE "|" ";" _sources "${SOURCES}")
set(_failed FALSE)
foreach(_src IN LISTS _sources)
  if(EXISTS "${_src}")
    file(STRINGS "${_src}" _lines REGEX "TEST_CASE\\(\"[^\"]*;")
    foreach(_l IN LISTS _lines)
      message(SEND_ERROR "${_src}: TEST_CASE name contains ';' (ctest would not run it): ${_l}\n  -> use ',' or ' - ' instead of ';' in the test name")
      set(_failed TRUE)
    endforeach()
  endif()
endforeach()
