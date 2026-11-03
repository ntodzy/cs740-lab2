# CONGA Lab Implementation TODO

This checklist orders work by dependency. Keep ECMP and CONGA on the same topology and traffic inputs throughout.

## 0. Record design decisions

- [x] Confirm the network configuration: 12 core switches with 24 × 40 Gb/s ports each; 24 leaf switches with 12 × 40 Gb/s uplinks and 32 × 10 Gb/s server-facing ports each; 768 servers total, each connected to exactly one leaf by a 10 Gb/s link. The leaf/core layer is a full mesh.
- [ ] Choose link propagation delay and justify queue sizes already provided by the skeleton.
- [ ] Define the offered-load denominator and traffic locality policy.
- [x] Choose DRE time constant, update rule, normalization, and metric quantization: periodic 20 us ticks, alpha 0.125, tau 160 us, transmitted bytes, and 3-bit normalized metric; interval/alpha are configurable.
- [x] Define unknown/stale table values, feedback scheduling, and deterministic tie-breaking: 10 ms expiry, unknown remote contributes zero, changed-first round-robin feedback, stable seeded per-flow/per-path tie ranks.
- [ ] Define warm-up, measurement window, incomplete-flow handling, repetitions, and seeds.

## 1. Establish a reproducible baseline

- [x] Run `make -C htsim` and preserve warning-free behavior for new code.
- [x] Smoke-test experiments 1 and 3 with short durations.
- [x] Add an `--lb=ecmp|conga` argument to experiment 2 with real congestion-aware CONGA selection and a separate ECMP baseline.
- [x] Separate experiment 2 traffic randomness (`--rngseed`) from routing/tie-break randomness (`--routingseed`); load balancers own an independent engine.
- [x] Add `--small=1` for fast debugging: 2 cores, 2 leaves, and 2 servers per leaf.

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

- [x] Select one core deterministically per flow from stable source/destination host IDs, flow sequence ID, and routing seed.
- [x] Keep all packets and ACKs of a flow on the constructed forward/reverse routes; check fabric core consistency at runtime.
- [x] Verify path distribution across all 12 cores with a 120,000-flow hash test and actual full-topology traffic.
- [x] Save basic link-use and completion-count statistics in `.links.csv`, `.routes.csv`, and `.summary.csv` beside the log prefix.

**Done when:** ECMP results are repeatable and provide a working baseline on the final topology.

Verified by `bash htsim/tests/test-routing-randomness.sh` and
`node htsim/tests/test-ecmp.mjs` (after building).

## 4. Add CONGA packet metadata

- [x] Add source leaf, destination leaf, `LBTag`, `CE`, feedback tag, feedback metric, and independent measurement/feedback validity fields in `packets/conga-metadata.h`.
- [x] Populate flow leaf context in experiment 2; assign forward IDs to data and reversed IDs to ACKs, including leaf 0.
- [x] Reset all metadata fields in `Packet::set()` because packets are pooled and reused, then apply current flow addressing.
- [x] Ensure `DataAck` copies the forward-path observation as feedback before the triggering `DataPacket` is freed (TCP/DCTCP, TIMELY, packet pair).
- [x] Preserve feedback when the ACK's independent reverse-path CE measurement starts.
- [x] Test data/ACK reuse, invalid/mismatched observations, leaf reversal, and actual sink feedback transfer (`test-dre.sh`, `test-conga-metadata.sh`).

**Done when:** metadata survives each intended route hop and a newly allocated pooled packet starts clean.

## 5. Add congestion measurement

- [x] Implement `congestion/DreTracker` and a queue-discipline-independent `DreQueueWrapper` using `EventList::Get().now()`.
- [x] Apply the paper's periodic `X *= (1-alpha)` at Tdre boundaries; add transmitted bytes, normalize with `tau = Tdre/alpha`, and quantize to 3 bits. Exclude dropped/still-queued bytes.
- [x] Expose `congestionMetric()` on each fabric wrapper for later local-uplink comparisons; reads account for idle time.
- [x] Update packet `CE` with `max(old_ce, link_metric)` on monitored fabric-link departures via the existing queue-service notifications.
- [x] Add deterministic tests for exact periodic ticks, no inter-tick decay, skipped ticks, steady-state load, saturation, quantization, unchanged queue behavior, excluded drops, and packet reuse (`bash htsim/tests/test-dre.sh`).

