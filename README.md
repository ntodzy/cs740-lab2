# Lab 2: CONGA in htsim

**CS740 Fall 2026**

- **Released:** Oct 11 2026
- **Due:** Nov 6 2026 11:59 PM
- **Total points:** 15

## Important notes

- All answers and code that you write for this lab must be your own work. Plagiarism is a serious violation of university statutes, and all answers will be thoroughly checked for it.
- Ensure that your answer has proper headings and subheadings wherever necessary.
- **Late policy:**
  - Up to 30 minutes late – lose 0% points
  - Up to 24 hours late — lose 10% of points
  - Up to 48 hours late — lose 30% of points
  - Up to 72 hours late — lose 60% of points
  - Beyond 72 hours — lose 100% of points

## Overview

htsim is an open-source network simulator. It is designed to simulate large-scale networks and analyze their performance characteristics (such as flow completion time, throughput, packet loss). htsim has a range of features, including support for custom network topologies, multiple traffic sources and sinks, and a variety of routing and congestion control algorithms. It also provides a flexible and extensible architecture that allows researchers to customize the simulator for their specific needs.

- Part 1: Testbed setup
- Part 2: CONGA implementation
- Part 3: Performance report
- Submission Instruction

## Learning Outcomes

After completing this lab, students should be able to:

- Understand the FatTree/Close based data center network architecture;
- Understand how htsim works and use htsim to perform simulation;
- Design and implementation CONGA in htsim;

## Part 1: Testbed setup

You can find the htsim source on canvas files

```bash
wget https://pages.cs.wisc.edu/~mgliu/CS740/F26/labs/lab2/lab2-skeleton.tar.gz
```

### htsim walk through

The key components of htsim include the

- **Workload**, which determines flow size based on workload type and distribution such as Pareto, uniform, enterprise, or data mining;
- **Flowgenerator**, which generates flows with a given flow rate and size and creates TCP connections between the source and destination;
- **Networking primitives**, which include Queues and Pipes used to construct the packet testbed;
- **Eventlist**, which schedules events for the simulation;
- **Testbed**, which is used for the simulation.

htsim is a network simulator that operates on events rather than mimicking physical switch behavior. For example, when a packet is ready to be sent from a queue, instead of immediately transmitting it, the simulator schedules a sending event to occur after a calculated time based on the packet size and linkspeed. The simulator's behavior is considered to be discrete.

You can use the following two examples to understand how the simulator works. It will also help you understand how to build the CONGA environment.

- `Test_single_link.cpp`
- `Test_fat-tree_testbed.cpp`

To run htsim, just:

```bash
make
```

And the executable will be in the same directory. You might encounter some compiling issues, like missing headers. Check the clang version and see what is missing (e.g., `libstdc++-12-dev`).

```bash
clang -v
```

### Command-line flags

Run the executable from `htsim/` so the default log path resolves to
`htsim/data/htsim-log`:

```bash
cd htsim
make
./htsim --expt=1 --duration=1 --utilization=0.1 --rngseed=1729
```

Every argument must use `--name=value` syntax. Running `./htsim` without an
experiment prints the available experiment numbers.

#### Global flags

| Flag | Default | Description |
|---|---:|---|
| `--expt` | `0` | Experiment: `1` single link, `2` CONGA, or `3` fat tree. `0` prints usage. |
| `--rngseed` | `1729` | Seed for flow arrivals, sizes, endpoints, and randomized routes. |
| `--logfile` | `data/htsim-log` | Output prefix for the binary `.trace` and text `.id` files. |

#### Experiment 1: single link

| Flag | Default | Description |
|---|---:|---|
| `--duration` | `10` | Simulation duration in seconds. |
| `--linkspeed` | `10000000000` | Link rate in bits per second. |
| `--linkdelay` | `25` | Total link-delay parameter in microseconds; each directional pipe uses half. |
| `--linkbuffer` | `512000` | Queue capacity in bytes. |
| `--utilization` | `0.75` | Offered load as a fraction of link capacity. |
| `--maxflows` | `0` | Maximum concurrent flows; `0` uses utilization-based arrivals. |
| `--onoff` | `0.0` | Off-time ratio used when `maxflows` is nonzero. |
| `--flowsize` | `100000` | Average flow size in bytes. |
| `--flowdist` | `uniform` | `uniform`, `pareto`, `enterprise`, or `datamining`. |
| `--queue` | `droptail` | `droptail`, `fq`, `afq`, or `sfq`. |
| `--endhost` | `tcp` | `tcp`, `dctcp`, `timely`, or `pp` (packet pair). |
| `--trace` | empty | Path to a flow-arrival trace; replaces generated arrivals. |
| `--afqH` | `2` | AFQ count-min sketch rows. |
| `--afqB` | `1024` | AFQ count-min sketch buckets per row. |
| `--afqQ` | `32` | Number of AFQ FIFO queues. |
| `--afqBpR` | `1500` | AFQ bytes per round. |
| `--afqAlpha` | `8` | AFQ dynamic-buffer-sharing coefficient. |

