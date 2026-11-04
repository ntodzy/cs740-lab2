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
    for mode in ecmp conga; do
        for seed in 42 43; do
            prefix="$test_dir/$workload-$mode-$seed"
            ./htsim --expt=2 --lb="$mode" --small=1 --duration=1 --utilization=0.0001 \
                --flowsize=10000 --flowdist="$workload" --rngseed=12314 \
                --routingseed="$seed" --logfile="$prefix" --verbose=1 \
                > "$prefix.out" 2> "$prefix.err"
            # Source records contain name, start time, ID, size, source, destination.
            # Ignore IDs: they are simulator objects, not part of the traffic demand.
            sed 's/^\.//' "$prefix.out" |
                awk '/^src[0-9]+ / { print $1, $2, $4, $5, $6 }' \
                > "$prefix.demands"
            test -s "$prefix.demands"
        done
        cmp "$test_dir/$workload-$mode-42.demands" "$test_dir/$workload-$mode-43.demands"
    done
    for seed in 42 43; do
        cmp "$test_dir/$workload-ecmp-$seed.demands" "$test_dir/$workload-conga-$seed.demands"
    done
    printf '%s: identical demands across routing seeds and ECMP/CONGA policies\n' "$workload"
done
