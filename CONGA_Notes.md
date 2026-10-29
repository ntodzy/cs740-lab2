# CONGA Implementation Notes — CS740 Lab 2

> Supplemental CONGA concepts and implementation guidance. Not a replacement for the Lab 2 assignment.

## A. How CONGA fits into the datacenter networking material

**ECMP:** Typically hashes each TCP flow to a single equal-cost path. It is inexpensive and stable, but an unlucky hash can put multiple large flows on the same congested path while others remain lightly utilized.

**CONGA:** Makes *congestion-aware* load-balancing decisions at the source leaf. It collects path-level congestion information from remote destination leaves, then chooses a less-congested uplink.

**VL2/Clos:** A leaf-spine (two-tier Clos) fabric supplies multiple equal-cost paths that load balancing can use. CONGA makes those path choices dynamic, rather than relying on fixed flow hashes.

**NDP (contrast, not part of this lab):** Uses per-packet multipath, trimming, and receiver PULLs. CONGA instead uses congestion feedback in the network and normally changes paths at **flowlet** boundaries, avoiding TCP changes and excessive reordering. For this lab **flowlet detection is explicitly waived**: treating an entire flow as one flowlet is allowed.

**DCTCP (separate layer):** Adjusts the sender's congestion window based on ECN marking. CONGA addresses a different problem: which available *network path* should carry a flow or flowlet? Congestion control and load balancing are complementary.

## B. The required topology

The assignment specifies:

| Component | Count | Ports and speeds |
|---|---:|---|
| Core switches | 12 | 24 × 40 Gb/s ports per switch |
| Leaf switches | 24 | 12 × 40 Gb/s uplinks and 32 × 10 Gb/s server-facing ports per leaf |
| Servers | 24 × 32 = **768** | One 10 Gb/s link to one leaf per server |

A natural interpretation of the specified fully connected leaf-to-core arrangement is one 40 Gb/s uplink from every leaf to each of the 12 core switches: 24 × 12 = 288 leaf-to-core links, using all 24 ports of each core switch. **This wiring pattern is a derived implementation interpretation, not an additional explicit sentence in the assignment.**

The leaf's nominal total uplink bandwidth is 12 × 40 = 480 Gb/s and total server-facing bandwidth is 32 × 10 = 320 Gb/s. This configuration is not oversubscribed at the leaf's aggregate fabric uplink, although individual link bottlenecks can still arise.

Conceptual topology:

```text
                  12 core switches
           C0   C1   C2 ...       C11
            |\   |\  /|             |
            | \  | \/ |  ...        |
          24 leaf switches (L0 ... L23)
            |        |             |
          32 hosts 32 hosts      32 hosts
             each attached at 10 Gb/s
Leaf-to-core links: 40 Gb/s
```

## C. Congestion feedback: end-to-end logic

**State primarily lives at the leaf switches.** The source leaf chooses its uplink. Core switches update packet-carried path congestion information as packets traverse links.

The relevant CONGA metadata fields are:

| Field | Meaning | Role |
|---|---|---|
| `LBTag` | Source-leaf uplink identifier | Identifies which outgoing path's congestion is being measured |
| `CE` | Congestion encountered along the forward path | Maximum congestion metric among traversed fabric links |
| `FB_LBTag` | Uplink identifier for a feedback entry | Tells the source leaf which path the feedback describes |
| `FB_Metric` | Congestion value for that feedback entry | Updates the source leaf's remote congestion estimate |

A packet's path congestion is accumulated as:

\[
CE \leftarrow \max(CE, M_{\text{link}})
\]

At the start, `CE = 0`. At each traversed link, compare the packet's `CE` to that link's congestion metric and retain the larger value. This measures the *worst* congested link along the path.

### Step-by-step forward and return path

1. **Source leaf A** has data for a host under **destination leaf B**.
2. A consults its local congestion estimate and its **Congestion-To-Leaf** entries for B. It selects an uplink.
3. A stamps `LBTag = chosen uplink` and initializes `CE = 0`.
4. Each relevant fabric link updates `CE = max(CE, measured_link_congestion)`.
5. **Destination leaf B** receives the packet, then records its `CE` under `(source leaf A, LBTag)` in **Congestion-From-Leaf**.
6. On a reverse-direction packet, B piggybacks one stored feedback entry using `FB_LBTag` and `FB_Metric`.
7. **Leaf A** receives the reverse packet and updates its **Congestion-To-Leaf** entry for `(destination leaf B, uplink)`.
8. Subsequent path selections have fresher knowledge of congestion beyond A's local uplink.

```text
Forward:
  Host -> Leaf A -> [selected uplink] -> Core -> Leaf B -> Host
             LBTag=u, CE=0  ── CE=max(CE, link metric) ───>

Reverse:
  Host <- Leaf A <- Core <- Leaf B <- Host
             <── FB_LBTag=u, FB_Metric=observed CE
```

