#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
make
test_dir=$(mktemp -d /tmp/htsim-metadata-test.XXXXXX)
printf 'Test artifacts: %s\n' "$test_dir"
objects=()
for source in *.cpp load-balancers/*.cpp congestion/*.cpp packets/*.cpp stats/*.cpp; do
    case "$source" in main.cpp|test_*.cpp) continue ;; esac
    objects+=(".obj/${source%.cpp}.o")
done
"${CXX:-clang++}" -std=c++11 -Wall -Wextra -g tests/test-conga-metadata.cpp \
    "${objects[@]}" -o "$test_dir/test-metadata"
"$test_dir/test-metadata"