The `afq*` settings apply only with `--queue=afq`. For the enterprise and
data-mining distributions, the built-in workload means replace `--flowsize`.

#### Experiment 2: CONGA

Experiment 2 builds the complete leaf/core topology and uses a deterministic
hash to select an equal-cost core per flow. CONGA table-driven selection is
still tracked in `TODO.md`.

The route callback captures a `shared_ptr<LoadBalancer>` by value. The topology
prebuilds candidate `RoutePath` objects, each containing an opaque `pathId` and
variable-length forward/reverse hop lists. They may represent an uplink path
or a tunnel; the balancer does not assume any switch tier or hop count.
`load-balancers/route-context.h` supplies these candidates plus access/delivery
hops. The selected policy's `selectRoute()` returns a complete `RoutePath`
containing forward/reverse execution routes and their opaque ID. The caller
moves those hop vectors into the simulator's installed routes. Selection is
implemented entirely inside each concrete `selectRoute()`; there is no public
candidate-index method. `RouteContext` provides endpoints, flow identity,
candidates, and access/delivery hops. The candidate count comes from that
request's context, so a policy instance can handle different numbers of paths.
Constructors configure only the routing seed. `PathId` is a distinct type with
explicit integer construction, preventing implicit mixing with candidate
indices or core numbers. Only the testbed maps IDs
to physical cores for the existing `.routes.csv`; same-leaf traffic is reported
as `-1` there. Same-leaf traffic returns a direct `RoutePath` through the same
interface, using the topology's `directPathId`. The topology supplies
`usesFabric` in the input context; it is not part of the returned path.

`EcmpLoadBalancer` in `load-balancers/ecmp.*` and `CongaLoadBalancer` in
`load-balancers/conga.*` retain separate execution paths. The CONGA scaffold
currently copies ECMP hashing and the same forward/reverse core choice; it
does not use congestion to select paths. Detailed inline hop comments and
tagging/feedback hooks live beside `buildFabricPath()` in the testbed, where
this topology's queues, pipes, and leaf switches are connected. Candidate
paths are cached per leaf pair, so their hop lists are not rebuilt per flow.

| Flag | Default | Description |
|---|---:|---|
| `--duration` | `5` | Simulation duration in seconds. |
| `--lb` | `ecmp` | `ecmp` selects the baseline; `conga` runs a separate scaffold currently using identical ECMP selection and tagging. |
| `--small` | `0` | `1` uses 2 cores, 2 leaves, and 2 servers per leaf for debugging; `0` uses the required full topology. |
| `--routingseed` | value of `--rngseed` | Independent seed for load-balancer choices and future tie-breaking. |
| `--utilization` | `0.1` | Offered load relative to aggregate server-facing capacity. |
| `--flowsize` | `100000` | Average flow size in bytes. |
| `--linkdelay` | `0.1` | Propagation delay per directional pipe in microseconds. |
| `--dre-interval` | `20` | Periodic decay interval Tdre in microseconds; positive, at most 1,000,000. |
| `--dre-alpha` | `0.125` | Discount factor α, strictly between 0 and 1; each tick multiplies X by 1 − α. |
| `--dre-levels` | `8` | Compatibility option; must be 8 because the metric is fixed to 3 bits (0–7). |
| `--flowdist` | `uniform` | `uniform`, `pareto`, `enterprise`, or `datamining`. |
| `--queue` | `droptail` | Fabric/server-downlink queue: `droptail`, `fq`, `afq`, `pq`, or `sfq`. |
| `--endhost` | `dctcp` | `tcp`, `dctcp`, `timely`, or `pp`. |

For a quick topology smoke test, run from `htsim/`:

```bash
./htsim --expt=2 --small=1 --lb=ecmp --duration=1 --utilization=0.0001 \
  --flowsize=10000 --logfile=/tmp/conga-small
```

Offered load scales with the number of active servers in either topology.

