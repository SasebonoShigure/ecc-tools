"""Behavioral contracts for performance orchestration and diagnostic rejection."""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import dual_csr_timing_smoke as smoke
from dual_csr_timing_evidence import DURATION_KEYS, validate_batch_phase_timings


class PerformanceModeTest(unittest.TestCase):
    def test_sample_starts_only_the_target_workload(self):
        phases = [
            {**dict.fromkeys(DURATION_KEYS, 0.0),
             "generation_before": i, "generation_after": i + 1,
             "session_step_ms": 1.0, "query_materialization_ms": 2.0,
             "report_ms": 3.0, "consumer_step_ms": 6.5,
             "invariant_ms": "not_run", "verify_ms": "not_run"}
            for i in range(2)
        ]
        result = {
            "schema_version": "ista-incregpusta-performance-process/3.0.0",
            "debug_checks_enabled": False,
            "correctness_comparison": "not_run",
            "diagnostics": "not_run",
            "result_requirements": {
                "timing_values": True, "summary": True, "timing_paths": True,
                "file_artifacts": False,
            },
            "input": {"node_slots": 2, "arc_slots": 1, "batches": 2},
            "requested_mode": "full_stage5",
            "executed_mode": "full_stage5",
            "result": {"generation": 2, "lifecycle_cleanup_completed": True},
            "run": {"child_pid": 123, "batch_phase_timings": phases},
        }
        with patch.object(smoke, "run_child", return_value=(result, 1.0)) as child:
            sample = smoke._run_sample(
                pathlib.Path("probe"),
                pathlib.Path("trace"),
                [{}, {}],
                pathlib.Path("runs"),
                "sample",
                10,
            )
        child.assert_called_once_with(
            pathlib.Path("probe"),
            "performance",
            pathlib.Path("trace"),
            2,
            pathlib.Path("runs/sample/performance.json"),
            10,
        )
        self.assertEqual(sample["correctness_comparison"], "not_run")

    def test_consumer_interval_and_diagnostic_canaries(self):
        phase = {
            **dict.fromkeys(DURATION_KEYS, 0.0),
            "generation_before": 0, "generation_after": 1,
            "session_step_ms": 1.0, "query_materialization_ms": 2.0,
            "report_ms": 3.0, "consumer_step_ms": 6.5,
            "invariant_ms": "not_run", "verify_ms": "not_run",
        }
        validate_batch_phase_timings([phase], 1, "sample", smoke.require, debug_checks=False)
        for key, value in [
            ("consumer_step_ms", 5.0), ("verify_ms", 0.0),
            ("query_materialization_ms", float("nan")),
        ]:
            with self.subTest(key=key), self.assertRaises(smoke.ControllerError):
                validate_batch_phase_timings(
                    [{**phase, key: value}], 1, "sample", smoke.require, debug_checks=False,
                )

    def test_on_build_is_rejected_before_creating_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            probe = root / "probe"
            probe.touch()
            args = argparse.Namespace(
                samples=6, warmup_runs=1, probe=probe, timeout_seconds=10, run_root=root / "runs"
            )
            response = subprocess.CompletedProcess(
                [], 0, json.dumps({"debug_checks_enabled": True, "build_config": "Release"})
            )
            with (
                patch.object(smoke.subprocess, "run", return_value=response),
                patch.object(smoke, "run_child") as child,
                self.assertRaisesRegex(smoke.ControllerError, "DEBUG_CHECKS=OFF"),
            ):
                smoke.run(args)
            child.assert_not_called()
            self.assertFalse(args.run_root.exists())

    def test_non_release_native_build_skips_before_loading_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            probe = root / "probe"
            probe.touch()
            args = argparse.Namespace(
                samples=6, warmup_runs=1, probe=probe, timeout_seconds=10,
                run_root=root / "runs")
            response = subprocess.CompletedProcess(
                [], 0, json.dumps({"debug_checks_enabled": False,
                                   "build_config": "Debug"}))
            with (
                patch.object(smoke.subprocess, "run", return_value=response),
                patch.object(smoke, "run_child") as child,
            ):
                self.assertFalse(smoke.run(args))
            child.assert_not_called()
            self.assertFalse(args.run_root.exists())


def reject_real_on_probe(probe: pathlib.Path) -> None:
    build = subprocess.run([str(probe), "build-info"], check=True, capture_output=True, text=True)
    if json.loads(build.stdout)["debug_checks_enabled"] is not True:
        raise RuntimeError("rejection test requires an ON probe")
    # No trace or output arguments: rejection must precede workload parsing/loading.
    result = subprocess.run([str(probe), "performance"], capture_output=True, text=True)
    if result.returncode == 0 or "DEBUG_CHECKS=OFF" not in result.stderr:
        raise RuntimeError(f"performance mode was not explicitly rejected: {result.stderr}")
    try:
        smoke.probe_build_info(probe, 10)
    except smoke.ControllerError as error:
        if "DEBUG_CHECKS=OFF" not in str(error):
            raise
    else:
        raise RuntimeError("Python performance entry accepted ON")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--reject-probe":
        reject_real_on_probe(pathlib.Path(sys.argv[2]).resolve())
    else:
        unittest.main()
