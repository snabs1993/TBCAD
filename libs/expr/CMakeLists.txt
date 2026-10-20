# Small formula language for parametric assemblies: numbers, text, parameter names, arithmetic,
# comparison, a few functions. No scripting, no side effects.
tbcad_add_library(expr
  SOURCES
    src/expr.cpp
  DEPENDS tbcad::core)
tbcad_add_tests(expr
  SOURCES
    tests/main.cpp
    tests/expr_tests.cpp)

# Optional libFuzzer build (clang only): cmake -DTBCAD_FORMATS_FUZZ=ON
if(TBCAD_FORMATS_FUZZ)
  set(_fuzz_flags -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
  target_compile_options(tbcad_expr PRIVATE ${_fuzz_flags})
  add_executable(tbcad_expr_fuzz tests/fuzz_expr.cpp)
  target_link_libraries(tbcad_expr_fuzz PRIVATE tbcad::expr)
  target_compile_options(tbcad_expr_fuzz PRIVATE ${_fuzz_flags})
  target_link_options(tbcad_expr_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