In experiment 2, `--rngseed` controls traffic (arrival times, sizes, start
jitter, and endpoints). Load balancers use a separate `std::mt19937` engine
seeded by `--routingseed` for randomized policies; ECMP directly hashes the
source host, destination host, generator flow sequence ID, and routing seed.
Repeated requests for the same flow select the same core regardless of call
order. Future policies must use the separate engine for tie-breaking
instead of `rand()`/`drand()`. Changing the routing seed or number of policy
draws therefore leaves the traffic demands unchanged. Keep traffic seed,
topology, workload, load, and duration identical when comparing policies.
Experiment 3 retains its original shared random stream.

Run `bash htsim/tests/test-routing-randomness.sh` from the repository root to
verify engine isolation, repeatable route choices, and identical demands across
routing seeds for all four workloads. Test artifacts are written under `/tmp`.

Experiment 2 also writes these files alongside the `--logfile` prefix:

| File suffix | Contents |
|---|---|
| `.routes.csv` | Flow sequence ID (matching the number in `srcN`), source/destination host, and chosen core; `-1` means same-leaf traffic. |
| `.links.csv` | Each directional queue's rate, enqueue/departure/drop packet and byte counts, and measured utilization fraction. Counters include data and ACK packets. |
| `.summary.csv` | Generated, completed, and incomplete flow counts, plus `route_violations` (fabric departures using a different core than earlier packets/ACKs of that flow; expected zero). |

Statistics are flushed after the simulation event loop. Utilization is
departed bytes × 8 divided by link rate × elapsed simulation time; generated
flows include those scheduled to start after the cutoff due to start jitter.
CSV output adds aggregate counters without periodic queue sampling.

Every directional fabric link also has a `DreQueueWrapper` around its existing
queue. This preserves FIFO/FQ/AFQ/PQ/SFQ behavior. Its independent DRE measures
transmitted bytes at queue service completion; packets dropped or still queued
do not increment X. The source-leaf wrapper sets `LBTag` and clears `CE` on
ingress. Each wrapper intercepts the existing queue-service notification to
increment X and accumulate the maximum metric in CE before forwarding; it
also delegates logging to the original statistics logger. Data and ACK
traversals are measured independently. Experiment 2 now installs a `LeafSwitch`
at each leaf to collect observations and exchange table-selected feedback on
both data and ACK packets. Congestion-aware CONGA route selection remains
unimplemented; both `--lb=ecmp` and the `--lb=conga` scaffold exercise these hooks
on static paths selected by the same hash.

The packet metadata in `packets/conga-metadata.h` contains:

| Field | Meaning |
|---|---|
| `sourceLeaf`, `destinationLeaf` | Leaf IDs for this packet's direction; reversed on ACKs. `UNKNOWN_LEAF` represents missing context, so leaf 0 is valid. |
| `valid`, `lbTag`, `ce` | Whether a fabric path measurement has started, its chosen source uplink, and its accumulated congestion metric. |
| `feedbackValid`, `feedbackLbTag`, `feedbackMetric` | Whether reverse-path feedback is present and which original uplink/metric it describes. |

Experiment 2 supplies each flow's leaf IDs from its host endpoints. Data
and retransmissions inherit forward IDs; ACKs inherit reversed IDs. The ACK
factory copies the forward observation before the data packet is freed as a
fallback for routes without leaf hooks. In experiment 2 the outgoing leaf
replaces that echo with table-selected feedback (or clears it if none is fresh),
while source-uplink initialization preserves feedback and starts a fresh CE.
TCP/DCTCP, TIMELY, and packet-pair sinks use this factory. Same-leaf traffic
has leaf IDs but no fabric measurement or valid measurement feedback.
All fields are reset on pool reuse before current-flow leaf IDs are applied.
These are simulator metadata fields; packet sizes are not increased.

Run `bash htsim/tests/test-conga-metadata.sh` to verify actual transport sinks,
ACK leaf reversal, feedback preservation through reverse ingress, and existing
ECN behavior. `bash htsim/tests/test-dre.sh` also checks all metadata fields
on data/ACK pool reuse and rejects invalid or mismatched observations.

Leaf state lives in `leaf-switches/leafswitch.h/.cpp`. Every leaf preallocates
`ToLeaf[peer][uplink]` and `FromLeaf[peer][uplink]` for the active topology;
there is no route discovery. `receivePacket()` runs after fabric propagation
and before the host-facing queue. For an incoming A→B packet it records
`B.FromLeaf[A][LBTag] = CE` and independently applies any carried feedback to
`B.ToLeaf[A][feedbackLbTag]`. `egress()` runs before the outgoing fabric queue
and selects feedback from `FromLeaf[destination]`. Both hooks call `sendOn()`
without adding queueing or delay; same-leaf routes bypass them.

