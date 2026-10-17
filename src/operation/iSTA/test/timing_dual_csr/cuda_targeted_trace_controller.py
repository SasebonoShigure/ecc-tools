#!/usr/bin/env python3
"""Stage 8 device-targeted/native trace, independent full oracles, and OFF samples."""

import argparse
import json
import math
import pathlib
import subprocess

from functional_timing import validate_functional
from trace_process import first_difference, require, run_child, write_json


def require_cuda_telemetry(timing: dict) -> None:
    for field in ("explicit_kernel_launches", "device_algorithm_calls",
                  "levelization_decision_bytes"):
        require(type(timing.get(field)) is int and timing[field] >= 0,
                f"missing nonnegative CUDA {field}")
    require("kernel_launches" not in timing, "ambiguous CUDA total launch count")
    readback = timing["readback"]
    require(readback["schema_version"] == "ista-cuda-readback/1.0.0",
            "unknown CUDA readback accounting")
    require(type(readback["stream_waits"]) is int and readback["stream_waits"] >= 0,
            "invalid CUDA stream wait count")
    total_bytes = readback["other_d2h_bytes"]
    require(type(total_bytes) is int and total_bytes >= 0, "invalid unclassified readback bytes")
    waits = 0.0
    for category in ("levelization_decisions", "level_metadata", "status", "candidate", "result"):
        item = readback[category]
        require(type(item["bytes"]) is int and item["bytes"] >= 0,
                f"invalid {category} readback bytes")
        require(type(item["wait_ms"]) in (int, float)
                and math.isfinite(item["wait_ms"]) and item["wait_ms"] >= 0,
                f"invalid {category} readback wait")
        total_bytes += item["bytes"]
        waits += item["wait_ms"]
    require(type(timing["d2h_bytes"]) is int and total_bytes == timing["d2h_bytes"],
            "CUDA readback categories differ from actual D2H bytes")
    total_wait = readback["stream_wait_ms"]
    require(type(total_wait) in (int, float) and math.isfinite(total_wait) and total_wait >= 0
            and waits <= total_wait + 1e-6,
            "CUDA readback waits exceed recorded Runtime waits")


def require_final_consumption(timing: dict, *, final: bool) -> None:
    require(timing["result_consumption"] == ("completed" if final else "not_run"),
            "ordinary CUDA replay must consume full results only at the final prefix")
    for field in ("query_materialize_ms", "report_ms"):
        value = timing[field]
        require(
            type(value) in (int, float) and math.isfinite(value) and value >= 0
            if final else value is None,
            f"{field} must reflect actual result consumption",
        )


def require_performance_result(result: dict, strategy: str, expected_prefix: int) -> None:
    protocol = "incremental" if strategy == "incremental" else "targeted"
    require(
        not any(key in result for key in ("functional", "batches", "device_checks")),
        "performance child generated a diagnostic receipt",
    )
    require(
        result["schema_version"] == f"ista-cuda-{protocol}-trace/8.0.0"
        and result["debug_checks_enabled"] is False
        and result["correctness_comparison"] == "not_run"
        and result["executed_backend"] == "cuda"
        and result["executed_strategy"] == strategy
        and result["lifecycle_cleanup_completed"],
        "performance child did not complete the declared workload/lifecycle",
    )
    require(
        result["prefix_batches"] == expected_prefix
        and len(result["batch_timings"]) == expected_prefix,
        "performance workload omitted a batch",
    )
    for ordinal, timing in enumerate(result["batch_timings"]):
        require_execution(timing, strategy)
        require(timing["readback"]["other_d2h_bytes"] == 0,
                "performance child performed unclassified readbacks")
        require_final_consumption(timing, final=ordinal + 1 == expected_prefix)
        require(
            timing["generation_before"] == ordinal and timing["generation_after"] == ordinal + 1,
            "performance generation is not contiguous",
        )


