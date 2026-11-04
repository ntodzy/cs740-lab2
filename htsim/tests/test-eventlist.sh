#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
make
test_dir=$(mktemp -d /tmp/htsim-eventlist-test.XXXXXX)
objects=()
for source in *.cpp load-balancers/*.cpp congestion/*.cpp packets/*.cpp stats/*.cpp leaf-switches/*.cpp; do
    case "$source" in main.cpp|test_*.cpp) continue ;; esac
    objects+=(".obj/${source%.cpp}.o")
done
"${CXX:-clang++}" -std=c++11 -Wall -Wextra -g tests/test-eventlist.cpp \
    "${objects[@]}" -o "$test_dir/test-eventlist"
"$test_dir/test-eventlist"
