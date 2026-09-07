#!/usr/bin/env python3
"""Release-only Stage 5 raw timing and resource smoke evidence."""

import argparse
import datetime
import json
import os
import pathlib
import platform
import subprocess
import sys
from typing import Any

from dual_csr_timing_evidence import duration_ms
from dual_csr_trace_controller import (
    ControllerError,
    read_json,
    require,
    run_child,
    write_json,
)

SCHEMA = "ista-dual-csr-timing-smoke/2.0.0"
MIN_SAMPLES = 6


def _cpu_model() -> str:
    path = pathlib.Path("/proc/cpuinfo")
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.lower().startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    return platform.processor()


def _environment(
    probe: pathlib.Path,
    trace: pathlib.Path,
    build_config: str,
    compiler_id: str,
    compiler_version: str,
) -> dict[str, Any]:
    affinity = None
    if hasattr(os, "sched_getaffinity"):
        affinity = len(os.sched_getaffinity(0))
    uname = platform.uname()
    return {
        "captured_at_utc": datetime.datetime.now(datetime.UTC).isoformat(),
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


def _run_sample(
    probe: pathlib.Path,
    trace: pathlib.Path,
    batches: list[dict[str, Any]],
    root: pathlib.Path,
    name: str,
    timeout: int,
) -> dict[str, Any]:
    result, wall_ms = run_child(
        probe, "performance", trace, len(batches), root / name / "performance.json", timeout
    )
    require(
        result["schema_version"] == "ista-incregpusta-performance-process/2.0.0",
        f"{name} unsupported performance output",
    )
    require(
        result["debug_checks_enabled"] is False, f"{name} diagnostics enabled in performance child"
    )
    require(
        result["correctness_comparison"] == "not_run",
        f"{name} unexpectedly performed a correctness comparison",
    )
    require(
        result["result"]["generation"] == len(batches)
        and result["result"]["native_final_rollback_completed"] is True,
        f"{name} workload or lifecycle did not complete",
    )
    return {
        "targeted_pid": result["run"]["child_pid"],
        "targeted_wall_ms": duration_ms(wall_ms, f"{name}.wall_ms", require, positive=True),
        "batch_phase_timings": result["run"]["batch_phase_timings"],
        "requested_mode": result["requested_mode"],
        "executed_mode": result["executed_mode"],
        "result": result["result"],
        "input": result["input"],
        "debug_checks_enabled": False,
        "correctness_comparison": "not_run",
    }


def probe_build_info(probe: pathlib.Path, timeout: int) -> dict[str, Any]:
    completed = subprocess.run(
        [str(probe), "build-info"], check=True, capture_output=True, text=True, timeout=timeout
    )
    value = json.loads(completed.stdout)
    require(
        value["debug_checks_enabled"] is False,
        "performance sampling requires ECC_INCREGPUSTA_DEBUG_CHECKS=OFF",
    )
    require(value["build_config"] == "Release", "performance probe must use Release")
    return value


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
    require(
        args.samples >= MIN_SAMPLES, f"timing smoke requires at least {MIN_SAMPLES} raw samples"
    )
    require(args.warmup_runs >= 1, "timing smoke requires a warm-up run")
    probe = args.probe.resolve(strict=True)
    build = probe_build_info(probe, args.timeout_seconds)
    trace = args.trace.resolve(strict=True)
    trace_value = read_json(trace)
    batches = trace_value["replay"]["workload"]["batches"]
    require(
        isinstance(batches, list) and batches, "timing smoke trace must contain at least one batch"
    )
    root = args.run_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    warmups = [
        _run_sample(probe, trace, batches, root, f"warmup-{index + 1}", args.timeout_seconds)
        for index in range(args.warmup_runs)
    ]
    samples: list[dict[str, Any]] = []
    for index in range(args.samples):
        sample = _run_sample(
            probe, trace, batches, root, f"sample-{index + 1}", args.timeout_seconds
        )
        sample["sample_ordinal"] = index + 1
        samples.append(sample)
    raw_sample_count = len(samples)
    output = {
        "schema_version": SCHEMA,
        "trace_is_fixed": True,
        "trace": str(trace),
        "environment_fingerprint": _environment(
            probe, trace, build["build_config"], build["compiler_id"], build["compiler_version"]
        ),
        "warmup": {"runs": args.warmup_runs, "completed": True, "observations": warmups},
        "raw_samples": samples,
        "raw_sample_count": raw_sample_count,
        "build": build,
        "debug_checks_enabled": False,
        "correctness_comparison": "not_run",
        "performance_claims_made": False,
        "speedup_or_break_even_reported": False,
    }
    require(raw_sample_count >= MIN_SAMPLES, "timing smoke did not retain six raw samples")
    write_json(args.output.resolve(), output)


def main() -> int:
    try:
        args = parse_args()
        if args.build_config != "Release":
            print("dual CSR timing smoke: skipped outside Release")
            return 77
        run(args)
        return 0
    except (
        ControllerError,
        OSError,
        KeyError,
        TypeError,
        ValueError,
        subprocess.SubprocessError,
    ) as error:
        print(f"dual CSR timing smoke: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
