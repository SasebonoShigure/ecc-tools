#!/usr/bin/env python3
"""Release/OFF frozen two-mode final-only timing experiments."""

import argparse
import datetime
import json
import os
import pathlib
import platform
import subprocess
import sys
from typing import Any

from trace_process import (
    ControllerError,
    read_json,
    require,
    run_child,
    write_json,
)


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
        "probe_sha256": _file_reference(probe)["sha256"],
        "cuda_visible_devices": os.environ.get("CUDA_VISIBLE_DEVICES"),
        "trace": str(trace),
        "ld_library_path": os.environ.get("LD_LIBRARY_PATH", ""),
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
    return value


def _file_reference(path: pathlib.Path) -> dict[str, str]:
    import hashlib
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": str(path), "sha256": digest}


def _verified_file(value: dict[str, str]) -> pathlib.Path:
    path = pathlib.Path(value["path"]).resolve(strict=True)
    require(_file_reference(path)["sha256"] == value["sha256"],
            f"frozen input changed: {path}")
    return path


def run_consumer_experiment(args: argparse.Namespace, probe: pathlib.Path,
                            build: dict[str, Any]) -> bool:
    # The existing schema/semantic validator is also the statistics implementation.
    schema_root = pathlib.Path(__file__).resolve().parents[2] / "benchmark" / "schema"
    sys.path.insert(0, str(schema_root))
    from consumer_trace_samples import (
        BOOTSTRAP_RESAMPLES,
        BOOTSTRAP_SEED,
        SECONDARY_TIMING_METRICS,
        TIMING_MODES,
        TIMING_PAIRS,
        consumer_statistics,
        validate_consumer_measurement,
    )
    from validate_incremental_sta_samples import validate_document

    manifest_path = args.manifest.resolve(strict=True)
    manifest_identity = _file_reference(manifest_path)
    manifest = read_json(manifest_path)
    require(manifest["state"] == "frozen", "freeze inputs and host calibration before sampling")
    matches = [case for case in manifest["cases"] if case["name"] == args.case]
    require(len(matches) == 1, "--case must select exactly one frozen case")
    selected = matches[0]
    trace = _verified_file(selected["trace"])
    for source in manifest["input_files"]:
        _verified_file(source)
    profile = manifest["thread_execution_evidence"]
    _verified_file(profile)
    prefix = selected["prefix_batches"]
    workload = read_json(trace)
    require(workload["schema_version"] == "ista-scale-compact-replay/2",
            "compact input v2 required")
    require(prefix == len(workload["batches"]),
            "consumer experiments require the complete frozen trace")
    del workload
    case = {key: selected[key] for key in
            ("name", "trace", "prefix_batches", "threads", "cpu_affinity", "mandatory")}
    require(type(prefix) is int and prefix > 0, "invalid frozen prefix")
    require(type(case["threads"]) is int and case["threads"] > 0, "invalid frozen threads")
    affinity = case["cpu_affinity"]
    require(isinstance(affinity, list) and bool(affinity)
            and all(type(cpu) is int and cpu >= 0 for cpu in affinity)
            and affinity == sorted(set(affinity)), "invalid frozen CPU affinity")
    root = args.run_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    environment = _environment(probe, trace, build["build_config"],
                               build["compiler_id"], build["compiler_version"])
    samples = []
    result_requirements = None
    for warmup, count in ((True, args.warmup_runs), (False, args.samples)):
        for repetition in range(1, count + 1):
            # Fixed round-robin order; matching repetition is the pairing key.
            for mode in TIMING_MODES:
                backend, strategy = mode.split("/")
                name = f'{"warmup" if warmup else "sample"}-{repetition}/{backend}-{strategy}'
                raw_path = root / name / "consumer.json"
                require(not raw_path.parent.exists(),
                        f"refusing to overwrite evidence: {raw_path.parent}")
                raw, wall_ms = run_child(
                    probe, "benchmark", trace, prefix, raw_path, args.timeout_seconds,
                    input_flag="--replay", extra_args=(
                        "--backend", backend, "--strategy", strategy,
                        "--threads", str(case["threads"]),
                    ), cpu_affinity=affinity,
                )
                require(raw["build"] == build, "child build differs from build-info")
                measurement = validate_consumer_measurement(
                    raw, mode, prefix, case["threads"], affinity)
                result_requirements = raw["result_requirements"]
                samples.append({
                    "mode": mode, "warmup": warmup, "repetition": repetition,
                    "raw_file": _file_reference(raw_path),
                    "child_wall_ms": wall_ms, **measurement,
                })
                # Retain completed work even if a later child fails.
                write_json(root / "completed-samples.json", samples)
    require(_file_reference(probe)["sha256"] == environment["probe_sha256"],
            "benchmark executable changed during sampling")
    for source in (manifest_identity, selected["trace"], profile, *manifest["input_files"]):
        _verified_file(source)
    summaries, comparisons = consumer_statistics(
        samples, "consumer_loop_ms", modes=TIMING_MODES, pairs=TIMING_PAIRS)
    additional_metrics = {}
    for metric in SECONDARY_TIMING_METRICS:
        phase_summaries, phase_comparisons = consumer_statistics(
            samples, metric, modes=TIMING_MODES, pairs=TIMING_PAIRS)
        additional_metrics[metric] = {
            "summaries": phase_summaries, "comparisons": phase_comparisons}
    engineering = args.warmup_runs >= 5 and args.samples >= 30
    output = {
        "schema_version": "3.0.0", "manifest": manifest_identity,
        "report_schedule": "final_only", "primary_metric": "consumer_loop_ms",
        "case": case, "build": build, "thread_execution_evidence": profile,
        "environment": environment,
        "protocol": {
            "mode_order": list(TIMING_MODES), "warmups": args.warmup_runs,
            "repetitions": args.samples, "bootstrap_seed": BOOTSTRAP_SEED,
            "bootstrap_resamples": BOOTSTRAP_RESAMPLES,
            "pairing_unit": "fresh_process_complete_trace",
        },
        "result_requirements": result_requirements,
        "debug_checks_enabled": False, "correctness_comparison": "not_run",
        "diagnostics": "not_run",
        "sampling_kind": "engineering" if engineering else "short_smoke",
        "samples": samples, "summaries": summaries, "comparisons": comparisons,
        "additional_metrics": additional_metrics,
        "performance_gate": (
            "passed" if all(value["lower_bound_above_one"] for value in comparisons.values())
            else "failed"
        ) if engineering else "not_run",
        "stage_acceptance": "not_run",
    }
    validate_document(output, read_json(schema_root / "incremental_sta_samples_v3.schema.json"))
    write_json(args.output.resolve(), output)
    return True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--case", required=True, help="case name in the frozen experiment manifest")
    parser.add_argument("--run-root", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--warmup-runs", type=int, default=5)
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    return parser.parse_args()


def run(args: argparse.Namespace) -> bool:
    require(args.samples >= 1, "timing smoke requires at least one raw sample")
    require(args.warmup_runs >= 1, "timing smoke requires a warm-up run")
    probe = args.probe.resolve(strict=True)
    build = probe_build_info(probe, args.timeout_seconds)
    if build["build_config"] != "Release":
        return False
    return run_consumer_experiment(args, probe, build)


def main() -> int:
    try:
        args = parse_args()
        if not run(args):
            print("dual CSR timing smoke: skipped outside Release")
            return 77
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
