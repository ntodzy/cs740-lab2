#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d /tmp/htsim-dre-test.XXXXXX)
printf 'Test artifacts: %s\n' "$test_dir"
"${CXX:-clang++}" -std=c++11 -Wall -Wextra -g \
    tests/test-dre.cpp congestion/dre.cpp congestion/dre-queue-wrapper.cpp \
    packets/network.cpp packets/datapacket.cpp eventlist.cpp queue.cpp \
    fairqueue.cpp aprx-fairqueue.cpp priorityqueue.cpp stoc-fairqueue.cpp \
    -o "$test_dir/test-dre"
"$test_dir/test-dre"
