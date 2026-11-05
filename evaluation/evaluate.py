#!/usr/bin/env python3
"""Run paired ECMP/CONGA sweeps and plot completed-flow FCT without dependencies."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed, CancelledError
import csv
import html
import json
import math
from pathlib import Path
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
WORKLOADS = ("uniform", "pareto", "enterprise", "datamining")
MODES = ("ecmp", "conga")
CATEGORIES = ("all", "small", "large")
FIELDS = ("mode", "workload", "load", "seed", "routing_seed", "flow_id", "source",
          "destination", "size_bytes", "start_us", "finish_us", "fct_us")


def positive_int(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def duration_value(value):
    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0 or seconds >= ((1 << 63) - 1) / 1e12:
        raise argparse.ArgumentTypeError("must be positive, finite, and fit signed 64-bit picoseconds")
    if math.floor(seconds * 1e12 + 0.5) <= 1000001:
        raise argparse.ArgumentTypeError("must extend past the simulator's 1 us flow-generation start")
    return seconds


def seed_value(value):
    number = int(value)
    if not 0 <= number <= 0xffffffff:
        raise argparse.ArgumentTypeError("must be an unsigned 32-bit integer")
    return number


def positive_uint32(value):
    number = positive_int(value)
    if number > 0xffffffff:
        raise argparse.ArgumentTypeError("must fit an unsigned 32-bit integer")
    return number


def load_value(value):
    number = float(value)
    if not math.isfinite(number) or not 0 < number <= 1:
        raise argparse.ArgumentTypeError("must be in (0, 1]")
    return number


def finite(value, label):
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"Non-finite {label}")
    return number


def read_run(path):
    """Stream a run's CSV; retain only category sums/counts, not all flow rows."""
    path = Path(path)
    metadata = json.loads(path.read_text())
    if metadata.get("schema_version") != 1:
        raise ValueError(f"Unsupported run schema: {path}")
    config, counts = metadata["config"], metadata["counts"]
    mode, workload = config["mode"], config["workload"]
    if mode not in MODES or workload not in WORKLOADS:
        raise ValueError(f"Unknown mode/workload in {path}")
    load = finite(config["load"], "load")
    if not 0 < load <= 1:
        raise ValueError(f"Invalid load in {path}")
    generated, completed, incomplete = (int(counts[key]) for key in
                                         ("generated", "completed", "incomplete"))
    if min(generated, completed, incomplete) < 0 or generated != completed + incomplete:
        raise ValueError(f"Inconsistent flow counts in {path}")
    if int(counts["route_violations"]) != 0:
        raise ValueError(f"Route violations in {path}; refusing to plot invalid results")
    end_time = finite(metadata["end_time_us"], "run end time")
    hosts = int(config["leaves"]) * int(config["servers_per_leaf"])
    totals = {category: [0, 0.0] for category in CATEGORIES}
    seen_ids = set()
    csv_path = path.with_name(path.name.removesuffix(".run.json") + ".flows.csv")
    with csv_path.open(newline="") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames != list(FIELDS):
            raise ValueError(f"Unexpected completed-flow columns: {csv_path}")
        for row in reader:
            flow_id = int(row["flow_id"])
            if not 0 <= flow_id < generated or flow_id in seen_ids:
                raise ValueError(f"Duplicate/out-of-range completed-flow ID: {csv_path}")
            seen_ids.add(flow_id)
            source, destination = int(row["source"]), int(row["destination"])
            if not 0 <= source < hosts or not 0 <= destination < hosts or source == destination:
                raise ValueError(f"Invalid flow endpoints: {csv_path}")
            if (row["mode"] != mode or row["workload"] != workload or
                    not math.isclose(finite(row["load"], "flow load"), load) or
                    int(row["seed"]) != int(config["seed"]) or
                    int(row["routing_seed"]) != int(config["routing_seed"])):
                raise ValueError(f"Flow metadata disagrees with run configuration: {csv_path}")
            size = int(row["size_bytes"])
            start, finish, fct = (finite(row[key], key) for key in
                                   ("start_us", "finish_us", "fct_us"))
            if (size <= 0 or start < 0 or finish < start or finish > end_time + 1e-6 or
                    fct < 0 or not math.isclose(
                    finish - start, fct, rel_tol=1e-8, abs_tol=1e-6)):
                raise ValueError(f"Invalid completed-flow size/timing: {csv_path}")
            category = "small" if size < 100000 else "large"
            for key in ("all", category):
                totals[key][0] += 1
                totals[key][1] += fct
    if totals["all"][0] != completed:
        raise ValueError(f"Completed CSV count differs from run metadata: {csv_path}")
    return config, counts, totals