**Feedback is distributed and opportunistic:** the published CONGA design piggybacks feedback on reverse-direction traffic. The paper mentions sending metrics in round-robin order while favoring changed metrics, and aging stale entries (e.g., gradually decaying after approximately 10 ms without an update). If implementing aging or special handling for no reverse traffic, state your choice clearly in the README/report.

## D. The two congestion tables

Do not confuse **what was observed** with **what was reported back**:

| Table | Located at | Indexed by | Contents |
|---|---|---|---|
| **Congestion-To-Leaf** | Source leaf | Destination leaf + local uplink (`LBTag`) | Latest feedback describing that path's remote congestion |
| **Congestion-From-Leaf** | Destination leaf | Source leaf + source's `LBTag` | Path congestion observed on arriving packets, awaiting return feedback |

Suppose leaf A is sending to leaf B. Its `Congestion-To-Leaf[B][u]` estimates how congested the remote route to B is when A sends using uplink `u`.

Meanwhile B's `Congestion-From-Leaf[A][u]` holds congestion observations for incoming traffic from A that entered the fabric through uplink `u`. B returns those measurements to A.

These are *per-leaf*, not per-TCP-connection tables. A useful proposed data structure for this specific lab is an array or vector indexed by leaf ID and uplink ID. Pick a documented initialization policy for entries that have no feedback yet.

## E. How the source leaf chooses the uplink

For destination leaf `d`, let:

- \(L_u\) be the source leaf's **local** congestion estimate on uplink \(u\).
- \(T_{d,u}\) be the source leaf's latest **remote** congestion estimate from `Congestion-To-Leaf[d][u]`.

The source leaf compares:

\[
\boxed{M_{d,u}=\max(L_u,T_{d,u})}
\]

and chooses:

\[
\boxed{u^*=\arg\min_{u\in U} M_{d,u}}
\]

Here \(U\) is the set of eligible operational uplinks. The `max` represents the estimated bottleneck congestion along the candidate path; the `argmin` selects the least congested option.

Example (illustrative quantized metrics):

| Uplink | Local \(L_u\) | Remote \(T_{d,u}\) | Combined \(M_{d,u}\) |
|---|---:|---:|---:|
| 0 | 2 | 5 | 5 |
| 1 | 4 | 1 | 4 |
| 2 | 1 | 3 | **3** |

**Select uplink 2**, assuming all paths are eligible. If two uplinks tie, choose and document a tie-break policy (e.g., pseudo-random among equal minima) so results are reproducible.

**Why both metrics?** The source's local link may already be congested even if the remote path is clear; alternatively, the local uplink can be clear while a downstream fabric link is congested. Using just one would miss the other bottleneck.

## F. Congestion measurement using DRE

The CONGA paper proposes a **Discounting Rate Estimator (DRE)** at fabric links. Conceptually it estimates recent offered traffic by maintaining a counter \(X\) that:

1. Increases on packet arrivals by the packet size (in consistent units).
2. Decays exponentially as simulation time passes.
3. Is normalized by link rate \(C\) and time constant \(\tau\).
4. Is quantized into a small congestion metric used in packet headers and tables.

A useful continuous-time conceptual approximation is:

\[
X(t+\Delta t)\approx X(t)e^{-\Delta t/\tau}
\]

between arrivals, plus packet-size increments at arrivals. The rate estimate is proportional to \(X/\tau\), and the normalized utilization-like metric is \(X/(C\tau)\).

**Important implementation caution:** The equation above explains the filter, but is **not a claim that the paper mandates this exact discrete update code**. Check the paper's DRE subsection and the skeleton's time units before choosing an event-based approximation. The paper's reference discussion gives a time constant around \(\tau=160\,\mu s\) and a 3-bit congestion measure; these are *paper reference settings*, not additional Lab 2 requirements.

### htsim-specific implication

Because htsim is discrete-event based, your DRE update should be based on **simulation timestamps**, not wall-clock time.

A possible organization:

```text
When a packet arrives on a monitored fabric link:
  decay estimated load using time since previous update
  increment estimated load by arriving packet size
  compute the normalized congestion metric
  use that metric to update the packet's CE field
```

This is pseudocode guidance, **not an assignment solution**. Determine the actual event hooks, units, and link direction semantics by reading the skeleton.

## G. Flowlets: in the CONGA paper, optional in this lab

A **flowlet** is a burst of packets within one TCP flow, separated from other bursts by an inactivity gap. CONGA normally chooses a path at the *start* of each flowlet; all packets in the flowlet stay on the selected uplink. This allows more path flexibility than per-flow ECMP without creating arbitrary per-packet reordering.

The paper discusses a 500 μs flowlet gap in its evaluation. **However, the lab explicitly says flowlet implementation is not required.** Treating an entire simulated flow as one flowlet is sufficient for the assignment.

Therefore:

- Make a congestion-aware path selection once at the start of each flow under the simplified interpretation.
- Keep that path consistent for the flow unless the lab skeleton or your documented implementation choices require something else.
- Do **not** spend time implementing flowlet timeout detection unless you intentionally add it as an extension.

