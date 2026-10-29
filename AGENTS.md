# Repository Guidelines

## Project Structure & Module Organization

This repository contains a C++11 discrete-event network simulator for the CS740 CONGA lab. All simulator code is under `htsim/`. Core primitives such as packets, queues, pipes, TCP, logging, and event scheduling are split into matching `.h`/`.cpp` pairs. Workload generation lives in `workloads.*` and `flow-generator.*`. Simulation scenarios are in `test_single_link.cpp`, `test_fat-tree_testbed.cpp`, and `test_conga_testbed.cpp`; register new scenarios in `test.h`. Runtime traces are written under `htsim/data/`. The top-level `README` defines the assignment and required experiments.

## Build, Test, and Development Commands

Run commands from the repository root:

```bash
make -C htsim                 # Build htsim/htsim with clang++
./htsim/htsim                 # List available experiment numbers
./htsim/htsim --expt=1 --duration=1 --utilization=0.1
./htsim/htsim --expt=3 --duration=1 --utilization=0.1
make -C htsim clean           # Remove the binary, .obj/, and .d/
```

Arguments use `--name=value`; malformed flags are not supported. Build artifacts belong in `htsim/.obj/` and dependency files in `htsim/.d/`, not beside sources.

## Coding Style & Naming Conventions

Compile cleanly with the existing `-std=c++11 -Wall -Wextra` flags. Follow the surrounding style: four-space indentation, braces on the same line as control statements, and headers paired with implementation files. Use `PascalCase` for simulator classes (`FlowGenerator`), `camelCase` for methods and local functions (`setEndtime`), and uppercase names for topology constants (`N_SERVER`). Add include guards to headers. Keep topology-specific globals inside a namespace such as `conga` or `fat_tree`.

## Testing Guidelines

There is no standalone unit-test framework. Treat experiment runs as regression tests: rebuild, run the relevant experiment with a short duration, confirm the process exits successfully, and inspect `htsim/data/htsim-log.*`. For CONGA work, compare experiment 2 behavior against the ECMP/fat-tree baseline and exercise all four workload values: `uniform`, `pareto`, `enterprise`, and `datamining`.

## Commit & Pull Request Guidelines

Git history is not included in this checkout. Use short, imperative commits such as `Implement leaf congestion feedback`. Keep each commit focused and avoid committing generated logs or build outputs. Pull requests should describe the topology or algorithm change, list exact commands and parameters tested, note performance impact, and attach plots when results or flow-completion times change.