def aggregate(directory):
    records = sorted(Path(directory).rglob("*.run.json"))
    if not records:
        raise ValueError(f"No .run.json files in {directory}")
    groups, identities = {}, set()
    shared = {}
    # Mixing durations/topologies/transports into one comparison is misleading.
    invariant_keys = ("experiment", "duration_seconds", "flowsize_bytes", "endhost", "queue",
                      "small", "cores", "leaves", "servers_per_leaf", "server_link_bps",
                      "fabric_link_bps", "link_delay_us", "dre_interval_us", "dre_alpha", "dre_levels")
    paired = {}
    for path in records:
        config, counts, totals = read_run(path)
        invariants = {key: config[key] for key in invariant_keys}
        workload = config["workload"]
        if workload not in shared:
            shared[workload] = invariants
        elif invariants != shared[workload]:
            raise ValueError("Run directory mixes incompatible configurations; use separate outputs")
        identity = (config["workload"], float(config["load"]), int(config["seed"]),
                    int(config["routing_seed"]))
        if (identity, config["mode"]) in identities:
            raise ValueError(f"Duplicate run identity: {path}")
        identities.add((identity, config["mode"]))
        paired.setdefault(identity, {})[config["mode"]] = int(counts["generated"])
        for category in CATEGORIES:
            key = (config["workload"], config["mode"], float(config["load"]), category)
            group = groups.setdefault(key, {"runs": 0, "nonempty_runs": 0, "flows": 0,
                "sum_fct": 0.0, "sum_run_means": 0.0, "generated": 0, "completed": 0,
                "incomplete": 0})
            n, total = totals[category]
            group["runs"] += 1
            group["flows"] += n
            group["sum_fct"] += total
            if n:
                group["sum_run_means"] += total / n
                group["nonempty_runs"] += 1
            for label in ("generated", "completed", "incomplete"):
                group[label] += int(counts[label])
    for identity, pair in paired.items():
        if set(pair) != set(MODES) or pair["ecmp"] != pair["conga"]:
            raise ValueError(f"Missing paired mode or mismatched generated demands: {identity}")
    results = []
    for key, group in sorted(groups.items()):
        row = dict(zip(("workload", "mode", "load", "category"), key))
        row.update({label: group[label] for label in
                    ("runs", "nonempty_runs", "flows", "generated", "completed", "incomplete")})
        row["mean_fct_us"] = group["sum_fct"] / group["flows"] if group["flows"] else None
        row["mean_run_fct_us"] = (group["sum_run_means"] / group["nonempty_runs"]
                                   if group["nonempty_runs"] else None)
        row["completion_fraction"] = (group["completed"] / group["generated"]
                                       if group["generated"] else None)
        results.append(row)
    return results


def topology_description(config):
    label = "Small debug" if config["small"] else "Full"
    if config.get("experiment") == 4:
        label += " (experiment 4)" if config["small"] else " oversubscribed"
    return (f"{label} topology: {config['cores']} cores, {config['leaves']} leaves, "
            f"{config['servers_per_leaf']} servers/leaf")