**Done when:** `CE` is monotonic along a path and the metric rises under load and decays while idle.

## 6. Implement leaf state and feedback

- [x] Add `leaf-switches/leafswitch.h/.cpp` with leaf ID, incoming `PacketSink`, and outgoing `egress()` hook; install both in experiment 2's cross-leaf routes.
- [x] Preallocate `ToLeaf[destination][uplink]` and `FromLeaf[source][uplink]` tables with validity, metric, timestamp, and pending-change status.
- [x] On fabric arrival, record `(source leaf, LBTag, CE)` in the destination's From-Leaf table (data and ACKs).
- [x] On outgoing traffic, stamp fresh feedback from `FromLeaf[destination]`, replacing the endpoint echo or clearing feedback if none is available.
- [x] At the original source leaf, update the matching `ToLeaf[remote leaf][feedbackLbTag]` cell independently of the return packet's own uplink.
- [x] Prioritize new/changed entries, then refresh unchanged entries; use per-peer round-robin and invalidate both tables after 10 ms without received updates. Document this lab policy in `README.md`.

**Done when:** a controlled packet from leaf A through uplink `u` updates exactly `B.FromLeaf[A][u]`, then feedback updates exactly `A.ToLeaf[B][u]`.

Verified by `bash htsim/tests/test-leaf-feedback.sh`: actual TCP ACK round trip,
different forward/return uplinks, exact cell isolation, data-carried feedback,
changed-entry priority, round-robin refresh, invalid metadata, and expiry/reactivation.
Both modes run leaf feedback; CONGA consumes the source leaf's To-Leaf state
when selecting later flows, while ECMP retains its independent hash policy.

## 7. Implement CONGA route selection

- [x] Split ECMP and CONGA route construction into each balancer's `selectRoute()` implementation; preserve existing tagging hooks and detailed topology comments.
- [x] Return a complete `RoutePath` with an opaque ID and owned forward/reverse hop vectors, including direct same-leaf paths; keep core mapping, detailed hop comments, and tagging-hook wiring in the topology. Test non-index IDs (including zero), different forward/reverse hop counts, and independent ownership of returned vectors.
- [x] Keep selection entirely in each policy's `selectRoute(context)`, derive path counts from the context, and use a distinct `PathId` type. Test varying candidate counts on one policy instance.
- [x] For every eligible uplink compute `max(local[u], ToLeaf[destination][u])`, using explicit path-to-uplink mappings and a shared decision timestamp. Unknown/expired remote metrics contribute zero.
- [x] Choose the minimum using stable seeded hash ranks over endpoint/flow/path identities, with path ID breaking hash collisions; do not alter the traffic RNG stream.
- [x] Invoke selection once in the route callback at flow creation, treating the full flow as one flowlet and retaining its forward/reverse routes.
- [x] Add decision-time chosen metrics in `.decisions.csv` and per-leaf-pair/path/uplink selection and unknown-feedback counts in `.selections.csv`; record outside policy code.
- [x] Create controlled local/remote asymmetric-congestion tests, including feedback changing a later flow's choice while an existing flow keeps its route (`bash htsim/tests/test-conga.sh`).

**Done when:** the selected uplink is the expected arg-min and all packets in that flow retain the route.

Regression checks: `bash htsim/tests/test-conga.sh`,
`bash htsim/tests/test-routing-randomness.sh`, and `node htsim/tests/test-ecmp.mjs`.
They cover malformed contexts, expiry, arbitrary path IDs, tie repeatability/order
independence, traffic RNG isolation, all workloads, and decision/counter consistency.

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