Feedback policy: new or changed observations have priority, with round-robin
selection among pending entries. When none are pending, refresh all fresh
entries round-robin. Each peer has its own cursor; selection uses no RNG.
Entries store validity, metric, update timestamp, and pending status. Unknown
and expired entries are invalid, distinct from a measured zero. Both tables
expire lazily **10 ms** after their last received update (`LeafSwitch`'s
constructor parameter); expiration invalidates rather than decays the metric.
This is a lab policy, separate from DRE decay. Sending feedback clears pending
status but does not refresh the observation timestamp. Repeated observations
refresh timestamps without clearing pending changes. Without reverse traffic,
feedback waits; no standalone feedback packets are generated. Strict changed
priority can delay unchanged refreshes during continuous metric changes.

`toLeaf(peer, uplink, now)` and `fromLeaf(peer, uplink, now)` expose fresh
snapshots for debugging and later route selection. Invalid IDs/tags/metrics on
incoming packets are ignored; unrelated cells are preserved. Leaf tables are
not yet exported to CSV. Run `bash htsim/tests/test-leaf-feedback.sh` for exact
cell updates, feedback priority/round-robin, expiry boundaries, and a TCP ACK
round trip with different forward and return uplinks.

The tracker now uses the quoted paper rule: increment `X` by each transmitted
packet's size, and every Tdre apply `X *= (1 - alpha)`. Its normalization is
`X / ((link_bps / 8) * tau_seconds)`, where `tau = Tdre / alpha`. The 3-bit
metric is `min(7, floor(normalized_load * 8))`. With the default 20 us interval
and α = 0.125, tau is 160 us. These are configurable implementation defaults.

Decay ticks are anchored at simulation time zero. Accesses apply
`X *= pow(1-alpha, elapsed_whole_ticks)`, exactly accounting for skipped ticks
without periodic timer events. There is no partial decay between ticks;
a transmission exactly on a boundary applies the decay before adding its bytes.
The old exponential update remains commented in `congestion/dre.cpp`.
`--dre-tau` is now rejected; use `--dre-interval` and `--dre-alpha` instead.

`.links.csv` includes final `dre_metric`, `dre_normalized_load`, and cumulative
`dre_sent_bytes` (replacing `dre_offered_bytes`). The sent-byte counter equals
`departed_bytes` on monitored fabric links. These columns are blank for
unmonitored server links.

Related components are grouped under `htsim/`:

```text
congestion/       # DRE tracker and queue wrapper
packets/          # Packet, PacketFlow, data/ACK pools, and CONGA metadata
stats/            # Experiment statistics (AI-generated notices retained)
load-balancers/   # Load-balancer interface and ECMP policy
leaf-switches/    # Per-leaf tables and incoming/outgoing feedback hooks
```

Run `bash htsim/tests/test-dre.sh` to check periodic tick boundaries, quantization,
saturation, steady-state normalization, idle reads, packet reuse, exclusion of
queued/dropped packets, CE propagation, and equivalent
wrapped/unwrapped queue scheduling for all five queue disciplines.

After building, run `node htsim/tests/test-ecmp.mjs` from the repository root
to verify the full topology, all 12 cores carrying traffic, fixed-core packet
and ACK behavior, count consistency, all queue types, and identical outputs
on repeated runs. The hash test also checks 120,000 choices across 12 cores
against an 8% per-core deviation bound around an even split.

#### Experiment 3: fat tree

| Flag | Default | Description |
|---|---:|---|
| `--duration` | `5` | Simulation duration in seconds. |
| `--utilization` | `0.9` | Aggregate offered load as a capacity fraction. |
| `--flowsize` | `100000` | Average flow size in bytes. |
| `--flowdist` | `uniform` | `uniform`, `pareto`, `enterprise`, or `datamining`. |
| `--queue` | `droptail` | `droptail`, `fq`, `afq`, `pq`, or `sfq`. |
| `--endhost` | `dctcp` | `tcp`, `dctcp`, `dtcp`, `ddctcp`, `timely`, or `pp`. |
| `--lstf` | `0` | Deadline/coflow selector; currently has no effect because that generator is disabled. |

## Part 2: CONGA implementation

- Your task is to implement CONGA in htsim.

- **Network configuration:**
  - 12 Core switch with 24x40Gbps port
  - 24 leaf switch with 12x40Gbps uplink port and 32x10Gbps server-facing port.
  - Each server will just be connected to 1 leaf switch with a 10Gbps link

