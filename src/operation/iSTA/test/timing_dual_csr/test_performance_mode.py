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


class PerformanceModeTest(unittest.TestCase):
    def test_sample_starts_only_the_target_workload(self):
        result = {
            "schema_version": "ista-incregpusta-performance-process/2.0.0",
            "debug_checks_enabled": False,
            "correctness_comparison": "not_run",
            "input": {"node_slots": 2, "arc_slots": 1, "batches": 2},
            "requested_mode": "full_stage5",
            "executed_mode": "full_stage5",
            "result": {"generation": 2, "native_final_rollback_completed": True},
            "run": {"child_pid": 123, "batch_phase_timings": []},
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
