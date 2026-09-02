#!/usr/bin/env python3
"""Three-process Stage 5 frozen-trace functional acceptance controller."""

import argparse
import concurrent.futures
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile
import time
from typing import Any

from dual_csr_arc_group_validator import validate_arc_group_index
from dual_csr_timing_evidence import (
    duration_ms, validate_batch_phase_timings,
    validate_resource_observations)
from dual_csr_trace_controller_selftest import run_controller_self_test
from dual_csr_transaction_validator import validate_transaction as validate_full_transaction

WIRE_SCHEMA = "ista-dual-csr-trace-process/1.0.0"
EXACT_SCHEMA = "ista-dual-csr-exact-receipt/1.3.0"
ABS_TOLERANCE = 1.0e-9
REL_TOLERANCE = 1.0e-9


class ControllerError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ControllerError(message)


def read_json(path: pathlib.Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(value, dict), f"{path} must contain a JSON object")
    return value


def write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def numeric_equal(left: object, right: object) -> bool:
    if isinstance(left, bool) or isinstance(right, bool):
        return False
    if not isinstance(left, (int, float)) or not isinstance(right, (int, float)):
        return False
    if isinstance(left, int) and isinstance(right, int):
        return left == right
    return math.isclose(
        float(left), float(right), rel_tol=REL_TOLERANCE,
        abs_tol=ABS_TOLERANCE,
    )


def first_difference(left: object, right: object, path: str = "$") -> str | None:
    if type(left) is not type(right):
        if numeric_equal(left, right):
            return None
        return f"{path}: type {type(left).__name__} != {type(right).__name__}"
    if isinstance(left, dict):
        if set(left) != set(right):
            return f"{path}: keys {sorted(left)} != {sorted(right)}"
        for key in sorted(left):
            difference = first_difference(left[key], right[key], f"{path}.{key}")
            if difference:
                return difference
        return None
    if isinstance(left, list):
        if len(left) != len(right):
            return f"{path}: length {len(left)} != {len(right)}"
        for index, (lhs, rhs) in enumerate(zip(left, right, strict=True)):
            difference = first_difference(lhs, rhs, f"{path}[{index}]")
            if difference:
                return difference
        return None
    if isinstance(left, float) and numeric_equal(left, right):
        return None
    if left != right:
        return f"{path}: {left!r} != {right!r}"
    return None


