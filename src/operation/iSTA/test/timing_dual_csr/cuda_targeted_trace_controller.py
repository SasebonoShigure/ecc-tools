#!/usr/bin/env python3
"""Stage 8 device-targeted/native trace, independent full oracles, and OFF samples."""

import argparse
import concurrent.futures
import json
import pathlib
import subprocess

from dual_csr_trace_controller import first_difference, require, run_child, write_json


def require_performance_result(result: dict) -> None:
    require(
        not any(key in result for key in ("functional", "batches", "device_checks")),
        "performance child generated a diagnostic receipt",
    )
    require(
        result["schema_version"] == "ista-cuda-targeted-trace/1.0.0"
        and result["debug_checks_enabled"] is False
        and result["correctness_comparison"] == "not_run"
        and result["executed_backend"] == "cuda"
        and result["executed_strategy"] == "targeted-full-levelization"
        and result["native_final_rollback_completed"],
        "performance child did not complete the declared workload/lifecycle",
    )


def run(args: argparse.Namespace) -> int:
    probe = args.probe.resolve(strict=True)
    trace = args.trace.resolve(strict=True)
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
    batches = json.loads(trace.read_text())["replay"]["workload"]["batches"]
    require(isinstance(batches, list) and batches, "frozen trace requires nonempty batches")
    root.mkdir(parents=True, exist_ok=True)
    names = (
        ["warmup.json"] + [f"sample-{i}.json" for i in range(args.samples)]
        if args.performance
        else [
            f"{role}-{prefix}.json"
            for prefix in range(1, len(batches) + 1)
            for role in ("targeted", "cuda-full", "fresh")
        ]
        + ["rollback.json"]
    )
    for name in names:
        (root / name).unlink(missing_ok=True)
    if args.performance:
        warmup, _ = run_child(
            probe, "performance", trace, len(batches), root / "warmup.json", args.timeout_seconds
        )
        require_performance_result(warmup)
        samples = []
        for index in range(args.samples):
            result, duration = run_child(
                probe,
                "performance",
                trace,
                len(batches),
                root / f"sample-{index}.json",
                args.timeout_seconds,
            )
            require_performance_result(result)
            samples.append({"process_ms": duration, "result": result})
        write_json(
            root / "performance.json",
            {
                "schema_version": "ista-cuda-targeted-performance/1.0.0",
                "debug_checks_enabled": False,
                "correctness_comparison": "not_run",
                "build": build,
                "samples": samples,
            },
        )
        return 0

    require(args.full_probe is not None, "diagnostics require an independent full probe")
    full_probe = args.full_probe.resolve(strict=True)
    full_build = json.loads(subprocess.check_output([str(full_probe), "build-info"], text=True))
    require(full_build["debug_checks_enabled"] is True, "full oracle probe requires checks ON")
    verdicts = []
    previous_batches = []
    for prefix in range(1, len(batches) + 1):
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            targeted_task = pool.submit(
                run_child,
                probe,
                "targeted",
                trace,
                prefix,
                root / f"targeted-{prefix}.json",
                args.timeout_seconds,
            )
            full_task = pool.submit(
                run_child,
                full_probe,
                "cuda",
                trace,
                prefix,
                root / f"cuda-full-{prefix}.json",
                args.timeout_seconds,
            )
            fresh_task = pool.submit(
                run_child,
                full_probe,
                "fresh",
                trace,
                prefix,
                root / f"fresh-{prefix}.json",
                args.timeout_seconds,
            )
            targeted, targeted_ms = targeted_task.result()
            full, full_ms = full_task.result()
            fresh, fresh_ms = fresh_task.result()
        require(
            targeted["schema_version"] == "ista-cuda-targeted-trace/1.0.0"
            and targeted["executed_backend"] == "cuda"
            and targeted["executed_strategy"] == "targeted-full-levelization"
            and targeted["fallback_count"] == 0,
            "targeted trace did not execute the requested device path",
        )
        require(
            len(targeted["batches"]) == prefix
            and len(targeted["batch_timings"]) == prefix
            and len(targeted["device_checks"]) == prefix
            and targeted["native_final_rollback_completed"],
            "targeted prefix omitted a batch, device observation or rollback",
        )
        for ordinal, (values, full_values) in enumerate(
            zip(targeted["batches"], full["batches"], strict=True)
        ):
            difference = first_difference(values, full_values)
            require(difference is None, f"CUDA/full batch {ordinal + 1}: {difference}")
            timing = targeted["batch_timings"][ordinal]
            require(
                timing["generation_before"] == ordinal
                and timing["generation_after"] == ordinal + 1,
                "targeted generation is not contiguous",
            )
            require(
                targeted["device_checks"][ordinal]["graph_equivalent"] is True,
                "actual device decode differs from canonical graph",
            )
        difference = first_difference(targeted["batches"][-1], fresh["functional"])
        require(difference is None, f"fresh CPU/full prefix {prefix}: {difference}")
        require(
            first_difference(targeted["batches"][:-1], previous_batches) is None,
            "repeated prefix changed previously observed values",
        )
        previous_batches = targeted["batches"]
        verdicts.append(
            {
                "prefix": prefix,
                "cuda_full_equivalent": True,
                "fresh_cpu_equivalent": True,
                "targeted_process_ms": targeted_ms,
                "cuda_full_process_ms": full_ms,
                "fresh_cpu_process_ms": fresh_ms,
            }
        )
    if len(batches) >= 2:
        recovered, _ = run_child(
            probe, "rollback", trace, len(batches), root / "rollback.json", args.timeout_seconds
        )
        require(
            first_difference(recovered["batches"], previous_batches) is None
            and recovered["native_final_rollback_completed"],
            "recovered healthy trace changed values or omitted rollback",
        )
    write_json(
        root / "verdict.json",
        {
            "schema_version": "ista-cuda-targeted-verdict/1.0.0",
            "build": build,
            "full_oracle_build": full_build,
            "prefixes": verdicts,
        },
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    parser.add_argument("--full-probe", type=pathlib.Path)
    parser.add_argument("--trace", type=pathlib.Path, required=True)
    parser.add_argument("--run-root", type=pathlib.Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument("--performance", action="store_true")
    parser.add_argument("--samples", type=int, default=3)
    args = parser.parse_args()
    if args.samples <= 0 or args.timeout_seconds <= 0:
        parser.error("samples and timeout must be positive")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
