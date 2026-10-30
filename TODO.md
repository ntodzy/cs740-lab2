# CONGA Lab Implementation TODO

This checklist orders work by dependency. Keep ECMP and CONGA on the same topology and traffic inputs throughout.

## 0. Record design decisions

- [x] Confirm the network configuration: 12 core switches with 24 × 40 Gb/s ports each; 24 leaf switches with 12 × 40 Gb/s uplinks and 32 × 10 Gb/s server-facing ports each; 768 servers total, each connected to exactly one leaf by a 10 Gb/s link. The leaf/core layer is a full mesh.
- [ ] Choose link propagation delay and justify queue sizes already provided by the skeleton.
- [ ] Define the offered-load denominator and traffic locality policy.
- [ ] Choose DRE time constant, update rule, normalization, and metric quantization.
- [ ] Define unknown/stale table values, feedback scheduling, and deterministic tie-breaking.
- [ ] Define warm-up, measurement window, incomplete-flow handling, repetitions, and seeds.

## 1. Establish a reproducible baseline

- [ ] Run `make -C htsim` and preserve warning-free behavior for new code.
- [ ] Smoke-test experiments 1 and 3 with short durations.
- [ ] Add an `--lb=ecmp|conga` argument to experiment 2.
- [ ] Separate traffic randomness from routing/tie-break randomness so both modes receive identical demands.
- [ ] Add a small topology mode (for example `--small=1`) for fast debugging.

**Done when:** the same seed produces identical `(start, source, destination, size)` demands in both modes.

## 2. Build the leaf/core topology

- [x] Replace the stub in `htsim/test_conga_testbed.cpp` with directional host–leaf and leaf–core queue/pipe arrays.
- [x] Create 12 cores, 24 leaves, and 768 host IDs without duplicate attachments.
- [x] Use 10 Gb/s host links and 40 Gb/s fabric links.
- [x] Name queues/pipes and register them with `Logfile`.
- [x] Generate correct forward and reverse routes for same-leaf and cross-leaf traffic.
- [x] Set workload, duration, utilization, transport, and RNG parameters from `ArgList`.

**Done when:** every host pair is reachable and assertions confirm each leaf has 32 hosts and 12 core links.

## 3. Implement the ECMP comparison

- [ ] Select one core deterministically per flow from stable flow/endpoint data.
- [ ] Keep all packets and ACKs of a flow on the constructed forward/reverse routes.
- [ ] Verify path distribution across all 12 cores under many flows.
- [ ] Save basic link-use and completion-count statistics.

**Done when:** ECMP results are repeatable and provide a working baseline on the final topology.

## 4. Add CONGA packet metadata

- [ ] Add source leaf, destination leaf, `LBTag`, `CE`, feedback tag, feedback metric, and validity fields to `Packet` (or a contained metadata struct).
- [ ] Reset every field in `Packet::set()` because packets are pooled and reused.
- [ ] Ensure `DataAck` receives feedback before the triggering `DataPacket` is freed.
- [ ] Test data and ACK reuse for stale metadata leakage.

**Done when:** metadata survives each intended route hop and a newly allocated pooled packet starts clean.

## 5. Add congestion measurement

- [ ] Implement a `CongaQueue`/DRE tracker using `EventList::Get().now()`.
- [ ] Decay by elapsed simulation time, add arriving bytes, normalize by link rate and the chosen time constant, then quantize.
- [ ] Expose the current metric to the owning leaf for local-uplink comparison.
- [ ] Update packet `CE` with `max(old_ce, link_metric)` on monitored fabric links.
- [ ] Add deterministic tests for no elapsed time, long idle decay, saturation, and quantization boundaries.

**Done when:** `CE` is monotonic along a path and the metric rises under load and decays while idle.

## 6. Implement leaf state and feedback

- [ ] Add `leafswitch.h/.cpp` with `PacketSink` behavior and leaf ID.
- [ ] Allocate `ToLeaf[destination][uplink]` and `FromLeaf[source][uplink]` tables.
- [ ] On forward arrival, record `(source leaf, LBTag, CE)` in the destination's From-Leaf table.
- [ ] On reverse traffic, select and stamp a valid feedback entry.
- [ ] At the original source leaf, update the matching To-Leaf cell.
- [ ] Implement and document feedback priority/round-robin and stale-entry policy.

**Done when:** a controlled packet from leaf A through uplink `u` updates exactly `B.FromLeaf[A][u]`, then feedback updates exactly `A.ToLeaf[B][u]`.

## 7. Implement CONGA route selection

- [ ] For every eligible uplink compute `max(local[u], ToLeaf[destination][u])`.
- [ ] Choose the minimum using deterministic tie-breaking that does not alter the traffic RNG stream.
- [ ] Invoke selection once in the route callback, treating the full flow as one flowlet.
- [ ] Add counters for selected uplinks and selection metrics.
- [ ] Create a controlled asymmetric-congestion test.

**Done when:** the selected uplink is the expected arg-min and all packets in that flow retain the route.

## 8. Add experiment output

- [ ] Emit one machine-readable record per completed flow: mode, workload, load, seed, flow ID, source, destination, size, start, finish, and FCT.
- [ ] Emit generated/completed/incomplete counts and run configuration.
- [ ] Categorize `<100000` bytes as small and `>=100000` bytes as large.
- [ ] Add a script to aggregate average FCT and preserve raw results.
- [ ] Add a plotting script for ECMP versus CONGA.

**Done when:** one command can produce a CSV and a plot without parsing unrelated diagnostics.

## 9. Run the evaluation matrix

- [ ] Run `uniform`, `pareto`, `enterprise`, and `datamining`.
- [ ] Run loads `0.1, 0.2, ... 1.0` for both `ecmp` and `conga`.
- [ ] Use identical seeds/demands and multiple repetitions per paired configuration.
- [ ] Produce all-flow, small-flow, and large-flow averages: 12 figures total.
- [ ] Check for high-load truncation and rerun with a longer drain/measurement window if necessary.
- [ ] Archive commands, configuration, raw CSVs, aggregated data, and plots.

## 10. Report and submission

- [ ] Explain topology, DRE equation/units, feedback, tie-breaking, aging, load definition, and reproducibility controls.
- [ ] Discuss ECMP collisions, feedback lag/staleness, workload differences, and unexpected results.
- [ ] Update `README` with members, CS usernames, assumptions, and exact run commands.
- [ ] Build from clean state and run final smoke/regression tests.
- [ ] Include source, Makefile, `report.pdf`, and README in the required username-named tarball.
