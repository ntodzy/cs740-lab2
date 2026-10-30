# htsim Framework Tutorial for the CONGA Lab

## 1. What this repository contains

This is a small C++11 discrete-event network simulator. It does not emulate switches in real time. Instead, objects schedule events at future **simulation timestamps**, and `EventList` executes them in timestamp order. The current executable contains three experiments:

| Experiment | Source | Purpose |
|---|---|---|
| `--expt=1` | `test_single_link.cpp` | Learn queues, pipes, workloads, and TCP |
| `--expt=2` | `test_conga_testbed.cpp` | Lab target; currently only constants and a stub |
| `--expt=3` | `test_fat-tree_testbed.cpp` | Example multi-path topology and route construction |

Build and discover the command-line interface with:

```bash
make -C htsim
./htsim/htsim
./htsim/htsim --expt=1 --duration=1 --utilization=0.1 \
  --flowdist=uniform --endhost=dctcp --rngseed=1729
```

Arguments must have the form `--name=value`. The build currently succeeds with warnings because experiment 2 is unused.

## 2. The execution model

`main.cpp` parses arguments, seeds the global random generator, creates a `Logfile`, and calls `run_experiment()` from `test.h`. An experiment constructs the topology, creates a `FlowGenerator`, and sets the simulation end time. The main loop then repeatedly calls `EventList::doNextEvent()`.

```text
main
  -> experiment builds queues, pipes, routes, and generators
  -> EventList selects the earliest pending EventSource
  -> source handles the event and may schedule later events
  -> loop ends when no eligible events remain
```

Time is stored as `simtime_picosec` (`uint64_t`). Always use helpers in `htsim.h`, such as `timeFromUs()` and `timeAsUs()`. CONGA timers and DRE decay must use `EventList::Get().now()`, never wall-clock time.

## 3. Core classes and files

| Area | Main files | Responsibility |
|---|---|---|
| Scheduling | `eventlist.*` | Singleton event queue and current simulation time |
| Packets/routes | `network.*`, `datapacket.*` | Packet base class, data/ACK packets, ordered `route_t` hops |
| Links | `queue.*`, `pipe.*` | Serialization/buffering and propagation delay |
| Transport | `datasource.*`, `datasink.*`, `tcp.*` | Flow endpoints, TCP/DCTCP, ACKs, retransmission |
| Traffic | `flow-generator.*`, `workloads.*` | Arrival process, endpoints, sizes, and flow creation |
| Results | `logfile.*`, `loggers.*` | Binary event records, object IDs, queue/TCP sampling |
| Scenarios | `test_*.cpp`, `test.h` | Topology construction and experiment registration |

`PacketSink::receivePacket()` is the common interface for every hop. `Packet::sendOn()` advances `_nexthop` through a `route_t`, which is simply a `vector<PacketSink*>`.

## 4. Following one packet

A typical forward route alternates queues and pipes:

```text
TcpSrc -> Queue -> Pipe -> Queue -> Pipe -> TcpSink
```

1. `FlowGenerator::createFlow()` asks a route callback for forward and reverse routes.
2. It constructs a `TcpSrc` and `TcpSink`, appends the sink/source as terminal hops, and schedules the source start event.
3. `TcpSrc` allocates pooled `DataPacket` objects and calls `sendOn()`.
4. `Queue::receivePacket()` buffers or drops the packet. Its service event occurs after `packet_bytes * 8 / link_bps`.
5. `Pipe` schedules delivery after a fixed propagation delay.
6. `TcpSink` updates its cumulative ACK and creates a `DataAck` on the reverse route.
7. `TcpSrc` processes ACKs, changes its congestion window, and reports completion after the flow size is acknowledged.

The queue marks ECN when its occupancy exceeds `dctcpThreshold()`. DCTCP reacts to the echoed `ECN_REV` flag. This is transport congestion control; CONGA path selection is a separate layer.

Packets are recycled through `PacketDB`. Any new metadata must therefore be initialized every time `Packet::set()` is called. Feedback copied from a data packet into an ACK must be copied before the data packet is returned to its pool.

## 5. Flow generation and workloads

`FlowGenerator` uses a Poisson arrival process unless trace mode or replacement mode is selected. The requested bit rate and the workload mean determine the mean inter-arrival time. Built-in distributions are:

- `uniform`: every flow uses `--flowsize` bytes;
- `pareto`: heavy-tailed sizes around the configured mean;
- `enterprise`: the embedded CONGA enterprise CDF, with a fixed mean of 215 KB;
- `datamining`: the embedded data-mining CDF, with a fixed mean of 12.5 MB.

The random route callback chooses source/destination hosts and constructs the entire path. Consequently, the present framework chooses a path **once per flow**. That directly supports the lab simplification “one flow is one flowlet”: select a CONGA uplink in the callback and keep every packet of that flow on the returned route.

