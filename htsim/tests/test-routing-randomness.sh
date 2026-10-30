#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
test_dir=$(mktemp -d /tmp/htsim-routing-test.XXXXXX)
printf 'Test artifacts: %s\n' "$test_dir"

make
"${CXX:-clang++}" -std=c++11 -Wall -Wextra tests/test-routing-randomness.cpp \
    load-balancers/ecmp.cpp -o "$test_dir/check-rng"
"$test_dir/check-rng"

for workload in uniform pareto enterprise datamining; do
    for seed in 42 43; do
        ./htsim --expt=2 --small=1 --duration=1 --utilization=0.0001 \
            --flowsize=10000 --flowdist="$workload" --rngseed=12314 \
            --routingseed="$seed" --logfile="$test_dir/$workload-$seed" \
            > "$test_dir/$workload-$seed.out" 2> "$test_dir/$workload-$seed.err"
        # Source records contain name, start time, ID, size, source, destination.
        # Ignore IDs: they are simulator objects, not part of the traffic demand.
        sed 's/^\.//' "$test_dir/$workload-$seed.out" |
            awk '/^src[0-9]+ / { print $1, $2, $4, $5, $6 }' \
            > "$test_dir/$workload-$seed.demands"
        test -s "$test_dir/$workload-$seed.demands"
    done
    cmp "$test_dir/$workload-42.demands" "$test_dir/$workload-43.demands"
    printf '%s: identical demands across routing seeds\n' "$workload"
done
