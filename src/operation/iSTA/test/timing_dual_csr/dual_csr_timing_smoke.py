#!/usr/bin/env python3
"""Release-only Stage 5 raw timing and resource smoke evidence."""

import argparse
import datetime
import os
import pathlib
import platform
import sys
from typing import Any

from dual_csr_timing_evidence import duration_ms
from dual_csr_trace_controller import (
    ControllerError, first_difference, normalized_source, read_json, require,
    run_child, validate_fresh, validate_targeted, write_json,
)

SCHEMA = "ista-dual-csr-timing-smoke/1.0.0"
MIN_SAMPLES = 6


def _cpu_model() -> str:
    path = pathlib.Path("/proc/cpuinfo")
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.lower().startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    return platform.processor()


def _environment(probe: pathlib.Path, trace: pathlib.Path,
                 build_config: str, compiler_id: str,
                 compiler_version: str) -> dict[str, Any]:
    affinity = None
    if hasattr(os, "sched_getaffinity"):
        affinity = len(os.sched_getaffinity(0))
    uname = platform.uname()
    return {
        "captured_at_utc": datetime.datetime.now(
            datetime.timezone.utc).isoformat(),
        "build_config": build_config,
        "compiler_id": compiler_id,
        "compiler_version": compiler_version,
        "platform": platform.platform(),
        "system": uname.system,
        "release": uname.release,
        "machine": uname.machine,
        "cpu_model": _cpu_model(),
        "logical_cpu_count": os.cpu_count(),
        "process_affinity_cpu_count": affinity,
        "python_version": platform.python_version(),
        "python_executable": str(pathlib.Path(sys.executable).resolve()),
        "probe": str(probe),
        "probe_size_bytes": probe.stat().st_size,
        "trace": str(trace),
        "ld_library_path": os.environ.get("LD_LIBRARY_PATH", ""),
    }


def _run_sample(probe: pathlib.Path, trace: pathlib.Path,
                batches: list[dict[str, Any]], root: pathlib.Path,
                name: str, timeout: int) -> dict[str, Any]:
    prefix = len(batches)
    targeted, targeted_wall = run_child(
        probe, "targeted", trace, prefix, root / name / "targeted.json",
        timeout)
    fresh, fresh_wall = run_child(
        probe, "fresh", trace, prefix, root / name / "fresh.json", timeout)
    targeted_functional, timings, resources = validate_targeted(
        targeted, prefix, batches, f"{name}.targeted")
    fresh_functional = validate_fresh(fresh, prefix, f"{name}.fresh")
    mismatch = first_difference(targeted_functional, fresh_functional)
    require(mismatch is None, f"{name} functional mismatch: {mismatch}")
    targeted_source = normalized_source(
        targeted["source_isolation"], f"{name}.targeted.source")
    fresh_source = normalized_source(
        fresh["source_isolation"], f"{name}.fresh.source")
    mismatch = first_difference(targeted_source, fresh_source)
    require(mismatch is None, f"{name} source mismatch: {mismatch}")
    targeted_pid = targeted["run"]["child_pid"]
    fresh_pid = fresh["run"]["child_pid"]
    require(targeted_pid != fresh_pid
            and targeted_pid != os.getpid() and fresh_pid != os.getpid(),
            f"{name} did not use two isolated child processes")
    transaction = targeted["exact_receipt"]["transaction"]
    evidence = transaction["evidence"]
    counters = evidence["operation_counters"]
    require(evidence["native_final_rollback_completed"] is True
            and counters["native_full_graph_exports"] == 0
            and counters["native_full_reference_exports"] == 0
            and counters["host_snapshot_rebuilds"] == 0
            and counters["fallback_calls"] == 0,
            f"{name} targeted execution/rollback evidence differs")
    return {
        "targeted_pid": targeted_pid,
        "fresh_pid": fresh_pid,
        "targeted_wall_ms": duration_ms(
            targeted_wall, f"{name}.targeted_wall_ms", require,
            positive=True),
        "fresh_oracle_ms": duration_ms(
            fresh_wall, f"{name}.fresh_oracle_ms", require, positive=True),
        "batch_phase_timings": timings,
        "batch_resource_observations": resources,
        "phase_trace": evidence["phase_trace"],
        "functional_fields_equal_with_stage2_tolerance": True,
        "source_isolation_equal": True,
        "native_final_rollback_completed": True,
        "targeted_region_full_export_count": 0,
        "targeted_region_fallback_count": 0,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--trace", required=True, type=pathlib.Path)
    parser.add_argument("--run-root", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--build-config", required=True)
    parser.add_argument("--compiler-id", required=True)
    parser.add_argument("--compiler-version", required=True)
    parser.add_argument("--warmup-runs", type=int, default=1)
    parser.add_argument("--samples", type=int, default=MIN_SAMPLES)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    return parser.parse_args()


def run(args: argparse.Namespace) -> None:
    require(args.samples >= MIN_SAMPLES,
            f"timing smoke requires at least {MIN_SAMPLES} raw samples")
    require(args.warmup_runs >= 1, "timing smoke requires a warm-up run")
    probe = args.probe.resolve(strict=True)
    trace = args.trace.resolve(strict=True)
    trace_value = read_json(trace)
    batches = trace_value["replay"]["workload"]["batches"]
    require(isinstance(batches, list) and batches,
            "timing smoke trace must contain at least one batch")
    root = args.run_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    warmups = [
        _run_sample(probe, trace, batches, root, f"warmup-{index + 1}",
                    args.timeout_seconds)
        for index in range(args.warmup_runs)
    ]
    samples: list[dict[str, Any]] = []
    for index in range(args.samples):
        sample = _run_sample(
            probe, trace, batches, root, f"sample-{index + 1}",
            args.timeout_seconds)
        sample["sample_ordinal"] = index + 1
        samples.append(sample)
    output = {
        "schema_version": SCHEMA,
        "trace_is_fixed": True,
        "trace": str(trace),
        "environment_fingerprint": _environment(
            probe, trace, args.build_config, args.compiler_id,
            args.compiler_version),
        "warmup": {"runs": args.warmup_runs,
                   "completed": True,
                   "observations": warmups},
        "raw_samples": samples,
        "raw_sample_count": len(samples),
        "functional_correctness_primary": True,
        "performance_claims_made": False,
        "speedup_or_break_even_reported": False,
    }
    require(output["raw_sample_count"] >= MIN_SAMPLES,
            "timing smoke did not retain six raw samples")
    write_json(args.output.resolve(), output)


def main() -> int:
    try:
        args = parse_args()
        if args.build_config != "Release":
            print("dual CSR timing smoke: skipped outside Release")
            return 77
        run(args)
        return 0
    except (ControllerError, OSError, KeyError, TypeError, ValueError) as error:
        print(f"dual CSR timing smoke: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