def render_svg(rows, workload, category, config=None):
    """Standalone SVG: missing measurements are gaps, never zero-valued FCTs."""
    selected = [r for r in rows if r["workload"] == workload and r["category"] == category]
    values = [r["mean_run_fct_us"] for r in selected if r["mean_run_fct_us"] is not None]
    ymax = max(values) * 1.1 if values and max(values) > 0 else 1.0
    left, right, top, bottom = 90, 710, 90, 420
    def x(load):
        return left + float(load) * (right - left)
    def y(value):
        return bottom - value / ymax * (bottom - top)
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="780" height="520" viewBox="0 0 780 520">',
             '<rect width="100%" height="100%" fill="white"/>',
             '<g font-family="sans-serif" font-size="13" fill="#222">',
             f'<text x="390" y="28" text-anchor="middle" font-size="19">{html.escape(workload)}: {category} flows</text>',
             '<text x="390" y="49" text-anchor="middle">Completed flows; equal-weight mean across nonempty seed runs</text>']
    if config is not None:
        parts.append(f'<text x="390" y="69" text-anchor="middle">{html.escape(topology_description(config))}</text>')
    for i in range(11):
        load = i / 10
        parts.append(f'<line x1="{x(load):.2f}" y1="{top}" x2="{x(load):.2f}" y2="{bottom}" stroke="#eee"/>')
        parts.append(f'<text x="{x(load):.2f}" y="442" text-anchor="middle">{i * 10}%</text>')
    for i in range(6):
        value = ymax * i / 5
        parts.append(f'<line x1="{left}" y1="{y(value):.2f}" x2="{right}" y2="{y(value):.2f}" stroke="#ddd"/>')
        parts.append(f'<text x="80" y="{y(value)+4:.2f}" text-anchor="end">{value:.3g}</text>')
    parts.extend([f'<path d="M{left},{top} V{bottom} H{right}" fill="none" stroke="#222"/>',
                  '<text x="400" y="478" text-anchor="middle">Offered load</text>',
                  '<text transform="translate(24 245) rotate(-90)" text-anchor="middle">Mean FCT (microseconds)</text>'])
    for mode, color in (("ecmp", "#2166ac"), ("conga", "#b2182b")):
        series = sorted((r for r in selected if r["mode"] == mode), key=lambda r: r["load"])
        segment = []
        for row in series + [{"mean_run_fct_us": None}]:
            value = row["mean_run_fct_us"]
            if value is None:
                if segment:
                    parts.append(f'<polyline points="{" ".join(segment)}" fill="none" stroke="{color}" stroke-width="2"/>')
                    segment = []
                continue
            px, py = x(row["load"]), y(value)
            segment.append(f"{px:.2f},{py:.2f}")
            parts.append(f'<circle cx="{px:.2f}" cy="{py:.2f}" r="4" fill="{color}"/>')
        lx = 270 if mode == "ecmp" else 410
        parts.append(f'<line x1="{lx}" y1="500" x2="{lx+25}" y2="500" stroke="{color}" stroke-width="3"/>')
        parts.append(f'<text x="{lx+33}" y="504">{mode.upper()}</text>')
    if not values:
        parts.append('<text x="400" y="240" text-anchor="middle">No completed flows in this category</text>')
    parts.append('</g></svg>')
    return "\n".join(parts) + "\n"