def compare_values(actual: dict, expected: dict, label: str, *, incremental: bool) -> None:
    if incremental:
        actual, _ = validate_functional(actual, f"{label}.incremental")
        expected, _ = validate_functional(expected, f"{label}.full")
    difference = first_difference(actual, expected)
    require(difference is None, f"{label}: {difference}")


def require_execution(timing: dict, strategy: str) -> None:
    require_cuda_telemetry(timing)
    incremental = strategy == "incremental"
    record = timing["execution"]
    require(
        record
        == {
            "requested_backend": "cuda",
            "requested_update_strategy": "incremental" if incremental else "full",
            "fallback_policy": "error",
            "executed_backend": "cuda",
            "executed_update_strategy": "incremental" if incremental else "full",
            "execution_kind": "incremental_update" if incremental else "full_update",
            "propagation_scope": "full_graph",
            "incremental_batch_executed": incremental,
            "fallback_to_full": False,
            "fallback_reason": None,
        },
        "targeted batch did not execute the declared strategy and full propagation",
    )


def run(args: argparse.Namespace) -> int:
    incremental = args.strategy == "incremental"
    protocol = "incremental" if incremental else "targeted"
    performance_mode = "incremental-performance" if incremental else "performance"
    rollback_mode = "incremental-rollback" if incremental else "rollback"
    probe = args.probe.resolve(strict=True)
    trace = (args.replay or args.trace).resolve(strict=True)
    input_flag = "--replay" if args.replay else "--trace"
    root = args.run_root.resolve()
    for name in ("verdict.json", "performance.json"):
        (root / name).unlink(missing_ok=True)
    build = json.loads(subprocess.check_output([str(probe), "build-info"], text=True))
    require(
        build["debug_checks_enabled"] != args.performance,
        "performance requires checks OFF; diagnostics require checks ON",
    )
    if args.performance and build["build_config"] != "Release":
        print("SKIP: targeted CUDA performance requires Release")
        return 77
    value = json.loads(trace.read_text())
    program = value if args.replay else value["replay"]
    batches = program["workload"]["batches"]
    require(isinstance(batches, list) and batches, "frozen trace requires nonempty batches")
    root.mkdir(parents=True, exist_ok=True)
    names = (
        [f"warmup-{i}.json" for i in range(args.warmup_runs)]
        + [f"sample-{i}.json" for i in range(args.samples)]
        if args.performance
        else [
            f"{role}-{prefix}.json"
            for prefix in range(1, len(batches) + 1)
            for role in ("targeted", "fresh")
        ]
        + ["rollback.json"]
    )
    for name in names:
        (root / name).unlink(missing_ok=True)
    if args.performance:
        warmups = []
        for index in range(args.warmup_runs):
            warmup, duration = run_child(
                probe,
                performance_mode,
                trace,
                len(batches),
                root / f"warmup-{index}.json",
                args.timeout_seconds,
                input_flag=input_flag,
            )
            require_performance_result(warmup, args.strategy, len(batches))
            warmups.append({"process_ms": duration, "result": warmup})
        samples = []
        for index in range(args.samples):
            result, duration = run_child(
                probe,
                performance_mode,
                trace,
                len(batches),
                root / f"sample-{index}.json",
                args.timeout_seconds,
                input_flag=input_flag,
            )
            require_performance_result(result, args.strategy, len(batches))
            samples.append({"process_ms": duration, "result": result})
        write_json(
            root / "performance.json",
            {
                "schema_version": f"ista-cuda-{protocol}-performance/7.0.0",
                "debug_checks_enabled": False,
                "correctness_comparison": "not_run",
                "build": build,
                "samples": samples,
                "warmup": warmups,
                "sampling_kind": "short_smoke"
                if args.warmup_runs < 5 or args.samples < 30
                else "engineering",
                "stage12_performance_acceptance": "not_run",
            },
        )
        return 0

    verdicts = []
    previous_batches = []
    for prefix in range(1, len(batches) + 1):
        # Preserve independent processes without overlapping multi-GiB snapshots.
        targeted, targeted_ms = run_child(
            probe,
            protocol,
            trace,
            prefix,
            root / f"targeted-{prefix}.json",
            args.timeout_seconds,
            input_flag=input_flag,
        )
        fresh, fresh_ms = run_child(
            probe,
            "fresh",
            trace,
            prefix,
            root / f"fresh-{prefix}.json",
            args.timeout_seconds,
            input_flag=input_flag,
        )
        require(
            targeted["schema_version"] == f"ista-cuda-{protocol}-trace/8.0.0"
            and targeted["executed_backend"] == "cuda"
            and targeted["executed_strategy"] == args.strategy
            and targeted["fallback_count"] == 0,
            "targeted trace did not execute the requested device path",
        )
        require(
            targeted["prefix_batches"] == prefix
            and len(targeted["batches"]) == prefix
            and len(targeted["batch_timings"]) == prefix
            and len(targeted["device_checks"]) == prefix
            and targeted["lifecycle_cleanup_completed"],
            "targeted prefix omitted a batch, device observation or lifecycle cleanup",
        )
        require(
            fresh["schema_version"] == "ista-native-fresh-trace/1.0.0"
            and fresh["prefix_batches"] == prefix
            and fresh["debug_checks_enabled"] is True
            and fresh["executed_backend"] == "cpu"
            and fresh["executed_strategy"] == "full"
            and fresh["fallback_count"] == 0,
            "fresh oracle did not execute independent native CPU/full",
        )
        for ordinal, values in enumerate(targeted["batches"]):
            if incremental:
                validate_functional(values, f"incremental batch {ordinal + 1}")
            timing = targeted["batch_timings"][ordinal]
            require_execution(timing, args.strategy)
            require(
                timing["generation_before"] == ordinal
                and timing["generation_after"] == ordinal + 1,
                "targeted generation is not contiguous",
            )
            require(
                targeted["device_checks"][ordinal]["graph_equivalent"] is True,
                "actual device decode differs from canonical graph",
            )
            visits = targeted["device_checks"][ordinal]["storage_visits"]
            require(
                isinstance(visits, list) and len(visits) == 12
                and all(type(count) is int and count >= 0 for count in visits),
                "device visits must contain four BaseOut/AuxOut/BaseIn triples",
            )
        compare_values(
            targeted["batches"][-1],
            fresh["functional"],
            f"fresh CPU/full prefix {prefix}",
            incremental=incremental,
        )
        require(
            first_difference(targeted["batches"][:-1], previous_batches) is None,
            "repeated prefix changed previously observed values",
        )
        previous_batches = targeted["batches"]
        verdicts.append(
            {
                "prefix": prefix,
                "fresh_cpu_equivalent": True,
                "targeted_process_ms": targeted_ms,
                "fresh_cpu_process_ms": fresh_ms,
            }
        )
    if len(batches) >= 2:
        recovered, _ = run_child(
            probe,
            rollback_mode,
            trace,
            len(batches),
            root / "rollback.json",
            args.timeout_seconds,
            input_flag=input_flag,
        )
        require(
            first_difference(recovered["batches"], previous_batches) is None
            and recovered["lifecycle_cleanup_completed"],
            "recovered healthy trace changed values or omitted lifecycle cleanup",
        )
    write_json(
        root / "verdict.json",
        {
            "schema_version": f"ista-cuda-{protocol}-verdict/9.0.0",
            "build": build,
            "prefixes": verdicts,
        },
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--trace", type=pathlib.Path)
    inputs.add_argument("--replay", type=pathlib.Path)
    parser.add_argument("--run-root", type=pathlib.Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument(
        "--strategy",
        choices=("targeted-full-levelization", "incremental"),
        default="targeted-full-levelization",
    )
    parser.add_argument("--performance", action="store_true")
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--warmup-runs", type=int, default=5)
    args = parser.parse_args()
    if args.warmup_runs <= 0 or args.samples <= 0 or args.timeout_seconds <= 0:
        parser.error("samples, warmups and timeout must be positive")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
