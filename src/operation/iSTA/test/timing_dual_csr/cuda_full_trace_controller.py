#!/usr/bin/env python3
"""Stage 7 real-device frozen-prefix differential and separate OFF smoke."""

import argparse
import json
import pathlib
import subprocess

from dual_csr_trace_controller import first_difference, require, run_child, write_json


def run(args: argparse.Namespace) -> int:
    probe = args.probe.resolve(strict=True)
    trace = (args.replay or args.trace).resolve(strict=True)
    input_flag = "--replay" if args.replay else "--trace"
    root = args.run_root.resolve()
    # Remove only this invocation's verdict files before any validation can fail.
    for name in ("verdict.json", "performance.json"):
        (root / name).unlink(missing_ok=True)
    build = json.loads(subprocess.check_output([str(probe), "build-info"], text=True))
    require(build["debug_checks_enabled"] != args.performance,
            "performance requires checks OFF; diagnostics require checks ON")
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
    outputs = (["warmup.json"] + [f"sample-{index}.json" for index in range(args.samples)]
               if args.performance else
               [f"{role}-{prefix}.json" for prefix in range(1, len(batches) + 1)
                for role in ("cuda", "fresh")] + ["rollback.json"])
    for name in outputs:
        (root / name).unlink(missing_ok=True)
    if args.performance:
        # Each child loads and cleans its own initial design; there is no
        # comparison child or functional receipt in this execution branch.
        run_child(probe, "performance", trace, len(batches),
                  root / "warmup.json", args.timeout_seconds,
                                   input_flag=input_flag)
        samples = []
        for index in range(args.samples):
            result, duration = run_child(probe, "performance", trace, len(batches),
                                         root / f"sample-{index}.json", args.timeout_seconds,
                                   input_flag=input_flag)
            require("functional" not in result and "batches" not in result,
                    "performance child generated a diagnostic receipt")
            samples.append({"process_ms": duration, "result": result})
        write_json(root / "performance.json", {
            "schema_version": "ista-cuda-full-performance/1.0.0",
            "debug_checks_enabled": False, "correctness_comparison": "not_run",
            "build": build, "samples": samples,
        })
        return 0
    verdicts = []
    for prefix in range(1, len(batches) + 1):
        # Real-design diagnostic children can each use 6 GiB; keep independent
        # processes sequential so validation fits the 16 GiB WSL environment.
        gpu, gpu_ms = run_child(
            probe, "cuda", trace, prefix, root / f"cuda-{prefix}.json",
            args.timeout_seconds, input_flag=input_flag)
        cpu, cpu_ms = run_child(
            probe, "fresh", trace, prefix, root / f"fresh-{prefix}.json",
            args.timeout_seconds, input_flag=input_flag)
        difference = first_difference(gpu["batches"][-1], cpu["functional"])
        require(difference is None, f"prefix {prefix}: {difference}")
        require(len(gpu["batches"]) == prefix and gpu["native_final_rollback_completed"],
                "GPU prefix did not complete every batch and actual rollback")
        for ordinal, telemetry in enumerate(gpu["batch_timings"]):
            require(telemetry["generation_before"] == ordinal
                    and telemetry["generation_after"] == ordinal + 1,
                    "GPU prefix generation is not contiguous")
            require(telemetry["h2d_bytes"] > 0 and telemetry["d2h_bytes"] > 0
                    and telemetry["allocations"] > 0 and telemetry["kernel_launches"] >= 2,
                    "GPU batch omitted full allocation/copy/compute")
        verdicts.append({"prefix": prefix, "equivalent": True,
                         "cuda_process_ms": gpu_ms, "fresh_process_ms": cpu_ms})
        # Compare earlier observations from this cumulative run with the prior
        # independent prefix, so repeat/cold state cannot silently diverge.
        if prefix > 1:
            previous = json.loads((root / f"cuda-{prefix - 1}.json").read_text())
            require(first_difference(gpu["batches"][:-1], previous["batches"]) is None,
                    "repeated CUDA prefix changed earlier batch values")
    if len(batches) >= 2:
        recovered, _ = run_child(probe, "rollback", trace, len(batches),
                                  root / "rollback.json", args.timeout_seconds,
                                   input_flag=input_flag)
        require(first_difference(recovered["batches"][-1], gpu["batches"][-1]) is None,
                "healthy replay after failed second batch changed result")
    write_json(root / "verdict.json", {"schema_version": "ista-cuda-full-verdict/1.0.0",
                                       "build": build, "prefixes": verdicts})
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
    parser.add_argument("--samples", type=int, default=3)
    args = parser.parse_args()
    if args.samples <= 0:
        parser.error("samples must be positive")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