def require_exact_keys(value: object, keys: set[str], label: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(set(value) == keys, f"{label} fields differ: {sorted(value)}")
    return value


def require_count(value: object, label: str) -> int:
    require(type(value) is int and value >= 0,
            f"{label} must be a nonnegative integer")
    return value


def validate_extent(value: object, logical: int, label: str) -> None:
    item = require_exact_keys(value, {"logical_size", "capacity"}, label)
    require(item["logical_size"] == logical, f"{label}.logical_size differs")
    require(item["capacity"] >= logical, f"{label}.capacity is insufficient")


def validate_offsets(offsets: object, rows: int, entries: int, label: str) -> None:
    require(isinstance(offsets, list), f"{label} must be an array")
    require(len(offsets) == rows + 1, f"{label} row count differs")
    require(offsets and offsets[0] == 0 and offsets[-1] == entries,
            f"{label} endpoints differ")
    require(all(isinstance(item, int) and item >= 0 for item in offsets),
            f"{label} contains a non-index")
    require(all(left <= right for left, right in zip(offsets, offsets[1:])),
            f"{label} is not monotonic")


def validate_base(value: object, node_slots: int, label: str) -> set[int]:
    item = require_exact_keys(
        value,
        {"row_offsets", "physical_slots", "offsets_extent", "edge_extent"},
        label,
    )
    slots = item["physical_slots"]
    require(isinstance(slots, list), f"{label}.physical_slots must be an array")
    validate_offsets(item["row_offsets"], node_slots, len(slots),
                     f"{label}.row_offsets")
    validate_extent(item["offsets_extent"], node_slots + 1,
                    f"{label}.offsets_extent")
    validate_extent(item["edge_extent"], len(slots), f"{label}.edge_extent")
    active = {slot for slot in slots if slot is not None}
    require(all(isinstance(slot, int) and slot >= 0 for slot in active),
            f"{label} contains an invalid ArcId")
    require(len(active) == sum(slot is not None for slot in slots),
            f"{label} contains duplicate active ArcIds")
    return active


def validate_aux(value: object, node_slots: int, label: str) -> set[int]:
    keys = {
        "row_offsets", "row_sources", "arc_ids", "source_to_aux_row",
        "offsets_extent", "source_extent", "edge_extent", "mapping_extent",
    }
    item = require_exact_keys(value, keys, label)
    sources = item["row_sources"]
    arcs = item["arc_ids"]
    mapping = item["source_to_aux_row"]
    require(isinstance(sources, list) and isinstance(arcs, list)
            and isinstance(mapping, list), f"{label} arrays are malformed")
    validate_offsets(item["row_offsets"], len(sources), len(arcs),
                     f"{label}.row_offsets")
    require(len(mapping) == node_slots, f"{label}.source_to_aux_row size differs")
    require(len(set(sources)) == len(sources), f"{label} repeats a source row")
    for row, source in enumerate(sources):
        require(0 <= source < node_slots and mapping[source] == row,
                f"{label} source mapping is not inverse")
    require(all(value == -1 or 0 <= value < len(sources) for value in mapping),
            f"{label} source mapping has an invalid row")
    require(len(set(arcs)) == len(arcs), f"{label} repeats an ArcId")
    validate_extent(item["offsets_extent"], len(sources) + 1,
                    f"{label}.offsets_extent")
    validate_extent(item["source_extent"], len(sources),
                    f"{label}.source_extent")
    validate_extent(item["edge_extent"], len(arcs), f"{label}.edge_extent")
    validate_extent(item["mapping_extent"], len(mapping),
                    f"{label}.mapping_extent")
    return set(arcs)


def validate_telemetry(value: object, generation: int, label: str) -> None:
    require(isinstance(value, dict), f"{label} must be an object")
    required = {
        "generation_before", "generation_after", "logical_edges_before",
        "logical_edges_after", "base_physical_edges", "base_active_edges",
        "base_tombstones", "aux_rows", "aux_edges",
        "tombstone_reuse_count", "base_change_count", "reverse_change_count",
        "nodes_appended", "arcs_appended", "payload_change_count",
        "aux_rebuild_count", "full_rebuild_count", "fallback_count",
        "capacity_grow_events", "traversal",
    }
    require(set(value) == required, f"{label} fields differ")
    require(value["generation_after"] == generation,
            f"{label}.generation_after differs")
    require(value["full_rebuild_count"] == 0 and value["fallback_count"] == 0,
            f"{label} observed a rebuild/fallback")
    for index, event in enumerate(value["capacity_grow_events"]):
        item = require_exact_keys(
            event, {"buffer", "old_capacity", "required", "new_capacity"},
            f"{label}.capacity_grow_events[{index}]",
        )
        require(item["new_capacity"] >= item["required"]
                and item["new_capacity"] >= item["old_capacity"],
                f"{label} contains an invalid capacity grow")


def validate_structural(value: object, generation: int, label: str) -> dict[str, Any]:
    item = require_exact_keys(
        value,
        {"generation", "high_water", "arc_group_index", "inactive_arcs",
         "base_out", "base_in", "aux_out", "update_telemetry"},
        label,
    )
    require(item["generation"] == generation, f"{label}.generation differs")
    high = require_exact_keys(
        item["high_water"], {"node_slots", "net_slots", "arc_slots"},
        f"{label}.high_water",
    )
    require(high["node_slots"] > 0 and high["arc_slots"] > 0,
            f"{label} has an empty high-water")
    out = validate_base(item["base_out"], high["node_slots"], f"{label}.base_out")
    incoming = validate_base(item["base_in"], high["node_slots"], f"{label}.base_in")
    aux = validate_aux(item["aux_out"], high["node_slots"], f"{label}.aux_out")
    active = out | aux
    require(not out & aux, f"{label} BaseOut and AuxOut overlap")
    require(active == incoming, f"{label} forward/reverse active ArcIds differ")
    inactive = item["inactive_arcs"]
    require(isinstance(inactive, list), f"{label}.inactive_arcs must be an array")
    inactive_ids = [entry["id"] for entry in inactive]
    require(len(set(inactive_ids)) == len(inactive_ids),
            f"{label} repeats an inactive ArcId")
    require(not active & set(inactive_ids), f"{label} exposes inactive adjacency")
    require(active | set(inactive_ids) == set(range(high["arc_slots"])),
            f"{label} does not cover every stable ArcId")
    for position, entry in enumerate(inactive):
        inactive_item = require_exact_keys(
            entry, {"id", "historical_payload"},
            f"{label}.inactive_arcs[{position}]",
        )
        require(isinstance(inactive_item["historical_payload"], dict),
                f"{label}.inactive_arcs[{position}] lacks semantic payload")
    validate_arc_group_index(item["arc_group_index"], generation,
                             high["arc_slots"], active,
                             f"{label}.arc_group_index", require)
    telemetry = item["update_telemetry"]
    validate_telemetry(telemetry, generation, f"{label}.update_telemetry")
    require(telemetry["logical_edges_after"] == len(active),
            f"{label} logical edge telemetry differs")
    require(telemetry["base_physical_edges"] == len(item["base_out"]["physical_slots"]),
            f"{label} physical edge telemetry differs")
    require(telemetry["base_active_edges"] == len(out)
            and telemetry["base_tombstones"]
            == len(item["base_out"]["physical_slots"]) - len(out),
            f"{label} Base telemetry differs")
    require(telemetry["aux_rows"] == len(item["aux_out"]["row_sources"])
            and telemetry["aux_edges"] == len(aux),
            f"{label} Aux telemetry differs")
    return item


def normalized_source(value: object, label: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(value.get("source_unchanged") is True
            and value.get("artifacts_before") == value.get("artifacts_after")
            and value.get("idb_before") == value.get("idb_after"),
            f"{label} source evidence changed")
    result = json.loads(json.dumps(value))
    for field in ("artifacts_before", "artifacts_after"):
        for artifact in result[field]:
            require(pathlib.Path(artifact["canonical_path"]).is_absolute(),
                    f"{label} contains a non-absolute source path")
            del artifact["canonical_path"]
    return result


def validate_fresh(value: dict[str, Any], prefix: int, label: str) -> dict[str, Any]:
    require(value.get("schema_version") == WIRE_SCHEMA
            and value.get("role") == "fresh-oracle"
            and value.get("prefix_batches") == prefix,
            f"{label} envelope differs")
    run = value.get("run")
    require(isinstance(run, dict) and run.get("child_pid", 0) > 0
            and run.get("loader_invocation_count") == 1
            and run.get("guarded_work_invocation_count") == 1
            and run.get("one_shot_state") == "finished",
            f"{label} one-shot evidence differs")
    require(isinstance(value.get("functional"), dict),
            f"{label} functional receipt is absent")
    normalized_source(value.get("source_isolation"), f"{label}.source_isolation")
    return value["functional"]


def validate_targeted(value: dict[str, Any], prefix: int,
                      expected_batches: list[dict[str, Any]],
                      label: str) -> tuple[dict[str, Any], list[dict[str, Any]],
                                           list[dict[str, Any]]]:
    require(value.get("schema_version") == WIRE_SCHEMA
            and value.get("role") == "targeted-under-test"
            and value.get("prefix_batches") == prefix,
            f"{label} envelope differs")
    require(value.get("receipt_validated") is True,
            f"{label} did not seal a validated exact receipt")
    exact = value.get("exact_receipt")
    require(isinstance(exact, dict) and exact.get("schema") == EXACT_SCHEMA,
            f"{label} exact receipt schema differs")
    published = exact.get("published")
    require(isinstance(published, dict), f"{label} published receipt is absent")
    structural = validate_structural(
        published.get("structural"), prefix, f"{label}.structural"
    )
    functional = published.get("functional")
    require(isinstance(functional, dict), f"{label} functional receipt is absent")
    validate_full_transaction(
        exact.get("transaction"), prefix, structural, functional, expected_batches,
        f"{label}.transaction", require,
        lambda left, right, path: first_difference(left, right, path),
    )
    observations = value.get("published_observations")
    require(isinstance(observations, list) and len(observations) == prefix,
            f"{label} published observation count differs")
    for index, observation in enumerate(observations):
        require(isinstance(observation, dict)
                and set(observation) == {"functional", "structural"}
                and isinstance(observation["functional"], dict),
                f"{label}.published_observations[{index}] fields differ")
        validate_structural(
            observation["structural"], index + 1,
            f"{label}.published_observations[{index}].structural")
    require(observations[-1] == published,
            f"{label} final published observation differs")
    run = value.get("run")
    require(isinstance(run, dict) and run.get("child_pid", 0) > 0
            and run.get("loader_invocation_count") == 1
            and run.get("guarded_work_invocation_count") == 1
            and run.get("one_shot_state") == "finished"
            and run.get("baseline_factory_calls_outside_targeted") == 1
            and run.get("baseline_factory_calls_inside_targeted") == 0,
            f"{label} one-shot/baseline evidence differs")
    timings = validate_batch_phase_timings(
        run.get("batch_phase_timings"), prefix,
        f"{label}.run.batch_phase_timings", require)
    resources = validate_resource_observations(
        run.get("batch_resource_observations"), observations,
        exact["transaction"], prefix,
        f"{label}.run.batch_resource_observations", require)
    normalized_source(value.get("source_isolation"), f"{label}.source_isolation")
    return functional, timings, resources


def run_child(probe: pathlib.Path, role: str, trace: pathlib.Path, prefix: int,
              output: pathlib.Path,
              timeout: int) -> tuple[dict[str, Any], float]:
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [str(probe), role, "--trace", str(trace), "--prefix", str(prefix),
               "--output", str(output)]
    started = time.perf_counter()
    completed = subprocess.run(
        command, cwd=trace.parent.parent, check=False, capture_output=True,
        text=True, timeout=timeout,
    )
    output.with_suffix(".stdout.txt").write_text(
        completed.stdout, encoding="utf-8")
    output.with_suffix(".stderr.txt").write_text(
        completed.stderr, encoding="utf-8")
    require(completed.returncode == 0,
            f"{' '.join(command)} failed ({completed.returncode})")
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    return read_json(output), elapsed_ms


def replay(args: argparse.Namespace) -> None:
    probe = args.probe.resolve(strict=True)
    trace = args.trace.resolve(strict=True)
    trace_value = read_json(trace)
    batches = trace_value["replay"]["workload"]["batches"]
    require(isinstance(batches, list) and batches,
            "Stage 5 correctness trace must contain a nonempty batch")
    root = pathlib.Path(tempfile.mkdtemp(
        prefix="run-", dir=args.run_root.resolve()
    ))
    summaries: list[dict[str, Any]] = []
    prior_observations: list[dict[str, Any]] = []
    all_pids = {os.getpid()}
    for prefix in range(1, len(batches) + 1):
        target_output = root / f"prefix-{prefix}" / "targeted.json"
        fresh_output = root / f"prefix-{prefix}" / "fresh.json"
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            target_future = executor.submit(
                run_child, probe, "targeted", trace, prefix, target_output,
                args.timeout_seconds,
            )
            fresh_future = executor.submit(
                run_child, probe, "fresh", trace, prefix, fresh_output,
                args.timeout_seconds,
            )
            targeted, targeted_wall_ms = target_future.result()
            fresh, fresh_oracle_ms = fresh_future.result()
        targeted_functional, phase_timings, resources = validate_targeted(
            targeted, prefix, batches[:prefix], f"targeted prefix {prefix}"
        )
        fresh_functional = validate_fresh(fresh, prefix, f"fresh prefix {prefix}")
        difference = first_difference(targeted_functional, fresh_functional)
        require(difference is None,
                f"prefix {prefix} functional mismatch: {difference}")
        target_source = normalized_source(
            targeted["source_isolation"], f"targeted prefix {prefix} source"
        )
        fresh_source = normalized_source(
            fresh["source_isolation"], f"fresh prefix {prefix} source"
        )
        difference = first_difference(target_source, fresh_source)
        require(difference is None, f"prefix {prefix} source mismatch: {difference}")
        pids = {targeted["run"]["child_pid"], fresh["run"]["child_pid"]}
        require(len(pids) == 2 and not pids & all_pids,
                f"prefix {prefix} did not use two new child processes")
        all_pids |= pids
        observations = targeted["published_observations"]
        for index, prior in enumerate(prior_observations):
            difference = first_difference(observations[index], prior)
            require(difference is None,
                    f"prefix {prefix} changed observation {index + 1}: {difference}")
        prior_observations = observations
        summaries.append({
            "prefix_batches": prefix,
            "targeted_pid": targeted["run"]["child_pid"],
            "fresh_pid": fresh["run"]["child_pid"],
            "functional_fields_equal_with_stage2_tolerance": True,
            "structural_receipt_validated": True,
            "transaction_receipt_validated": True,
            "source_isolation_equal": True,
            "targeted_wall_ms": duration_ms(
                targeted_wall_ms, f"prefix {prefix}.targeted_wall_ms", require,
                positive=True),
            "fresh_oracle_ms": duration_ms(
                fresh_oracle_ms, f"prefix {prefix}.fresh_oracle_ms", require,
                positive=True),
            "batch_phase_timings": phase_timings,
            "batch_resource_observations": resources,
        })
    write_json(root / "controller-summary.json", {
        "schema_version": "ista-dual-csr-differential-verdict/1.0.0",
        "trace": str(trace),
        "controller_pid": os.getpid(),
        "all_process_ids_distinct": True,
        "absolute_tolerance": ABS_TOLERANCE,
        "relative_tolerance": REL_TOLERANCE,
        "prefixes": summaries,
        "functional_correctness_primary": True,
        "hash_or_raw_dense_id_used_as_acceptance": False,
        "timing_and_resource_evidence_saved": True,
    })


def self_test() -> None:
    run_controller_self_test(require, first_difference, ControllerError)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--probe", type=pathlib.Path)
    parser.add_argument("--trace", type=pathlib.Path)
    parser.add_argument("--run-root", type=pathlib.Path)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    return parser.parse_args()


def main() -> int:
    try:
        args = parse_args()
        if args.self_test:
            self_test()
        else:
            require(args.probe is not None and args.trace is not None
                    and args.run_root is not None,
                    "--probe, --trace, and --run-root are required")
            args.run_root.mkdir(parents=True, exist_ok=True)
            replay(args)
        return 0
    except (ControllerError, OSError, subprocess.SubprocessError,
            KeyError, TypeError, ValueError) as error:
        print(f"dual CSR trace controller: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