def write_results(directory):
    directory = Path(directory)
    rows = aggregate(directory)
    configs = {}
    for path in sorted(directory.rglob("*.run.json")):
        config = json.loads(path.read_text())["config"]
        summary = configs.setdefault(config["workload"], dict(config, seeds=set(), routing_seeds=set()))
        summary["seeds"].add(config["seed"])
        summary["routing_seeds"].add(config["routing_seed"])
    with (directory / "aggregate.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    plots = directory / "plots"
    plots.mkdir(exist_ok=True)
    # Remove only filenames generated by this tool, so a narrower replacement
    # sweep cannot leave stale figures that look like part of the new result.
    for workload in WORKLOADS:
        for category in CATEGORIES:
            (plots / f"{workload}-{category}.svg").unlink(missing_ok=True)
    for workload in sorted({row["workload"] for row in rows}):
        for category in CATEGORIES:
            (plots / f"{workload}-{category}.svg").write_text(render_svg(rows, workload, category, configs[workload]))
    (directory / "comparison.md").write_text(comparison_report(rows, configs))
    incomplete = [row for row in rows if row["category"] == "all" and row["incomplete"]]
    if incomplete:
        print("Some flows are incomplete at cutoff; see completion_fraction in aggregate.csv. "
              "FCT means include completed flows only.", file=sys.stderr)
    print(f"Wrote {directory / 'aggregate.csv'} and {plots}")


def comparison_report(rows, configs=None):
    parts = ["# ECMP versus CONGA evaluation", "",
        "Each point is the equal-weight average of completed-flow mean FCTs across nonempty seed runs. "
        "Small flows are <100000 bytes; large flows are >=100000 bytes. "
        "Negative FCT change means CONGA is faster. Missing categories stay blank, not zero.", "",
        "These results exclude incomplete flows. Different completion fractions can bias the comparison "
        "toward faster completed flows, especially at high loads. Inspect raw records and extend the "
        "measurement/drain policy before drawing conclusions from heavily truncated runs.", ""]
    better = worse = equal = 0
    for workload in sorted({r["workload"] for r in rows}):
        parts += [f"## {workload}", ""]
        if configs is not None:
            config = configs[workload]
            hosts = int(config["leaves"]) * int(config["servers_per_leaf"])
            seeds = ", ".join(str(seed) for seed in sorted(config["seeds"]))
            routing_seeds = ", ".join(str(seed) for seed in sorted(config["routing_seeds"]))
            parts += [f"**{topology_description(config)} ({hosts} servers total).** "
                f"Duration: {config['duration_seconds']} simulated seconds; transport: {config['endhost']}; "
                f"queue: {config['queue']}; traffic seeds: {seeds}; routing seeds: {routing_seeds}.", ""]
            if workload == "uniform":
                empty_category = "small" if config["flowsize_bytes"] >= 100000 else "large"
                parts += [f"Uniform traffic has fixed flow size {config['flowsize_bytes']} bytes, "
                    f"so the {empty_category}-flow category is empty by construction. "
                    "Its plot correctly contains no measured FCT points.", ""]
        for category in CATEGORIES:
            lookup = {(r["load"], r["mode"]): r for r in rows
                      if r["workload"] == workload and r["category"] == category}
            parts += [f"### {category} flows", "", f"![{workload} {category}](plots/{workload}-{category}.svg)", "",
                "| Load | ECMP FCT (us) | CONGA FCT (us) | FCT change | ECMP run completion | CONGA run completion |",
                "| --- | ---: | ---: | ---: | ---: | ---: |"]
            for load in sorted({key[0] for key in lookup}):
                ecmp, conga = lookup[(load, "ecmp")], lookup[(load, "conga")]
                emean, cmean = ecmp["mean_run_fct_us"], conga["mean_run_fct_us"]
                change = None if emean is None or cmean is None or emean == 0 else (cmean / emean - 1) * 100
                if change is not None:
                    better += change < -1e-9
                    worse += change > 1e-9
                    equal += abs(change) <= 1e-9
                def number(value, percent=False):
                    if value is None:
                        return "—"
                    return f"{value:.2f}%" if percent else f"{value:.6g}"
                parts.append(f"| {load * 100:g}% | {number(emean)} | {number(cmean)} | {number(change, True)} | "
                             f"{number(None if ecmp['completion_fraction'] is None else ecmp['completion_fraction'] * 100, True)} | "
                             f"{number(None if conga['completion_fraction'] is None else conga['completion_fraction'] * 100, True)} |")
            parts.append("")
    parts += ["## Interpretation", "",
        f"Across measured workload/category/load points, CONGA has lower mean FCT at {better}, "
        f"higher mean FCT at {worse}, and equal mean FCT at {equal}. "
        "All/small/large categories overlap; these counts are descriptive, not independent samples.", "",
        "Possible explanations (inference, not established by FCT alone): lower CONGA FCT may reflect "
        "avoiding ECMP hash collisions or busy uplinks using congestion feedback. Higher CONGA FCT "
        "may reflect feedback lag/staleness, cold-start exploration, or fixed whole-flow routes that "
        "cannot respond to later congestion. At low load, ECMP and CONGA may perform similarly "
        "because links rarely contend. Correlate these hypotheses with decision/link CSVs and repeat "
        "paired seeds before attributing a difference to the algorithm.", ""]
    if configs and any(config["small"] for config in configs.values()):
        parts += ["## Small-topology interpretation", "",
            "The debug topology has two 10 Gbps servers per leaf and two fabric links: "
            "40 Gbps each in experiment 2, or 20 Gbps each in experiment 4. "
            "Even all server ingress at one leaf totals only 20 Gbps, so this debug topology "
            "does not reproduce the full experiment's aggregate fabric oversubscription. "
            "CONGA can change transient fabric queueing, "
            "but it cannot change the selected hosts' shared 10 Gbps access/downlink bottlenecks. "
            "This capacity argument is an architectural explanation for similar ECMP/CONGA FCTs, "
            "not proof of the cause of each measured difference. A single seed cannot establish "
            "that small positive or negative changes are robust. Use the full lab topology and "
            "multiple paired seeds for the submitted performance discussion.", ""]
    return "\n".join(parts)


def reusable_run(path, expected_config):
    """None means unfinished; valid-but-incompatible/broken runs remain errors."""
    path = Path(path)
    if not path.exists():
        return None
    try:
        json.loads(path.read_text())
    except json.JSONDecodeError:
        # The simulator opens metadata on startup and writes it at shutdown.
        # Interrupted runs may leave an empty file or a partial final write.
        return None
    result = read_run(path)
    config = result[0]
    if any(config[key] != value for key, value in expected_config.items()):
        raise ValueError(f"Unexpected simulator configuration: {path}")
    return result


class SimulationRunner:
    """Bound independent simulator processes and stop active children on failure."""
    def __init__(self, jobs, resume=False):
        self.jobs = jobs
        self.resume = resume
        self._lock = threading.Lock()
        self._cancelled = threading.Event()
        self._active = set()

    def run_one(self, command):
        prefix = Path(command["prefix"])
        prefix.parent.mkdir(parents=True, exist_ok=True)
        metadata_path = prefix.with_suffix(".run.json")
        previous = reusable_run(metadata_path, command["expected_config"]) if self.resume else None
        if self._cancelled.is_set():
            raise CancelledError()
        if previous is not None:
            return None
        started = time.perf_counter()
        with prefix.with_suffix(".stdout").open("w") as stdout, prefix.with_suffix(".stderr").open("w") as stderr:
            # Serialize registration with cancellation so no child can start
            # between taking the active-process snapshot and stopping it.
            with self._lock:
                if self._cancelled.is_set():
                    raise CancelledError()
                process = subprocess.Popen(command["argv"], cwd=ROOT, stdout=stdout, stderr=stderr)
                self._active.add(process)
            try:
                returncode = process.wait()
            finally:
                with self._lock:
                    self._active.discard(process)
        if returncode != 0:
            raise subprocess.CalledProcessError(returncode, command["argv"])
        elapsed = time.perf_counter() - started
        config, _, _ = read_run(metadata_path)
        if any(config[key] != value for key, value in command["expected_config"].items()):
            raise ValueError(f"Unexpected simulator configuration: {prefix}")
        prefix.with_suffix(".timing.json").write_text(json.dumps({"wall_seconds": elapsed}, indent=2) + "\n")
        return elapsed

    def stop(self):
        with self._lock:
            self._cancelled.set()
            processes = list(self._active)
        for process in processes:
            try:
                process.terminate()
            except ProcessLookupError:
                pass
        # A child that fails to stop must not hold the executor open forever.
        for process in processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    def run(self, commands, directory):
        executor = ThreadPoolExecutor(max_workers=min(self.jobs, len(commands)))
        futures = {}
        try:
            for command in commands:
                futures[executor.submit(self.run_one, command)] = command
            for completed, future in enumerate(as_completed(futures), 1):
                elapsed = future.result()
                prefix = Path(futures[future]["prefix"]).relative_to(directory)
                status = "reused" if elapsed is None else f"{elapsed:.2f}s"
                print(f"[{completed}/{len(commands)}] {prefix}: {status}", flush=True)
        except BaseException:
            self.stop()
            raise
        finally:
            executor.shutdown(wait=True, cancel_futures=True)


def run_sweep(args):
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    if any(directory.rglob("*.run.json")) and not (args.overwrite or args.resume):
        raise ValueError("Output already contains runs; select another --output, --resume, or --overwrite")
    if args.overwrite:
        # Do not silently combine old workloads/seeds with a new sweep.
        for path in directory.rglob("*.run.json"):
            path.unlink()
    if not args.no_build:
        subprocess.run(["make", "-C", str(ROOT / "htsim")], check=True)
    binary = args.binary.resolve()
    if not binary.is_file():
        raise ValueError(f"Simulator not found: {binary}; run evaluation/run.sh init")
    workloads = WORKLOADS if args.distribution == "all" else (args.distribution,)
    if len(set(args.seeds)) != len(args.seeds) or len(set(args.loads)) != len(args.loads):
        raise ValueError("Seeds and loads must not contain duplicates")
    experiment = 4 if args.oversubscription else 2
    fabric_rate = 20000000000 if args.oversubscription else 40000000000
    commands = []
    for workload in workloads:
        for load in sorted(args.loads):
            for seed in args.seeds:
                for mode in MODES:
                    prefix = directory / "raw" / workload / f"load-{load}" / f"seed-{seed}" / mode
                    flags = {"expt": experiment, "lb": mode, "flowdist": workload, "utilization": load,
                             "rngseed": seed, "routingseed": seed if args.routing_seed is None else args.routing_seed,
                             "duration": args.duration, "flowsize": args.flowsize, "small": int(args.small),
                             "endhost": args.endhost, "queue": args.queue, "verbose": 0, "logfile": prefix}
                    expected = dict(experiment=experiment, fabric_link_bps=fabric_rate,
                        mode=mode, workload=workload, load=load, seed=seed,
                        routing_seed=flags["routingseed"], duration_seconds=args.duration,
                        flowsize_bytes=args.flowsize, small=args.small, endhost=args.endhost, queue=args.queue)
                    commands.append({"prefix": str(prefix), "expected_config": expected, "argv": [str(binary)] +
                                     [f"--{key}={value}" for key, value in flags.items()]})
    manifest = {"schema_version": 1, "jobs": args.jobs, "commands": commands,
                "aggregation": "completed flows only; small <100000 bytes, large >=100000; plots average run means"}
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Evaluating {len(commands)} runs on {'small' if args.small else 'full'} topology "
          f"(experiment {experiment}) "
          f"with up to {args.jobs} parallel jobs", flush=True)
    SimulationRunner(args.jobs, args.resume).run(commands, directory)
    write_results(directory)


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="run paired modes, aggregate CSVs, and generate SVG plots")
    run.add_argument("--distribution", "--workload", choices=(*WORKLOADS, "all"), default="all")
    run.add_argument("--loads", type=load_value, nargs="+", default=[i / 10 for i in range(1, 11)])
    run.add_argument("--seeds", type=seed_value, nargs="+", default=[1729])
    run.add_argument("--routing-seed", type=seed_value)
    run.add_argument("--duration", type=duration_value, default=1.0,
                     help="simulated seconds, including fractions such as 0.1")
    run.add_argument("--flowsize", type=positive_uint32, default=100000, help="bytes; empirical workloads use built-in means")
    run.add_argument("--small", action="store_true", help="debug topology; omit for lab figures")
    run.add_argument("--oversubscription", action="store_true",
                     help="run experiment 4 with 20 Gbps fabric links (full topology: 4:3 oversubscription)")
    run.add_argument("--queue", choices=("droptail", "fq", "afq", "pq", "sfq"), default="droptail")
    run.add_argument("--endhost", choices=("tcp", "dctcp", "timely", "pp"), default="dctcp")
    run.add_argument("--output", type=Path, default=ROOT / "logs" / "evaluation")
    run.add_argument("--binary", type=Path, default=ROOT / "htsim" / "htsim")
    run.add_argument("--no-build", action="store_true")
    run.add_argument("--jobs", type=positive_int, default=1,
                     help="maximum concurrent simulator processes (default: 1)")
    reuse = run.add_mutually_exclusive_group()
    reuse.add_argument("--overwrite", action="store_true", help="replace prior run metadata in this output directory")
    reuse.add_argument("--resume", action="store_true", help="reuse completed runs after validating their configuration")
    existing = sub.add_parser("aggregate", help="regenerate aggregate CSV and plots without simulations")
    existing.add_argument("--input", type=Path, required=True)
    return result


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.command == "run":
            run_sweep(args)
        else:
            write_results(args.input.resolve())
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"Evaluation failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted; child simulations stopped. Use --resume to keep completed runs.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
