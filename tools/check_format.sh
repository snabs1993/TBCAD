#!/usr/bin/env bash
# Checks C/C++ formatting with clang-format (config: .clang-format).
set -eu
cd "$(dirname "$0")/.."
mapfile -t files < <(git ls-files 'libs/*' 'apps/*' 'tests/*' | grep -E '\.(h|hpp|c|cc|cpp|cxx)$' || true)
if [ ${#files[@]} -eq 0 ]; then echo "No C++ files"; exit 0; fi
clang-format --dry-run --Werror "${files[@]}"
echo "Format OK (${#files[@]} files)"
