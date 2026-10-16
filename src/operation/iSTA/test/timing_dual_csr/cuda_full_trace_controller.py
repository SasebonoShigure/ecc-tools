#!/usr/bin/env python3
"""Stage 7 real-device frozen-prefix differential and separate OFF smoke."""

import argparse
import json
import math
import pathlib
import subprocess

from dual_csr_trace_controller import first_difference, require, run_child, write_json


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


def require_performance_result(result: dict, prefix: int, build: dict) -> None:
    require(not any(key in result for key in ("functional", "batches", "device_checks")),
            "performance child generated a diagnostic receipt")
    require(result["schema_version"] == "ista-cuda-full-trace/7.0.0"
            and result["build"] == build and result["debug_checks_enabled"] is False
            and result["correctness_comparison"] == "not_run"
            and result["requested_backend"] == result["executed_backend"] == "cuda"
            and result["requested_strategy"] == result["executed_strategy"] == "full"
            and result["fallback_count"] == 0 and result["lifecycle_cleanup_completed"]
            and result["prefix_batches"] == prefix
            and len(result["batch_timings"]) == prefix,
            "performance child omitted the declared full workload or lifecycle")
    for ordinal, timing in enumerate(result["batch_timings"]):
        require_cuda_telemetry(timing)
        require(timing["readback"]["other_d2h_bytes"] == 0,
                "performance child performed unclassified readbacks")
        require_final_consumption(timing, final=ordinal + 1 == prefix)
        require(timing["generation_before"] == ordinal
                and timing["generation_after"] == ordinal + 1,
                "performance generation is not contiguous")


def run(args: argparse.Namespace) -> int:
    probe = args.probe.resolve(strict=True)
    trace = (args.replay or args.trace).resolve(strict=True)
    input_flag = "--replay" if args.replay else "--trace"
    root = args.run_root.resolve()
    # Remove only this invocation's verdict files before any validation can fail.
    for name in ("verdict.json", "performance.json"):
        (root / name).unlink(missing_ok=True)
    build = json.loads(subprocess.check_output([str(probe), "build-info"], text=True))
    require(
        build["debug_checks_enabled"] != args.performance,
        "performance requires checks OFF; diagnostics require checks ON",
    )
    if args.performance and build["build_config"] != "Release":
        print("SKIP: CUDA performance requires Release")
        return 77
    value = json.loads(trace.read_text())
    program = value if args.replay else value["replay"]
    batches = program["workload"]["batches"]
    require(isinstance(batches, list) and batches, "frozen trace requires nonempty batches")
    root.mkdir(parents=True, exist_ok=True)
    # run_child requires successful exit plus an output file, but does not remove
    # old outputs: Stage 5 gives it a new directory. Clear this run's exact files
    # before reuse so a child that exits without writing cannot inherit success.
    outputs = (
        [f"warmup-{i}.json" for i in range(args.warmup_runs)]
        + [f"sample-{index}.json" for index in range(args.samples)]
        if args.performance
        else [
            f"{role}-{prefix}.json"
            for prefix in range(1, len(batches) + 1)
            for role in ("cuda", "fresh")
        ]
        + ["rollback.json"]
    )
    for name in outputs:
        (root / name).unlink(missing_ok=True)
    if args.performance:
        # Each child loads and cleans its own initial design; there is no
        # comparison child or functional receipt in this execution branch.
        warmups = []
        for index in range(args.warmup_runs):
            result, duration = run_child(
                probe,
                "performance",
                trace,
                len(batches),
                root / f"warmup-{index}.json",
                args.timeout_seconds,
                input_flag=input_flag,
            )
            require_performance_result(result, len(batches), build)
            warmups.append({"process_ms": duration, "result": result})
        samples = []
        for index in range(args.samples):
            result, duration = run_child(
                probe,
                "performance",
                trace,
                len(batches),
                root / f"sample-{index}.json",
                args.timeout_seconds,
                input_flag=input_flag,
            )
            require_performance_result(result, len(batches), build)
            samples.append({"process_ms": duration, "result": result})
        write_json(
            root / "performance.json",
            {
                "schema_version": "ista-cuda-full-performance/6.0.0",
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
    for prefix in range(1, len(batches) + 1):
        # Real-design diagnostic children can each use 6 GiB; keep independent
        # processes sequential so validation fits the 16 GiB WSL environment.
        gpu, gpu_ms = run_child(
            probe,
            "cuda",
            trace,
            prefix,
            root / f"cuda-{prefix}.json",
            args.timeout_seconds,
            input_flag=input_flag,
        )
        cpu, cpu_ms = run_child(
            probe,
            "fresh",
            trace,
            prefix,
            root / f"fresh-{prefix}.json",
            args.timeout_seconds,
            input_flag=input_flag,
        )
        require(
            gpu["schema_version"] == "ista-cuda-full-trace/7.0.0"
            and cpu["schema_version"] == "ista-cuda-full-trace/7.0.0",
            "unexpected full trace schema",
        )
        difference = first_difference(gpu["batches"][-1], cpu["functional"])
        require(difference is None, f"prefix {prefix}: {difference}")
        require(
            len(gpu["batches"]) == prefix and gpu["lifecycle_cleanup_completed"],
            "GPU prefix did not complete every batch and lifecycle cleanup",
        )
        for ordinal, telemetry in enumerate(gpu["batch_timings"]):
            require_cuda_telemetry(telemetry)
            require(
                telemetry["generation_before"] == ordinal
                and telemetry["generation_after"] == ordinal + 1,
                "GPU prefix generation is not contiguous",
            )
            require(
                telemetry["h2d_bytes"] > 0
                and telemetry["d2h_bytes"] > 0
                and telemetry["explicit_kernel_launches"] > 0,
                "GPU batch omitted full upload/compute/readback",
            )
        verdicts.append(
            {
                "prefix": prefix,
                "equivalent": True,
                "cuda_process_ms": gpu_ms,
                "fresh_process_ms": cpu_ms,
            }
        )
        # Compare earlier observations from this cumulative run with the prior
        # independent prefix, so repeat/cold state cannot silently diverge.
        if prefix > 1:
            previous = json.loads((root / f"cuda-{prefix - 1}.json").read_text())
            require(
                first_difference(gpu["batches"][:-1], previous["batches"]) is None,
                "repeated CUDA prefix changed earlier batch values",
            )
    if len(batches) >= 2:
        recovered, _ = run_child(
            probe,
            "rollback",
            trace,
            len(batches),
            root / "rollback.json",
            args.timeout_seconds,
            input_flag=input_flag,
        )
        require(
            first_difference(recovered["batches"][-1], gpu["batches"][-1]) is None,
            "healthy replay after failed second batch changed result",
        )
    write_json(
        root / "verdict.json",
        {"schema_version": "ista-cuda-full-verdict/7.0.0", "build": build, "prefixes": verdicts},
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
    parser.add_argument("--performance", action="store_true")
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--warmup-runs", type=int, default=5)
    args = parser.parse_args()
    if args.warmup_runs <= 0 or args.samples <= 0:
        parser.error("samples and warmups must be positive")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
