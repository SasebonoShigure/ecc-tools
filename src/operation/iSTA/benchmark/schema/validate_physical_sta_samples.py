#!/usr/bin/env python3
"""Validate paired physical loops with one final artifact publication."""

import argparse
import json
import math
from pathlib import Path

from consumer_trace_samples import (
    BOOTSTRAP_RESAMPLES,
    BOOTSTRAP_SEED,
    MODES,
    TIMING_MODES,
    consumer_statistics,
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def duration(value):
    require(
        type(value) in (int, float) and math.isfinite(value) and value >= 0,
        "elapsed time must be finite and nonnegative",
    )
    return value


def validate_sample(value: dict) -> None:
    require(
        value["schema_version"] in ("physical-sta-sample/2.0.0", "physical-sta-sample/3.0.0"),
        "sample schema differs",
    )
    modes = MODES if value["schema_version"] == "physical-sta-sample/2.0.0" else TIMING_MODES
    require(
        value["debug_checks_enabled"] is False
        and value["build"]["debug_checks_enabled"] is False
        and value["build"]["build_config"] == "Release",
        "performance requires Release/OFF",
    )
    require(
        value["correctness_comparison"] == value["diagnostics"] == "not_run",
        "performance must not run diagnostic comparisons",
    )
    require(value["lifecycle_cleanup_completed"] is True, "sample cleanup did not complete")
    require(
        value["workload"] == "fixed_trace_consumption"
        and value["placement_cost"] == "positions_frozen_before_sampling"
        and value["stop_reason"] == "trace_completed",
        "workload consumption differs",
    )
    require(
        type(value["threads_requested"]) is int
        and value["threads_requested"] > 0
        and value["thread_activity"] == "external_profile",
        "missing requested threads or actual thread evidence boundary",
    )
    affinity = value["cpu_affinity"]
    require(
        isinstance(affinity, list)
        and bool(affinity)
        and all(type(cpu) is int and cpu >= 0 for cpu in affinity)
        and affinity == sorted(set(affinity)),
        "invalid CPU affinity",
    )
    request = value["requested"]
    mode = f"{request['backend']}/{request['update_strategy']}"
    require(mode in modes and request["fallback_policy"] == "error", "invalid performance request")
    requirements = value["result_requirements"]
    require(
        requirements["per_round"] == "candidate_inputs"
        and requirements["final"] == "timing_qor_paths_power_sdf_def_netlist"
        and requirements["max_paths"] == requirements["nworst"] == 20
        and requirements["top_k"] == 100
        and requirements["max_buffers"] == 1000,
        "result requirements differ",
    )
    cells = requirements["candidate_cells"]
    require(
        isinstance(cells, list)
        and all(isinstance(cell, str) and cell for cell in cells)
        and cells == sorted(set(cells)),
        "candidate cells are not a frozen ordered set",
    )
    batches = value["batch_timings"]
    require(isinstance(batches, list) and bool(batches), "missing physical batches")
    for generation, batch in enumerate(batches, 1):
        execution = batch["execution"]
        require(
            batch["generation"] == generation
            and execution["fallback_to_full"] is False
            and execution["executed_backend"] == request["backend"]
            and execution["executed_update_strategy"] == request["update_strategy"],
            "batch generation or actual mode differs",
        )
        if request["update_strategy"] == "incremental":
            require(execution["incremental_batch_executed"] is True, "baseline is not an edit")
        require(
            batch["explicit_complete_report_request"] == "not_run",
            "unexpected explicit per-round complete report request",
        )
        require(
            type(batch["buffer_count"]) is int
            and batch["buffer_count"] > 0
            and type(batch["candidate_count"]) is int
            and batch["candidate_count"] >= 0,
            "invalid physical edit/selection counts",
        )
        phases = duration(batch["candidate_read_ms"]) + duration(batch["commit_ms"])
        require(
            math.isclose(duration(batch["step_ms"]), phases, rel_tol=1e-9, abs_tol=1e-6),
            "batch timing omits completed phases",
        )
    require(
        sum(batch["buffer_count"] for batch in batches) <= requirements["max_buffers"],
        "fixed trace exceeds frozen budget",
    )
    generation = len(batches)
    artifact = value["artifacts"]
    require(
        value["result"]["generation"] == artifact["generation"] == generation,
        "result or artifact generation differs",
    )
    require(
        artifact["schema_version"] == "physical-sta-artifacts/1.0.0"
        and artifact["report_request"]["max_paths"] == 20
        and artifact["report_request"]["nworst"] == 20,
        "artifact consumer request differs",
    )
    phases = (
        sum(batch["step_ms"] for batch in batches)
        + duration(value["final_query_ms"])
        + duration(value["final_artifact_ms"])
    )
    total = duration(value["optimization_end_to_end_ms"])
    require(total > 0 and total + 1e-6 >= phases, "loop timing omits final consumption")
    lifecycle = total + sum(
        duration(value[key]) for key in ("load_ms", "initialize_baseline_ms", "cleanup_ms")
    )
    require(
        duration(value["session_end_to_end_ms"]) + 1e-6 >= lifecycle,
        "session timing omits setup or cleanup",
    )


def validate_document(document: dict) -> tuple[dict, dict]:
    require(
        document["schema_version"] in ("physical-sta-samples/2.0.0", "physical-sta-samples/3.0.0"),
        "summary schema differs",
    )
    historical = document["schema_version"] == "physical-sta-samples/2.0.0"
    modes = MODES if historical else TIMING_MODES
    sample_schema = "physical-sta-sample/2.0.0" if historical else "physical-sta-sample/3.0.0"
    pairs = (
        (("cpu/full", "cuda/full"), ("cuda/full", "cuda/incremental"))
        if historical
        else (("cpu/full", "cuda/incremental"),)
    )
    protocol = document["protocol"]
    warmups, repeats = protocol["warmups"], protocol["repetitions"]
    require(
        type(warmups) is int and warmups >= 0 and type(repeats) is int and repeats > 0,
        "invalid sample counts",
    )
    require(protocol["mode_order"] == list(modes), "mode order differs from frozen rotation")
    require(
        protocol["bootstrap_seed"] == BOOTSTRAP_SEED
        and protocol["bootstrap_resamples"] == BOOTSTRAP_RESAMPLES
        and protocol["pairing_unit"] == "fresh_process_complete_physical_loop",
        "statistical protocol differs",
    )
    kind = "engineering" if warmups >= 5 and repeats >= 30 else "short_smoke"
    require(
        document["sampling_kind"] == kind and document["stage15_acceptance"] == "not_run",
        "sample classification or acceptance claim differs",
    )
    expected = [
        (warmup, repetition, mode)
        for warmup, count in ((True, warmups), (False, repeats))
        for repetition in range(1, count + 1)
        for mode in modes
    ]
    samples = document["samples"]
    actual = [(s["warmup"], s["repetition"], s["mode"]) for s in samples]
    require(actual == expected, "missing, duplicated, reordered or unpaired loop samples")
    baseline = samples[0]["measurement"]
    rows = []
    for sample in samples:
        value = sample["measurement"]
        require(value["schema_version"] == sample_schema, "sample and summary schema differ")
        validate_sample(value)
        require(
            sample["mode"]
            == f"{value['requested']['backend']}/{value['requested']['update_strategy']}",
            "sample mode differs from actual request",
        )
        for key in (
            "build",
            "binding",
            "inputs",
            "trace",
            "result_requirements",
            "threads_requested",
            "cpu_affinity",
        ):
            require(value[key] == baseline[key], f"paired samples have different {key}")
        require(
            len(value["batch_timings"]) == len(baseline["batch_timings"]),
            "paired samples have different trace lengths",
        )
        require(
            duration(sample["process_wall_ms"]) + 1e-6 >= value["session_end_to_end_ms"],
            "process wall time omits session lifetime",
        )
        rows.append(
            {
                "mode": sample["mode"],
                "warmup": sample["warmup"],
                "optimization_end_to_end_ms": value["optimization_end_to_end_ms"],
            }
        )
    summaries, comparisons = consumer_statistics(
        rows, metric="optimization_end_to_end_ms", modes=modes, pairs=pairs
    )
    require(
        document["summaries"] == summaries and document["comparisons"] == comparisons,
        "saved statistics differ from paired raw samples",
    )
    return summaries, comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("documents", type=Path, nargs="+")
    args = parser.parse_args()
    for path in args.documents:
        value = json.loads(path.read_text())
        if value["schema_version"] in ("physical-sta-sample/2.0.0", "physical-sta-sample/3.0.0"):
            validate_sample(value)
        else:
            validate_document(value)
        print(f"valid: {path}")


if __name__ == "__main__":
    main()