- **You need to implement the following:**
  - Build the simulation testbed based on the given configuration and sample testbeds (fat-tree and single-link )
  - A distributed congestion awareness mechanism as CONGA provided
    - You may need to define an `leafswitch.cpp` (`leafswitch.h`) to maintain the Congestion-to-leaf table and congestion-from-leaf table.
    - The leaf switch will be able to determine which uplink to take with the local table.
    - The congestion information exchange mechanism as described in CONGA (congestion feedback)
    - A way to determine the extent of congestion along the packets’ path. (DRE or other similar approach that serve the objective)

- **The following pieces from CONGA are not required:**
  - The flowlet, so you can just assume the flow will work as flowlet in CONGA paper.
  - VXLAN tunneling: you don’t have to follow the exact header definition mentioned in the paper as long as you can encode the necessary information.

### Files to edit for the CONGA implementation

The expected implementation touches the following files. Keep the ECMP and
CONGA paths in the same experiment so they share topology and workload code.

| File | Required changes |
|---|---|
| `htsim/test_conga_testbed.cpp` | Extend the implemented 12-core/24-leaf/768-server topology and route generator with selectable ECMP and CONGA policies. |
| `htsim/load-balancers/conga.h`, `htsim/load-balancers/conga.cpp` | Replace the scaffold's copied ECMP selection and evolve its independent route construction for CONGA. |
| `htsim/load-balancers/ecmp.*`, `htsim/load-balancers/route-context.h` | ECMP selection, opaque path IDs, variable-length candidate hops, and complete `RoutePath` results; core mapping stays in the testbed. |
| `htsim/packets/network.h` | Contains packet accessors and flow leaf context; `packets/conga-metadata.h` defines leaf IDs, validity, path measurement, and feedback fields. |
| `htsim/packets/network.cpp` | Resets CONGA metadata in `Packet::set()` because packet objects are pooled and reused. |
| `htsim/packets/datapacket.h`, `htsim/tcp.cpp`, `htsim/timely.cpp`, `htsim/packetpair.cpp` | ACK factory and sinks copy observed path feedback before freeing the received data packet. Experiment 2's leaf hook replaces this fallback with table-selected feedback. |

The following new files are recommended to keep CONGA logic out of generic
simulator components:

| New file | Responsibility |
|---|---|
| `htsim/leaf-switches/leafswitch.h`, `htsim/leaf-switches/leafswitch.cpp` | Implemented To-/From-Leaf tables, receive processing, changed-first round-robin feedback, and expiry; expose To-Leaf snapshots for future CONGA selection. |
| `htsim/congestion/dre.*`, `htsim/congestion/dre-queue-wrapper.*` (implemented) | Maintain simulation-time congestion estimates independently of queue discipline and update packet `CE` on fabric links. |

Depending on the measurement design, also update `htsim/flow-generator.h/.cpp`
to preserve demand information or write machine-readable flow records, and
`htsim/tcp.h/.cpp` to emit final FCT data. `htsim/test.h` already registers
experiment 2, so it only needs editing if the experiment interface changes.
The Makefile discovers `.cpp` files in `htsim/`, `htsim/load-balancers/`,
`htsim/congestion/`, `htsim/packets/`, and `htsim/stats/`, so implementations
in those directories compile automatically.

## Part 3: Performance Report

You will evaluate your own implementation in detail and compare with the ECMP. You need to report the following results for four workloads (i.e., uniform, pareto, enterprise, dataming):

1. Flow completion time for all flows varying the load for 10/20/30/40/50/60/70/80/90/100 % cases;
2. Flow completion time for small flows (<100KB) varying the load for 10/20/30/40/50/60/70/80/90/100 % cases;
3. Flow completion time for large flows (>=100KB) varying the load for 10/20/30/40/50/60/70/80/90/100 % cases;

Specifically, you will provide **12 figures (3 figs for each workload)** with load (from 10%-100%) as x-axis and the average flow completion time (FCT) as y-axis. You can find a similar figure in the CONGA paper (fig10). Please compare with ECMP and discuss why your solution outperforms/underperforms ECMP.

---

## Submission Instructions

You must submit(one submission for each group):

- The skeleton code with your implementation including the client and server source code along with their makefiles to compile them respectively.
- Your measurement results and answers to the questions from Part 3—all measurements and answers to questions in the file `report.pdf`
- A README file with the names and CS usernames of all group members, assumptions made, and how to run your application

You must submit a single tar file containing the above. To create the tar file, run the following command, replacing username1, username2 (just username1 if you are doing the lab by yourself) with the CS username of each group member:

```bash
tar czvf username1_username2.tgz htsim report.pdf README
tar czvf username1.tgz htsim report.pdf README
```

Upload the tar file on the Lab2 tab on course's Canvas page. Please submit only one tar file per group.
