import csv
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import subprocess
import threading
import signal
import sys
import textwrap
import time
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

SPEC = importlib.util.spec_from_file_location("evaluate", Path(__file__).parents[1] / "evaluate.py")
evaluate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(evaluate)


class EvaluationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def run_files(self, mode, seed=1, fcts=(20.0, 60.0), load=0.1, workload="uniform"):
        prefix = self.directory / f"{mode}-{workload}-{load}-{seed}"
        config = dict(experiment=2, mode=mode, workload=workload, load=load, seed=seed,
            routing_seed=42, duration_seconds=1, flowsize_bytes=100000, endhost="dctcp",
            queue="droptail", small=True, cores=2, leaves=2, servers_per_leaf=2,
            server_link_bps=10000000000, fabric_link_bps=40000000000, link_delay_us=0.1,
            dre_interval_us=20, dre_alpha=0.125, dre_levels=8)
        metadata = dict(schema_version=1, config=config, end_time_us=1000000,
                        counts=dict(generated=len(fcts) + 1, completed=len(fcts), incomplete=1, route_violations=0))
        run_path = Path(str(prefix) + ".run.json")
        run_path.write_text(json.dumps(metadata))
        with Path(str(prefix) + ".flows.csv").open("w", newline="") as out:
            writer = csv.DictWriter(out, fieldnames=evaluate.FIELDS)
            writer.writeheader()
            for i, fct in enumerate(fcts):
                writer.writerow(dict(mode=mode, workload=workload, load=load, seed=seed,
                    routing_seed=42, flow_id=i, source=0, destination=3,
                    size_bytes=99999 if i == 0 else 100000,
                    start_us=10, finish_us=10 + fct, fct_us=fct))
        return run_path

    def test_paired_aggregation_categories_and_seed_means(self):
        self.run_files("ecmp", seed=1, fcts=(20, 60))
        self.run_files("ecmp", seed=2, fcts=(100,))
        self.run_files("conga", seed=1, fcts=(10, 40))
        self.run_files("conga", seed=2, fcts=(50,))
        rows = evaluate.aggregate(self.directory)
        all_ecmp = next(r for r in rows if r["mode"] == "ecmp" and r["category"] == "all")
        self.assertEqual(all_ecmp["flows"], 3)
        self.assertEqual(all_ecmp["mean_fct_us"], 60)
        self.assertEqual(all_ecmp["mean_run_fct_us"], 70)
        self.assertEqual(all_ecmp["completion_fraction"], 3 / 5)
        large_ecmp = next(r for r in rows if r["mode"] == "ecmp" and r["category"] == "large")
        self.assertEqual(large_ecmp["flows"], 1)  # Exactly 100000 is large.
        self.assertEqual(large_ecmp["nonempty_runs"], 1)
        self.assertEqual(large_ecmp["mean_run_fct_us"], 60)
        evaluate.write_results(self.directory)
        self.assertEqual(len(list((self.directory / "plots").glob("*.svg"))), 3)
        for graph in (self.directory / "plots").glob("*.svg"):
            ET.parse(graph)
        report = (self.directory / "comparison.md").read_text()
        self.assertIn("inference", report)
        self.assertIn("incomplete", report)
        self.assertIn("-50.00%", report)
        self.assertIn("Small debug topology: 2 cores, 2 leaves, 2 servers/leaf", report)
        self.assertIn("traffic seeds: 1, 2", report)
        self.assertIn("small-flow category is empty by construction", report)
        graph = (self.directory / "plots" / "uniform-all.svg").read_text()
        self.assertIn("Small debug topology", graph)

    def test_explicit_full_topology_label(self):
        rows = [{"workload": "uniform", "category": "all", "mode": mode,
                 "load": 0.1, "mean_run_fct_us": 20} for mode in evaluate.MODES]
        graph = evaluate.render_svg(rows, "uniform", "all", dict(
            small=False, cores=12, leaves=24, servers_per_leaf=32))
        self.assertIn("Full topology: 12 cores, 24 leaves, 32 servers/leaf", graph)

    def test_all_workloads_produce_twelve_plots(self):
        for workload in evaluate.WORKLOADS:
            for mode in evaluate.MODES:
                self.run_files(mode, workload=workload)
        evaluate.write_results(self.directory)
        self.assertEqual(len(list((self.directory / "plots").glob("*.svg"))), 12)

    def test_narrower_results_remove_stale_generated_plots(self):
        for mode in evaluate.MODES:
            self.run_files(mode)
        plots = self.directory / "plots"
        plots.mkdir()
        (plots / "pareto-all.svg").write_text("old result")
        (plots / "my-notes.svg").write_text("user file")
        evaluate.write_results(self.directory)
        self.assertFalse((plots / "pareto-all.svg").exists())
        self.assertTrue((plots / "my-notes.svg").exists())

    def test_invalid_id_endpoints_and_cutoff_are_rejected(self):
        for key, value in (("flow_id", "999"), ("source", "3"), ("destination", "4"),
                           ("finish_us", "1000001")):
            with self.subTest(key=key):
                path = self.run_files("ecmp")
                flow_path = path.with_name(path.name.removesuffix(".run.json") + ".flows.csv")
                with flow_path.open(newline="") as source:
                    rows = list(csv.DictReader(source))
                rows[0][key] = value
                with flow_path.open("w", newline="") as output:
                    writer = csv.DictWriter(output, fieldnames=evaluate.FIELDS)
                    writer.writeheader()
                    writer.writerows(rows)
                with self.assertRaises(ValueError):
                    evaluate.read_run(path)

    def test_empty_categories_are_missing_not_zero(self):
        for mode in evaluate.MODES:
            self.run_files(mode, fcts=())
        rows = evaluate.aggregate(self.directory)
        self.assertTrue(all(r["mean_fct_us"] is None for r in rows))
        svg = evaluate.render_svg(rows, "uniform", "small")
        self.assertIn("No completed flows", svg)
        self.assertNotIn("<circle", svg)

    def test_missing_pair_is_rejected(self):
        self.run_files("ecmp")
        with self.assertRaisesRegex(ValueError, "Missing paired"):
            evaluate.aggregate(self.directory)

    def test_count_mismatch_and_route_violations_are_rejected(self):
        path = self.run_files("ecmp")
        metadata = json.loads(path.read_text())
        metadata["counts"].update(completed=1, incomplete=2)
        path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "CSV count"):
            evaluate.read_run(path)
        metadata["counts"]["route_violations"] = 1
        path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "Route violations"):
            evaluate.read_run(path)

    def test_resume_retries_empty_and_truncated_metadata(self):
        path = self.directory / "interrupted.run.json"
        self.assertIsNone(evaluate.reusable_run(path, {}))
        for text in ("", "  \n", '{"schema_version": 1, "config":', '{"schema_version": 1'):
            with self.subTest(text=text):
                path.write_text(text)
                self.assertIsNone(evaluate.reusable_run(path, {}))

    def test_resume_still_rejects_valid_but_incompatible_or_corrupt_runs(self):
        path = self.run_files("ecmp")
        self.assertIsNotNone(evaluate.reusable_run(path, {"mode": "ecmp"}))
        with self.assertRaisesRegex(ValueError, "Unexpected simulator configuration"):
            evaluate.reusable_run(path, {"mode": "conga"})
        metadata = json.loads(path.read_text())
        metadata["counts"]["completed"] = 99
        path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "Inconsistent flow counts"):
            evaluate.reusable_run(path, {"mode": "ecmp"})

    def test_default_matrix_and_invalid_load(self):
        args = evaluate.parser().parse_args(["run"])
        self.assertEqual(args.distribution, "all")
        self.assertEqual(args.loads, [i / 10 for i in range(1, 11)])
        self.assertEqual(args.jobs, 1)
        self.assertFalse(args.oversubscription)
        self.assertTrue(evaluate.parser().parse_args(["run", "--oversubscription"]).oversubscription)
        self.assertEqual(evaluate.parser().parse_args(["run", "--jobs", "4"]).jobs, 4)
        for value in ("0", "-1"):
            with self.assertRaises(Exception):
                evaluate.positive_int(value)
        for value in ("0", "1.01", "nan", "inf"):
            with self.assertRaises(Exception):
                evaluate.load_value(value)
        with self.assertRaises(Exception):
            evaluate.positive_uint32("4294967296")

    def test_fractional_durations_and_resume_configuration(self):
        for value in ("0.1", "0.25", "0.001", "2e-6", "1"):
            args = evaluate.parser().parse_args(["run", "--duration", value])
            self.assertEqual(args.duration, float(value))
        for value in ("0", "-1", "nan", "inf", "1e20", "0.000001", "0.1junk"):
            with self.subTest(value=value), self.assertRaises((ValueError, evaluate.argparse.ArgumentTypeError)):
                evaluate.duration_value(value)
        metadata = self.run_files("ecmp")
        data = json.loads(metadata.read_text())
        data['config']['duration_seconds'] = 0.1
        data['end_time_us'] = 100000
        metadata.write_text(json.dumps(data))
        self.assertIsNotNone(evaluate.reusable_run(metadata, {"duration_seconds": 0.1}))
        with self.assertRaisesRegex(ValueError, "Unexpected simulator configuration"):
            evaluate.reusable_run(metadata, {"duration_seconds": 1})

    def test_oversubscription_metadata_and_resume_isolation(self):
        path = self.run_files("ecmp")
        with self.assertRaisesRegex(ValueError, "Unexpected simulator configuration"):
            evaluate.reusable_run(path, {"experiment": 4, "fabric_link_bps": 20000000000})
        config = dict(small=False, experiment=4, cores=12, leaves=24, servers_per_leaf=32)
        self.assertIn('oversubscribed', evaluate.topology_description(config))
        config['small'] = True
        self.assertIn('experiment 4', evaluate.topology_description(config))

    def test_parallel_worker_limit_and_timing_records(self):
        barrier = threading.Barrier(2)
        lock = threading.Lock()
        active = maximum = 0

        class Process:
            def __init__(self, *args, **kwargs):
                nonlocal active, maximum
                with lock:
                    active += 1
                    maximum = max(maximum, active)

            def wait(self):
                nonlocal active
                barrier.wait(timeout=5)
                with lock:
                    active -= 1
                return 0

        commands = [dict(prefix=str(self.directory / str(i)), argv=["fake"], expected_config={})
                    for i in range(4)]
        with patch.object(evaluate.subprocess, "Popen", Process), \
                patch.object(evaluate, "read_run", return_value=({}, {}, {})):
            evaluate.SimulationRunner(2).run(commands, self.directory)
        self.assertEqual(maximum, 2)
        self.assertEqual(active, 0)
        for command in commands:
            timing = Path(command["prefix"]).with_suffix(".timing.json")
            self.assertGreaterEqual(json.loads(timing.read_text())["wall_seconds"], 0)

    def test_parallel_failure_stops_running_children(self):
        slow_started = threading.Event()
        terminated = threading.Event()

        class Process:
            def __init__(self, argv, **kwargs):
                self.failure = argv[0] == "fail"
                if not self.failure:
                    slow_started.set()

            def wait(self, timeout=None):
                if self.failure:
                    if not slow_started.wait(timeout=5):
                        raise RuntimeError("Second worker never started")
                    return 7
                if not terminated.wait(timeout=timeout or 10):
                    raise subprocess.TimeoutExpired("slow", timeout)
                return -15

            def terminate(self):
                terminated.set()

        commands = [dict(prefix=str(self.directory / name), argv=[name], expected_config={})
                    for name in ("fail", "slow")]
        runner = evaluate.SimulationRunner(2)
        with patch.object(evaluate.subprocess, "Popen", Process):
            with self.assertRaises(subprocess.CalledProcessError):
                runner.run(commands, self.directory)
        self.assertTrue(terminated.is_set(), "Failure left a sibling process running")
        self.assertFalse(runner._active)

    def test_parallel_resume_preserves_existing_timing(self):
        metadata = self.run_files("ecmp")
        prefix = self.directory / "cached"
        prefix.with_suffix(".run.json").write_text(metadata.read_text())
        original_flows = metadata.with_name(metadata.name.removesuffix(".run.json") + ".flows.csv")
        prefix.with_suffix(".flows.csv").write_text(original_flows.read_text())
        timing = prefix.with_suffix(".timing.json")
        timing.write_text('{"wall_seconds": 123}\n')
        command = dict(prefix=str(prefix), argv=["must-not-run"], expected_config={"mode": "ecmp"})
        with patch.object(evaluate.subprocess, "Popen", side_effect=AssertionError("Reran completed simulation")):
            self.assertIsNone(evaluate.SimulationRunner(2, resume=True).run_one(command))
        self.assertEqual(json.loads(timing.read_text())["wall_seconds"], 123)

    @unittest.skipUnless(os.name == "posix", "Requires POSIX process signals")
    def test_parallel_interrupt_stops_child_processes(self):
        binary = self.directory / "slow-simulator"
        binary.write_text(f"#!{sys.executable}\n" + textwrap.dedent('''\
            from pathlib import Path
            import os, sys, time
            args = dict(arg[2:].split('=', 1) for arg in sys.argv[1:])
            prefix = Path(args['logfile'])
            prefix.with_suffix('.run.json').write_text('')
            prefix.with_suffix('.pid').write_text(str(os.getpid()))
            time.sleep(60)
            '''))
        binary.chmod(0o755)
        output = self.directory / "interrupted"
        process = subprocess.Popen([sys.executable, evaluate.__file__, "run", "--no-build",
            "--binary", str(binary), "--distribution", "uniform", "--loads", "0.1",
            "--jobs", "2", "--small", "--output", str(output)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
        pids = []
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                files = list(output.rglob("*.pid"))
                if len(files) == 2:
                    pids = [int(path.read_text()) for path in files]
                    break
                time.sleep(0.02)
            self.assertEqual(len(pids), 2, "Workers did not start concurrently")
            process.send_signal(signal.SIGINT)
            _, stderr = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 130, stderr)
            self.assertIn("child simulations stopped", stderr)
            for pid in pids:
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
        finally:
            # The separate session contains only this test's simulator children.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)


if __name__ == "__main__":
    unittest.main()
