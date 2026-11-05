# ECMP / CONGA evaluation

Run from the repository root. Python 3.9+ and the existing C++ build tools are
required; plotting uses standalone SVG and needs no pip packages or network
access. Node.js is needed only for the `ecmp` and `output` regression suites.

```bash
bash evaluation/run.sh init
bash evaluation/run.sh list
bash evaluation/run.sh test output conga evaluation
bash evaluation/run.sh test all
```

`init` checks Python and builds `htsim/htsim`. Test suites are selectable by name:
`conga`, `routing`, `ecmp`, `dre`, `leaf`, `metadata`, `events`, `output`, and
`evaluation`. Failures stop the command and return a nonzero exit status.
Set `PYTHON=/path/to/python3` to choose another interpreter.

## Run the comparison

```bash
# Fast development sweep: one distribution, two paired seeds, small topology.
bash evaluation/run.sh evaluate --distribution uniform --small \
  --seeds 1729 12314 --output logs/uniform-small

# Lab matrix: all four workloads, 10%-100% load, both policies, full topology.
# This is 240 simulations with three seeds; it can take a long time.
bash evaluation/run.sh evaluate --distribution all --duration 5 \
  --seeds 1729 12314 42 --jobs 2 --output logs/final

# A focused check with fewer points.
bash evaluation/run.sh evaluate --distribution pareto --small \
  --loads 0.1 0.5 1.0 --output logs/pareto-debug

# Shorter full-topology sweep using fractional simulated seconds.
bash evaluation/run.sh evaluate --distribution all --duration 0.1 \
  --seeds 1729 --jobs 24 --output logs/fractional

# Recompute statistics and plots without rerunning simulations.
bash evaluation/run.sh aggregate --input logs/final
```

Defaults are **all four distributions**, loads **0.1, 0.2, …, 1.0**, ECMP and
CONGA, one seed (`1729`), duration 1 simulated second, DCTCP, droptail, and the
full topology. `--workload` is an alias for `--distribution`. Use `--small` for
development; omit it for the required topology. Duration must be a positive
number of seconds, including fractions such as `0.1` or `0.25` (the cutoff must
extend past the simulator's 1 microsecond flow-generation start).
`--flowsize` sets the uniform/Pareto average in bytes; enterprise
and datamining use their built-in distributions.

Each paired run receives the same traffic and routing seeds. Routing seeds
default to the traffic seed; `--routing-seed 42` fixes that seed separately.
The command builds first; `--no-build` skips that step. `--binary` selects a
different simulator binary. Existing results are protected: choose another
output directory or explicitly use `--overwrite` to replace the sweep.
Use `--resume` with the same options/output directory to validate and reuse
completed runs after an interruption; uncompleted runs are executed again.
Empty or truncated run metadata is treated as an interrupted run and rerun.
Valid metadata with incompatible configuration or inconsistent counts remains
an error rather than being silently replaced.

`--jobs N` runs up to N independent simulator processes simultaneously (default
1). Each run keeps its original seeds and has a separate output prefix; completion
order does not change aggregation. The manifest records the worker count, and
each new successful run gets a `.timing.json` with simulator wall-clock seconds.
Changing `--jobs` is compatible with `--resume`; existing timing records are kept.
One run's failure or Ctrl-C stops active children and cancels queued work.
Completed runs can then be reused with `--resume`.

Start with two workers and measure throughput before increasing it. Parallelism
uses more CPU and memory, and per-run memory grows with generated-flow count,
load, and duration. A machine's logical CPU count alone does not determine a
suitable worker count. Simulation parameters and seeds remain identical with
sequential and parallel execution.

## Outputs and interpretation

The default all-workload sweep produces **12 SVG figures** in `plots/`: three
per workload, for all, small (`<100000` bytes), and large (`>=100000` bytes)
flows. Each figure plots offered load against average FCT in microseconds,
with ECMP and CONGA on the same axes. SVGs open in browsers and can be included
in a report without losing resolution.
Plots label the small/debug or full topology explicitly; `comparison.md` also
records topology dimensions, duration, transport, queue, and paired seeds per
workload. Uniform traffic is fixed-size, so the default 100000-byte setting
has no small flows; that category stays empty rather than inventing points.

`aggregate.csv` includes:

- `mean_fct_us`: pooled mean across completed flows.
- `mean_run_fct_us`: equal-weight mean of each nonempty seed run's completed-flow
  mean; this is the plotted value.
- Completed-flow samples, nonempty-run count, generated/completed/incomplete
  counts, and `completion_fraction`. Counts are run totals, repeated for each
  size category; they are not category-specific generated counts.

Empty categories are missing measurements, never zero FCT. Incomplete flows
are excluded from averages, so comparisons at a busy cutoff can be biased.
Compare completion fractions, inspect raw data, and use multiple paired seeds.
The current simulator has no separate drain window; increasing duration also
continues generating traffic and does not eliminate cutoff censoring.

`comparison.md` embeds the figures and lists per-load measured differences.
Negative FCT change means CONGA is faster. Possible causes are labeled as
inferences: FCT alone does not prove congestion feedback avoided collisions or
that stale feedback caused a slowdown. Use `.decisions.csv` and `.links.csv`
to investigate those explanations. No performance advantage is assumed.

`manifest.json` records exact commands. `raw/` preserves each run's `.flows.csv`,
`.run.json`, other simulator CSVs, logs, stdout, and stderr. Aggregation rejects
missing mode pairs, mismatched generated counts, malformed flow records, route
violations, duplicate runs, and incompatible configurations within a workload.
It never parses the simulator's verbose console text.