Likewise, the lab waives the requirement to implement literal VXLAN header layout. Carry the necessary metadata in whatever packet representation is suitable for the simulator, with clear documentation.

## H. Suggested htsim implementation structure (not prescribed)

| Component | Responsibilities to investigate |
|---|---|
| Network topology/testbed | Construct 12 cores, 24 leaves, 768 host-facing attachments, and 40/10 Gb/s links |
| `LeafSwitch` (suggested) | Maintain `Congestion-To-Leaf`, `Congestion-From-Leaf`, choose uplink and process feedback |
| Link congestion tracker | Maintain a DRE-like estimate for each measured fabric link |
| Packet metadata | Carry `LBTag`, `CE`, `FB_LBTag`, `FB_Metric`, and any validity flags |
| Routing decision | Evaluate `min_u max(local, remote)` for a new flow |
| Flow generator | Produce workload-specific flows and offered loads |
| ECMP baseline | Choose a stable path by flow hash, without congestion feedback |
| Metrics/reporting | Record each flow's start time, completion time, size, and trial configuration |

Possible implementation order:

1. Build and validate the topology using `Test_single_link.cpp` and `Test_fat-tree_testbed.cpp`.
2. Check reachability and link capacities before introducing dynamic load balancing.
3. Create an ECMP baseline for correctness and comparison.
4. Add packet metadata and local congestion estimation.
5. Add the two per-leaf congestion tables.
6. Implement forward-path congestion accumulation and reverse-path feedback.
7. Add the congestion-aware uplink selection decision.
8. Test path choice with controlled congestion before running full workloads.
9. Run and record repeatable workload comparisons.

### Correctness checks worth performing

- Every server is attached to one and only one leaf.
- Every leaf has the intended 12 core-facing and 32 host-facing links.
- A packet's `CE` never decreases while moving along its forward path.
- Feedback from leaf B updates leaf A's correct destination/uplink entry.
- The selected uplink minimizes the computed `max(local, remote)` metric.
- The ECMP baseline and CONGA use identical topology, traffic, and simulator parameters.
- Results are recorded in **simulation time**, not host-clock time.

## I. Performance report: experimental matrix

The original lab calls for **4 workload types × 3 flow-size categories = 12 figures**. Each figure compares **ECMP vs. CONGA** as load varies across:

`10%, 20%, 30%, 40%, 50%, 60%, 70%, 80%, 90%, 100%`.

| Workload | All flows | Small flows (<100 KB) | Large flows (≥100 KB) |
|---|---|---|---|
| Uniform | 1 figure | 1 figure | 1 figure |
| Pareto | 1 figure | 1 figure | 1 figure |
| Enterprise | 1 figure | 1 figure | 1 figure |
| Data mining | 1 figure | 1 figure | 1 figure |

Each plot:

- **X-axis:** offered load (%), 10–100.
- **Y-axis:** average flow completion time (FCT).
- **Series:** ECMP and CONGA for the same workload and flow-size category.

FCT for a flow is:

\[
FCT_i=t_{\mathrm{finish},i}-t_{\mathrm{start},i}
\]

The average for a category containing \(n\) completed flows is:

\[
\overline{FCT}=\frac{1}{n}\sum_{i=1}^{n}FCT_i
\]

Define precisely how simulator time, load, flow start/completion, and incomplete flows are handled. The assignment specifies **average FCT**, not p99 as a required chart (although tail latency may be an interesting optional discussion).

### What to discuss when comparing ECMP and CONGA

- **ECMP collisions:** Fixed flow-to-path hashes can overload some paths and leave others idle.
- **Feedback benefit:** CONGA can direct new flows toward paths with lower estimated bottleneck congestion.
- **Feedback staleness:** Changes in load take time to be reflected in source-leaf tables.
- **Granularity caveat:** The lab's flow-as-flowlet simplification limits CONGA's ability to redirect a long active flow. In the full paper, new flowlets can move within a TCP flow.
- **Workload effects:** A heavy-tailed distribution with large flows can amplify persistent ECMP collisions; very short flows may complete before congestion feedback is useful.
- **Fair methodology:** Control the load definition, topology, workload seed, link capacities, transport settings, and observation window.

Do not assume CONGA must outperform ECMP on every test. Report observed results and explain the traffic distribution, measurement methodology, and congestion dynamics.

## J. Submission checklist (from the original assignment)

- [ ] Working htsim implementation with the required topology and CONGA features.
- [ ] ECMP comparison using the same network and workloads.
- [ ] All 12 average-FCT plots, covering 10–100% offered load.
- [ ] `report.pdf` with results and explanations.
- [ ] README listing group members, CS usernames, assumptions, and run instructions.
- [ ] Source code and necessary build files inside the `htsim` submission directory.
- [ ] One submission tarball named for group member CS username(s).
- [ ] Submit through the Lab 2 Canvas tab by **Nov 6, 2026, 11:59 PM** (subject to the assignment's late policy).

