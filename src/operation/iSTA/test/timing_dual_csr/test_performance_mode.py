"""Behavioral contracts for performance orchestration and diagnostic rejection."""

import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
import weakref
from unittest.mock import patch

import dual_csr_timing_smoke as smoke
from dual_csr_timing_evidence import DURATION_KEYS, validate_batch_phase_timings


def cuda_readback_fixture(total_bytes, decision_bytes):
    categories = {
        name: {"bytes": 0, "wait_ms": 0.0}
        for name in ("levelization_decisions", "level_metadata", "status", "candidate", "result")
    }
    categories["levelization_decisions"]["bytes"] = decision_bytes
    categories["level_metadata"]["bytes"] = total_bytes - decision_bytes
    return {
        "schema_version": "ista-cuda-readback/1.0.0",
        "stream_waits": 2,
        "stream_wait_ms": 1.0,
        "other_d2h_bytes": 0,
        **categories,
    }


class PerformanceModeTest(unittest.TestCase):
    def test_child_receives_cpu_affinity_without_external_taskset(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            probe = root / "probe"
            probe.write_text(
                f"#!{sys.executable}\n"
                "import json,os,pathlib,sys\n"
                "pathlib.Path(sys.argv[sys.argv.index('--output')+1]).write_text("
                "json.dumps({'affinity': sorted(os.sched_getaffinity(0))}))\n"
            )
            probe.chmod(0o755)
            affinity = [min(os.sched_getaffinity(0))]
            value, _ = smoke.run_child(
                probe,
                "benchmark",
                root / "trace",
                1,
                root / "result.json",
                10,
                cpu_affinity=affinity,
            )
            self.assertEqual(value, {"affinity": affinity})

    def test_schema_cli_rejects_nonobject_document(self):
        validator = (
            pathlib.Path(smoke.__file__).resolve().parents[2]
            / "benchmark/schema/validate_incremental_sta_samples.py"
        )
        with tempfile.TemporaryDirectory() as directory:
            document = pathlib.Path(directory) / "scalar.json"
            document.write_text("null")
            result = subprocess.run(
                [sys.executable, str(validator), str(document)], capture_output=True, text=True
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("validation failed:", result.stderr)
            self.assertNotIn("Traceback", result.stderr)

    def test_sample_starts_only_the_target_workload(self):
        phases = [
            {
                **dict.fromkeys(DURATION_KEYS, 0.0),
                "generation_before": i,
                "generation_after": i + 1,
                "result_consumption": "completed" if i == 1 else "not_run",
                "session_step_ms": 1.0,
                "query_materialization_ms": 2.0 if i == 1 else None,
                "report_ms": 3.0 if i == 1 else None,
                "consumer_step_ms": 6.5,
                "invariant_ms": "not_run",
                "verify_ms": "not_run",
            }
            for i in range(2)
        ]
        result = {
            "schema_version": "ista-incregpusta-performance-process/4.0.0",
            "debug_checks_enabled": False,
            "correctness_comparison": "not_run",
            "diagnostics": "not_run",
            "result_requirements": {
                "scope": "final_prefix",
                "timing_values": True,
                "summary": True,
                "timing_paths": True,
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
        phases[0]["report_ms"] = 1.0
        with (
            patch.object(smoke, "run_child", return_value=(result, 1.0)),
            self.assertRaisesRegex(smoke.ControllerError, "must be null when not_run"),
        ):
            smoke._run_sample(
                pathlib.Path("probe"),
                pathlib.Path("trace"),
                [{}, {}],
                pathlib.Path("runs"),
                "sample",
                10,
            )

    def test_consumer_interval_and_diagnostic_canaries(self):
        phase = {
            **dict.fromkeys(DURATION_KEYS, 0.0),
            "generation_before": 0,
            "generation_after": 1,
            "result_consumption": "completed",
            "session_step_ms": 1.0,
            "query_materialization_ms": 2.0,
            "report_ms": 3.0,
            "consumer_step_ms": 6.5,
            "invariant_ms": "not_run",
            "verify_ms": "not_run",
        }
        validate_batch_phase_timings([phase], 1, "sample", smoke.require, debug_checks=False)
        for key, value in [
            ("consumer_step_ms", 5.0),
            ("verify_ms", 0.0),
            ("query_materialization_ms", float("nan")),
        ]:
            with self.subTest(key=key), self.assertRaises(smoke.ControllerError):
                validate_batch_phase_timings(
                    [{**phase, key: value}],
                    1,
                    "sample",
                    smoke.require,
                    debug_checks=False,
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
                samples=6, warmup_runs=1, probe=probe, timeout_seconds=10, run_root=root / "runs"
            )
            response = subprocess.CompletedProcess(
                [], 0, json.dumps({"debug_checks_enabled": False, "build_config": "Debug"})
            )
            with (
                patch.object(smoke.subprocess, "run", return_value=response),
                patch.object(smoke, "run_child") as child,
            ):
                self.assertFalse(smoke.run(args))
            child.assert_not_called()
            self.assertFalse(args.run_root.exists())

    def test_cuda_incremental_smoke_validates_warmups_and_samples(self):
        import copy

        import cuda_targeted_trace_controller as cuda

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            probe, trace = root / "probe", root / "replay.json"
            probe.touch()
            trace.write_text(json.dumps({"workload": {"batches": [{}, {}]}}))
            build = {"debug_checks_enabled": False, "build_config": "Release"}
            result = {
                "schema_version": "ista-cuda-incremental-trace/8.0.0",
                "build": build,
                "debug_checks_enabled": False,
                "correctness_comparison": "not_run",
                "requested_backend": "cuda",
                "executed_backend": "cuda",
                "requested_strategy": "incremental",
                "executed_strategy": "incremental",
                "fallback_count": 0,
                "lifecycle_cleanup_completed": True,
                "prefix_batches": 2,
                "batch_timings": [
                    {
                        "generation_before": i,
                        "generation_after": i + 1,
                        "explicit_kernel_launches": 4,
                        "device_algorithm_calls": 2,
                        "levelization_decision_bytes": 32,
                        "d2h_bytes": 64,
                        "readback": cuda_readback_fixture(64, 32),
                        "result_consumption": "completed" if i == 1 else "not_run",
                        "query_materialize_ms": 1.0 if i == 1 else None,
                        "report_ms": 2.0 if i == 1 else None,
                        "execution": {
                            "requested_backend": "cuda",
                            "requested_update_strategy": "incremental",
                            "fallback_policy": "error",
                            "executed_backend": "cuda",
                            "executed_update_strategy": "incremental",
                            "execution_kind": "incremental_update",
                            "propagation_scope": "full_graph",
                            "incremental_batch_executed": True,
                            "fallback_to_full": False,
                            "fallback_reason": None,
                        },
                    }
                    for i in range(2)
                ],
            }
            args = argparse.Namespace(
                probe=probe,
                replay=trace,
                trace=None,
                run_root=root / "runs",
                performance=True,
                strategy="incremental",
                samples=1,
                warmup_runs=1,
                timeout_seconds=10,
            )
            with (
                patch.object(cuda.subprocess, "check_output", return_value=json.dumps(build)),
                patch.object(cuda, "run_child", return_value=(result, 1.0)) as child,
            ):
                self.assertEqual(cuda.run(args), 0)
                for field in ("query_materialize_ms", "report_ms"):
                    bad = copy.deepcopy(result)
                    bad["batch_timings"][0][field] = 0.0
                    with self.subTest(field=field), self.assertRaises(smoke.ControllerError):
                        cuda.require_performance_result(bad, "incremental", 2)
                bad = copy.deepcopy(result)
                bad["batch_timings"][-1]["result_consumption"] = "not_run"
                with self.assertRaises(smoke.ControllerError):
                    cuda.require_performance_result(bad, "incremental", 2)
                for field, value in (("bytes", 1000), ("wait_ms", float("nan")), ("wait_ms", 2.0)):
                    bad = copy.deepcopy(result)
                    bad["batch_timings"][0]["readback"]["level_metadata"][field] = value
                    with (
                        self.subTest(field=field, value=value),
                        self.assertRaises(smoke.ControllerError),
                    ):
                        cuda.require_performance_result(bad, "incremental", 2)
                self.assertEqual(
                    [call.args[1] for call in child.call_args_list],
                    ["incremental-performance", "incremental-performance"],
                )
                for bad_sample in (False, True):
                    bad = copy.deepcopy(result)
                    del bad["batch_timings"][0]["device_algorithm_calls"]
                    child.reset_mock()
                    child.side_effect = [(result if bad_sample else bad, 1.0), (bad, 1.0)]
                    with self.subTest(sample=bad_sample), self.assertRaises(smoke.ControllerError):
                        cuda.run(args)
                    self.assertFalse((args.run_root / "performance.json").exists())

    def test_frozen_consumer_rotation_pairing_and_canaries(self):
        import copy

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            probe, trace, profile = (root / name for name in ("probe", "trace", "profile"))
            for path in (probe, trace, profile):
                path.write_text("{}")
            trace.write_text(
                json.dumps({"schema_version": "ista-scale-compact-replay/2", "batches": [{}, {}]})
            )
            build = {
                "debug_checks_enabled": False,
                "build_config": "Release",
                "compiler_id": "GNU",
                "compiler_version": "14",
            }
            requirements = {
                "timing_values": "all_nodes_arrival_required_slack",
                "qor": "complete",
                "max_paths": 20,
                "nworst": 20,
                "file_artifacts": False,
            }
            manifest = {
                "state": "frozen",
                "input_files": [smoke._file_reference(trace)],
                "thread_execution_evidence": smoke._file_reference(profile),
                "cases": [
                    {
                        "name": "wide",
                        "trace": smoke._file_reference(trace),
                        "prefix_batches": 2,
                        "threads": 8,
                        "cpu_affinity": [1, 2],
                        "mandatory": True,
                    }
                ],
            }
            manifest_path = root / "manifest.json"
            manifest_path.write_text(json.dumps(manifest))
            args = argparse.Namespace(
                manifest=manifest_path,
                case="wide",
                warmup_runs=1,
                samples=3,
                run_root=root / "runs",
                output=root / "samples.json",
                timeout_seconds=10,
            )
            seen = []
            workloads = []
            read_json = smoke.read_json

            class Workload(dict):
                pass

            def observe_workload(path):
                value = read_json(path)
                if path == trace:
                    value = Workload(value)
                    workloads.append(weakref.ref(value))
                return value

            def child(probe_arg, role, trace_arg, prefix, output, timeout, **options):
                self.assertTrue(all(value() is None for value in workloads))
                self.assertEqual(
                    (probe_arg, role, trace_arg, prefix, timeout),
                    (probe, "benchmark", trace, 2, 10),
                )
                self.assertEqual(options["input_flag"], "--replay")
                self.assertEqual(options["cpu_affinity"], [1, 2])
                backend, strategy = options["extra_args"][1], options["extra_args"][3]
                self.assertEqual(options["extra_args"][-2:], ("--threads", "8"))
                mode = f"{backend}/{strategy}"
                seen.append(mode)
                elapsed = {"cpu/full": 8.0, "cuda/incremental": 2.0}[mode]
                value = {
                    "schema_version": "ista-consumer-sample/3.0.0",
                    "report_schedule": "final_only",
                    "debug_checks_enabled": False,
                    "build": build,
                    "correctness_comparison": "not_run",
                    "diagnostics": "not_run",
                    "requested_backend": backend,
                    "requested_strategy": strategy,
                    "threads_requested": 8,
                    "cpu_affinity": [1, 2],
                    "thread_activity": "external_profile",
                    "prefix_batches": prefix,
                    "device": None
                    if backend == "cpu"
                    else {
                        "name": "test GPU",
                        "uuid": "GPU-00000000-0000-0000-0000-000000000000",
                        "driver_version": 12080,
                        "runtime_version": 12080,
                    },
                    "result": {
                        "generation": prefix,
                        "report_consumptions": 1,
                        "timing_node_count": 10,
                        "baseline_instance_count": 1,
                        "baseline_timing_node_count": 6,
                    },
                    "lifecycle_cleanup_completed": True,
                    "result_requirements": requirements,
                    "consumer_step_ms": 2 * elapsed,
                    "load_initialize_ms": 1.0,
                    "backend_setup_ms": 0.5,
                    "workload_ms": 2 * elapsed + 1,
                    "edit_sta_loop_ms": 1.5 * elapsed,
                    "consumer_loop_ms": 2 * elapsed + 0.25,
                    "cleanup_ms": 1.0,
                    "cold_end_to_end_ms": 2 * elapsed + 4,
                    "batch_timings": [
                        {
                            "generation_after": generation,
                            "commit_ms": elapsed / 2,
                            "query_materialize_ms": elapsed / 8 if generation == 2 else None,
                            "report_prepare_ms": elapsed / 8 if generation == 2 else None,
                            "consumer_read_ms": elapsed / 8 if generation == 2 else None,
                            "results_consumed": generation == 2,
                            "consumer_step_ms": elapsed,
                            "fallback": False,
                            "executed_backend": backend,
                            "executed_strategy": strategy,
                        }
                        for generation in (1, 2)
                    ],
                }
                if backend == "cuda":
                    for batch in value["batch_timings"]:
                        batch.update(
                            explicit_kernel_launches=12,
                            device_algorithm_calls=3,
                            levelization_decision_bytes=24,
                            allocations=0,
                            h2d_bytes=100,
                            d2h_bytes=200,
                            readback=cuda_readback_fixture(200, 24),
                        )
                output.parent.mkdir(parents=True)
                output.write_text(json.dumps(value))
                return value, value["cold_end_to_end_ms"] + 1

            with (
                patch.object(smoke, "read_json", side_effect=observe_workload),
                patch.object(smoke, "run_child", side_effect=child),
            ):
                self.assertTrue(smoke.run_consumer_experiment(args, probe, build))
            self.assertEqual(seen, ["cpu/full", "cuda/incremental"] * 4)
            value = json.loads(args.output.read_text())
            self.assertEqual(value["performance_gate"], "not_run")
            self.assertEqual(value["stage_acceptance"], "not_run")
            from validate_incremental_sta_samples import ValidationError, validate_document

            schema_path = (
                pathlib.Path(smoke.__file__).resolve().parents[2]
                / "benchmark/schema/incremental_sta_samples_v3.schema.json"
            )
            schema = json.loads(schema_path.read_text())
            validate_document(value, schema)
            historical = copy.deepcopy(value)
            from consumer_trace_samples import MODES, consumer_statistics

            historical.update(schema_version="2.0.0", stage12_acceptance="not_run")
            for key in ("report_schedule", "primary_metric", "stage_acceptance"):
                historical.pop(key)
            historical["protocol"]["mode_order"] = list(MODES)
            historical["samples"] = []
            for warmup, count in ((True, 1), (False, 3)):
                for repetition in range(1, count + 1):
                    for mode in MODES:
                        row = copy.deepcopy(value["samples"][0 if mode.startswith("cpu/") else 1])
                        row.update(mode=mode, warmup=warmup, repetition=repetition)
                        for key in (
                            "consumer_loop_ms",
                            "edit_sta_loop_ms",
                            "result",
                            "lifecycle_cleanup_completed",
                        ):
                            row.pop(key)
                        for batch in row["batch_timings"]:
                            batch.pop("results_consumed")
                            batch.pop("report_prepare_ms")
                            batch.pop("readback", None)
                            batch["query_materialize_ms"] = batch["consumer_step_ms"] / 8
                            batch["consumer_read_ms"] = batch["consumer_step_ms"] / 8
                            batch["executed_backend"], batch["executed_strategy"] = mode.split("/")
                        historical["samples"].append(row)
            historical["summaries"], historical["comparisons"] = consumer_statistics(
                historical["samples"]
            )
            historical_schema = json.loads(
                schema_path.with_name("incremental_sta_samples_v2.schema.json").read_text()
            )
            validate_document(historical, historical_schema)
            for tamper in (
                "missing",
                "unpaired",
                "negative",
                "fallback",
                "wrong-ci",
                "wrong-host",
                "changed-gpu",
                "missing-cub",
                "bad-launches",
                "short-gate",
                "early-report",
                "missing-final-report",
                "short-loop",
                "missing-readback",
                "fake-cleanup",
                "wrong-consumptions",
            ):
                bad = copy.deepcopy(value)
                if tamper == "missing":
                    bad["samples"].pop()
                elif tamper == "unpaired":
                    bad["samples"][2]["repetition"] = 2
                elif tamper == "negative":
                    bad["samples"][0]["consumer_step_ms"] = -1
                elif tamper == "fallback":
                    bad["samples"][0]["batch_timings"][0]["fallback"] = True
                elif tamper == "wrong-ci":
                    bad["comparisons"]["cuda/incremental_vs_cpu/full"]["ci95"][0] = 3
                elif tamper == "changed-gpu":
                    bad["samples"][3]["device"]["uuid"] = "GPU-00000000-0000-0000-0000-000000000001"
                elif tamper == "missing-cub":
                    del bad["samples"][1]["batch_timings"][0]["device_algorithm_calls"]
                elif tamper == "bad-launches":
                    bad["samples"][1]["batch_timings"][0]["explicit_kernel_launches"] = -1
                elif tamper == "wrong-host":
                    bad["samples"][0]["threads_requested"] = 1
                elif tamper == "early-report":
                    bad["samples"][0]["batch_timings"][0]["report_prepare_ms"] = 0.0
                elif tamper == "missing-final-report":
                    bad["samples"][0]["batch_timings"][-1]["results_consumed"] = False
                elif tamper == "short-loop":
                    bad["samples"][0]["consumer_loop_ms"] = 0.0
                elif tamper == "missing-readback":
                    del bad["samples"][1]["batch_timings"][0]["readback"]
                elif tamper == "fake-cleanup":
                    bad["samples"][0]["lifecycle_cleanup_completed"] = False
                elif tamper == "wrong-consumptions":
                    bad["samples"][0]["result"]["report_consumptions"] = 2
                else:
                    bad["performance_gate"] = "passed"
                with self.subTest(tamper=tamper), self.assertRaises(ValidationError):
                    validate_document(bad, schema)
            args.run_root = root / "changed-probe-runs"

            def changing_probe(*child_args, **options):
                result = child(*child_args, **options)
                probe.write_text("replaced binary")
                return result

            with (
                patch.object(smoke, "run_child", side_effect=changing_probe),
                self.assertRaisesRegex(smoke.ControllerError, "executable changed"),
            ):
                smoke.run_consumer_experiment(args, probe, build)
            for changed in (trace, profile, manifest_path):
                original = changed.read_bytes()
                args.run_root = root / f"changed-{changed.name}-runs"
                args.output = root / f"changed-{changed.name}-samples.json"

                def changing_input(*child_args, changed_path=changed, **options):
                    result = child(*child_args, **options)
                    changed_path.write_text("replaced input")
                    return result

                with (
                    self.subTest(changed=changed.name),
                    patch.object(smoke, "run_child", side_effect=changing_input),
                    self.assertRaisesRegex(smoke.ControllerError, "frozen input changed"),
                ):
                    smoke.run_consumer_experiment(args, probe, build)
                self.assertTrue((args.run_root / "completed-samples.json").exists())
                self.assertFalse(args.output.exists())
                changed.write_bytes(original)
            manifest["cases"][0]["prefix_batches"] = 1
            manifest_path.write_text(json.dumps(manifest))
            with (
                patch.object(smoke, "run_child") as launcher,
                self.assertRaises(smoke.ControllerError),
            ):
                smoke.run_consumer_experiment(args, probe, build)
            launcher.assert_not_called()
            manifest["cases"][0]["prefix_batches"] = 2
            manifest_path.write_text(json.dumps(manifest))
            trace.write_text("changed")
            with (
                patch.object(smoke, "run_child") as launcher,
                self.assertRaises(smoke.ControllerError),
            ):
                smoke.run_consumer_experiment(args, probe, build)
            launcher.assert_not_called()


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