In experiment 2, endpoint selection, arrivals, start jitter, and workload sizes use the traffic `rand()` stream seeded by `--rngseed`. ECMP hashes source/destination host IDs, the generator flow sequence ID, and `--routingseed` (defaulting to the traffic seed). Load balancers also own an independent `std::mt19937` engine for future randomized policies and CONGA tie-breaking. This separation keeps demands identical when policies consume different numbers of random values. Experiment 3 still uses its original shared stream. Regression checks are available via `bash htsim/tests/test-routing-randomness.sh` and `node htsim/tests/test-ecmp.mjs` from the repository root.

## 6. Reading the existing topology example

`test_fat-tree_testbed.cpp` is the best construction template. It:

1. allocates directional `Queue*` and `Pipe*` arrays;
2. constructs each physical direction separately;
3. names objects and registers names with `Logfile`;
4. maps host IDs into subtree, ToR, and server indices;
5. builds forward and reverse routes in `generateRandomRoute()`; and
6. configures a workload, offered rate, and end time.

Do not copy its topology literally. The confirmed lab configuration is a two-tier fabric with 12 core switches, each with 24 × 40 Gb/s ports, and 24 leaf switches, each with 12 × 40 Gb/s uplinks and 32 × 10 Gb/s server-facing ports. This forms a full mesh between leaves and cores: each leaf has one link to every core, and each core has one link to every leaf. There are 768 servers total, each attached to exactly one leaf by a 10 Gb/s link.

## 7. Mapping CONGA onto htsim

The cleanest design is to keep policy at leaves and measurement at fabric queues:

| New concept | Suggested location |
|---|---|
| `LBTag`, `CE`, feedback tag/metric, validity | Base `Packet`, so data and ACK packets can carry it |
| DRE-like link metric | A `CongaQueue` subclass or reusable tracker owned by each fabric queue |
| To-/From-Leaf tables | A `LeafSwitch` implementing `PacketSink` |
| Uplink choice | `LeafSwitch::chooseUplink()`, called by the route generator |
| ECMP/CONGA selection | Experiment argument such as `--lb=ecmp` or `--lb=conga` |

A useful route shape is:

```text
source host -> source leaf -> selected leaf/core queue+pipe
            -> core/destination-leaf queue+pipe -> destination leaf -> destination host
```

Static route entries still model the chosen core; leaf objects update metadata as packets pass. A monitored fabric queue updates its simulation-time DRE and applies `CE = max(CE, link_metric)`. The destination leaf records the observation in `FromLeaf[source][LBTag]`. A reverse packet carries a valid feedback entry; the original source leaf updates `ToLeaf[destination][FB_LBTag]`.

For candidate uplink `u`, choose the smallest:

```text
max(local_uplink_metric[u], ToLeaf[destination_leaf][u])
```

Define initialization, quantization, tie-breaking, feedback selection, and stale-entry behavior in the report. A 160 microsecond DRE constant and 3-bit metric are paper reference choices from `CONGA_Notes.md`, not explicit lab requirements.

## 8. Measurement and lab methodology

Completed TCP flows already print a line containing size, start time, end time, and FCT in microseconds. Live flows are dumped at the generator cutoff. The `.trace` file is a packed binary `Record` stream; `.id` maps numeric object IDs to names. For reliable plotting, add a stable CSV output or parser rather than scraping mixed diagnostic output indefinitely.

Experiment 2 now also saves aggregate `.links.csv`, `.routes.csv`, and `.summary.csv` files using the log prefix. These provide link counters/utilization, per-flow path selections, completed/incomplete counts, and a runtime core-consistency check. Completed-flow FCTs remain in the text output.

Run identical ECMP and CONGA trials for:

- four workloads: uniform, Pareto, enterprise, and data mining;
- loads from 10% through 100% in 10% steps;
- all flows, small flows below 100 KB, and large flows at least 100 KB.

This produces 12 figures, each with ECMP and CONGA series. Record the seed, duration, warm-up policy, incomplete-flow policy, exact offered-load formula, transport, queue sizes, DRE parameters, and number of repetitions. Average only completed-flow FCTs, but also report completion counts so truncation cannot silently bias high-load results.

## 9. Recommended validation ladder

Validate in small steps before running 768 hosts:

1. Build and run the single-link experiment.
2. Construct a two-leaf/two-core topology and verify every route hop.
3. Scale to 24 leaves, 12 cores, and 768 unique host attachments.
4. Run deterministic ECMP and check reachability and link rates.
5. Unit-test DRE decay and quantization with fixed timestamps.
6. Force one uplink to be congested and verify CONGA selects another.
7. Verify `CE` never decreases and feedback updates the exact table cell.
8. Compare ECMP and CONGA with the same demand sequence before running the full matrix.

