#!/usr/bin/env python3
"""Isolated Stage 6 mixed-levelization production/fresh controller."""

import argparse
import copy
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from itertools import pairwise
from typing import Any

HERE = pathlib.Path(__file__).resolve().parent
DUAL_CSR_SUPPORT = HERE.parent / "timing_dual_csr"
sys.path.insert(0, str(DUAL_CSR_SUPPORT))

from dual_csr_timing_evidence import duration_ms  # noqa: E402
from dual_csr_trace_controller import (  # noqa: E402
    ABS_TOLERANCE,
    REL_TOLERANCE,
    ControllerError,
    first_difference,
    normalized_source,
    read_json,
    require,
    run_child,
    validate_fresh,
    validate_targeted,
    write_json,
)
from stage6_execution_binding import (  # noqa: E402
    run_execution_binding_canary,
    validate_execution_observation_bindings,
)
from stage6_semantic_trace_exact import (  # noqa: E402
    run_semantic_trace_exact_canary,
    validate_exact_semantic_trace,
)
from stage6_winner_canary import run_equal_cost_canary  # noqa: E402
from stage6_winner_equivalence import (  # noqa: E402
    compare_timing_with_admissible_winners,
)

WIRE_SCHEMA = "ista-stage6-trace-process/5.0.0"
LEVEL_FIELDS = {
    "level_offsets", "level_nodes", "node_to_level",
}
TIMING_VALUE_FIELDS = {
    "schema_version", "nodes", "effective_clock", "physical_clock",
    "data_slews", "paths", "arcs",
}


def exact_object(value: object, keys: set[str], label: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(set(value) == keys, f"{label} fields differ: {sorted(value)}")
    return value


def canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def first_exact_difference(left: object, right: object,
                           path: str = "$") -> str | None:
    if type(left) is not type(right):
        return f"{path}: type {type(left).__name__} != {type(right).__name__}"
    if isinstance(left, dict):
        if set(left) != set(right):
            return f"{path}: keys {sorted(left)} != {sorted(right)}"
        for key in sorted(left):
            difference = first_exact_difference(
                left[key], right[key], f"{path}.{key}")
            if difference:
                return difference
        return None
    if isinstance(left, list):
        if len(left) != len(right):
            return f"{path}: length {len(left)} != {len(right)}"
        for index, (lhs, rhs) in enumerate(zip(left, right, strict=True)):
            difference = first_exact_difference(lhs, rhs, f"{path}[{index}]")
            if difference:
                return difference
        return None
    if isinstance(left, float):
        return None if left.hex() == right.hex() \
            else f"{path}: {left!r} != {right!r}"
    return None if left == right else f"{path}: {left!r} != {right!r}"


def node_keys(graph: dict[str, Any], label: str) -> list[object]:
    nodes = graph.get("nodes")
    require(isinstance(nodes, list), f"{label}.nodes must be an array")
    result: list[object] = []
    for index, node in enumerate(nodes):
        require(isinstance(node, dict) and isinstance(node.get("key"), dict),
                f"{label}.nodes[{index}].key is absent")
        result.append(node["key"])
    encoded = list(map(canonical, result))
    require(len(encoded) == len(set(encoded)), f"{label} repeats a NodeKey")
    return result


def complete_key_permutation(values: object, expected: list[object],
                             label: str) -> list[object]:
    require(isinstance(values, list), f"{label} must be an array")
    encoded = list(map(canonical, values))
    require(len(encoded) == len(expected)
            and len(encoded) == len(set(encoded))
            and set(encoded) == set(map(canonical, expected)),
            f"{label} must be the complete unique NodeKey permutation")
    return values


def validate_offsets(value: object, node_count: int, label: str) -> list[int]:
    require(isinstance(value, list)
            and all(type(item) is int and item >= 0 for item in value),
            f"{label} must be nonnegative integer offsets")
    offsets = value
    require(bool(offsets) and offsets[0] == 0
            and offsets[-1] == node_count,
            f"{label} endpoints do not cover all nodes")
    if node_count == 0:
        require(offsets == [0], f"{label} empty graph form differs")
    else:
        require(len(offsets) >= 2
                and all(left < right
                        for left, right in pairwise(offsets)),
                f"{label} contains an empty or reversed level")
    return offsets


def validate_levelization(graph: dict[str, Any], timing: dict[str, Any],
                          label: str) -> dict[str, Any]:
    item = timing
    graph_nodes = node_keys(graph, f"{label}.graph")
    offsets = validate_offsets(
        item["level_offsets"], len(graph_nodes), f"{label}.level_offsets")
    level_nodes = complete_key_permutation(
        item["level_nodes"], graph_nodes, f"{label}.level_nodes")

    inverse_raw = item["node_to_level"]
    require(isinstance(inverse_raw, list)
            and len(inverse_raw) == len(graph_nodes)
            and all(type(level) is int and 1 <= level < len(offsets)
                    for level in inverse_raw),
            f"{label}.node_to_level is malformed or out of range")
    inverse = dict(zip(map(canonical, graph_nodes), inverse_raw, strict=True))
    for level, (begin, end) in enumerate(
            pairwise(offsets), start=1):
        for position in range(begin, end):
            require(inverse[canonical(level_nodes[position])] == level,
                    f"{label}.node_to_level is not the exact inverse")

    arcs = graph.get("active_arcs")
    require(isinstance(arcs, list), f"{label}.graph.active_arcs is malformed")
    eligible_count = 0
    for index, arc in enumerate(arcs):
        require(isinstance(arc, dict)
                and isinstance(arc.get("eligibility"), dict),
                f"{label}.graph.active_arcs[{index}] is malformed")
        if arc["eligibility"].get("levelization") is not True:
            continue
        eligible_count += 1
        source = canonical(arc.get("source"))
        sink = canonical(arc.get("sink"))
        require(source in inverse and sink in inverse
                and inverse[source] < inverse[sink],
                f"{label}.graph.active_arcs[{index}] violates forward levels")
    return {"level_count": len(offsets) - 1,
            "node_count": len(graph_nodes),
            "eligible_arc_count": eligible_count}


def validate_functional(value: object, label: str
                        ) -> tuple[dict[str, Any], dict[str, Any]]:
    full = exact_object(value, {"graph", "timing"}, label)
    graph = exact_object(full["graph"],
                         {"nodes", "active_arcs", "reference_payload"},
                         f"{label}.graph")
    timing = exact_object(full["timing"], TIMING_VALUE_FIELDS | LEVEL_FIELDS,
                          f"{label}.timing")
    validate_levelization(graph, timing, f"{label}.timing")
    # Shallow views: level layout is checked independently of timing values.
    return ({"graph": graph,
             "timing": {field: timing[field] for field in TIMING_VALUE_FIELDS}},
            timing)


def semantic_edge_classes(graph: dict[str, Any], n_last: int,
                          new_keys: list[object],
                          label: str) -> dict[str, list[object]]:
    nodes = node_keys(graph, label)
    node_domain = set(map(canonical, nodes))
    encoded_new = list(map(canonical, new_keys))
    require(len(encoded_new) == len(set(encoded_new))
            and set(encoded_new) <= node_domain,
            f"{label} descriptor new-node keys are duplicate/out of domain")
    require(len(encoded_new) == len(nodes) - n_last,
            f"{label} descriptor new-node count disagrees with n_last")
    new = set(encoded_new)
    old = node_domain - new
    require(len(old) == n_last, f"{label} exact old NodeKey count differs")
    result = {name: [] for name in ("old_old", "old_new",
                                     "new_old", "new_new")}
    for index, arc in enumerate(graph["active_arcs"]):
        if arc["eligibility"]["levelization"] is not True:
            continue
        source = canonical(arc["source"])
        sink = canonical(arc["sink"])
        require(source in node_domain and sink in node_domain,
                f"{label}.active_arcs[{index}] endpoint is out of domain")
        source_old = source in old
        sink_old = sink in old
        category = ("old" if source_old else "new") + "_" \
            + ("old" if sink_old else "new")
        require(category in result,
                f"{label}.active_arcs[{index}] class is invalid")
        result[category].append(arc["key"])
    return result


def semantic_aux_keys(structural: dict[str, Any], graph: dict[str, Any],
                      label: str) -> list[object]:
    eligible = {canonical(arc["key"]) for arc in graph["active_arcs"]
                if arc["eligibility"]["levelization"] is True}
    aux_ids = {arc for row in structural["aux_out"]["row_arcs"] for arc in row}
    result: list[object] = []
    for slot in structural["arc_group_index"]["slots"]:
        key = slot["current_active_key"]
        if slot["arc_id"] in aux_ids and key is not None \
                and canonical(key) in eligible:
            result.append(key)
    require(len(result) == len(set(map(canonical, result))),
            f"{label} repeats an eligible Aux ArcKey")
    return result


def validate_synthetic_second_batch(transaction: dict[str, Any],
                                    graph: dict[str, Any],
                                    label: str) -> dict[str, list[object]]:
    records = transaction["execution_trace"]
    descriptors = transaction["descriptor_trace"]
    if len(records) < 2:
        return {"batch1_nodes_now_old": [], "batch2_nodes_now_new": []}
    boundary = records[1]["mixed"]["n_last_nodes"]
    batch1 = [entry["key"] for entry in descriptors[0]["node_appends"]]
    batch2 = [entry["key"] for entry in descriptors[1]["node_appends"]]
    nodes = set(map(canonical, node_keys(graph, label)))
    new = set(map(canonical, batch2))
    old = nodes - new
    require(batch1 and batch2
            and len(new) == len(batch2)
            and len(old) == boundary
            and all(canonical(key) in old for key in batch1)
            and all(canonical(key) in new for key in batch2),
            f"{label} descriptor keys do not prove the batch-2 high-water split")
    return {"batch1_nodes_now_old": batch1,
            "batch2_nodes_now_new": batch2}


def validate_mixed_execution(targeted: dict[str, Any], prefix: int,
                             trace_name: str,
                             label: str) -> list[dict[str, Any]]:
    transaction = targeted["transaction_receipt"]
    records = transaction["execution_trace"]
    observations = targeted["published_observations"]
    require(len(records) == prefix == len(observations),
            f"{label} execution/observation cardinality differs")
    bindings = validate_execution_observation_bindings(
        transaction, observations, first_exact_difference, require, label)
    evidence: list[dict[str, Any]] = []
    for index, record in enumerate(records):
        mixed = record["mixed"]
        require(record["mode"] == "mixed_stage6"
                and record["full_levelizer_entry_delta"] == 0
                and record["fallback_count"] == 0
                and record["supplied_levels_validated"] is True
                and isinstance(mixed, dict),
                f"{label}.execution_trace[{index}] is not a sealed mixed run")
        level_traversal = exact_object(
            record["traversal"]["levelization"],
            {"base_out", "aux_out", "base_in"},
            f"{label}.execution_trace[{index}].traversal.levelization")
        require(set(level_traversal.values()) == {0},
                f"{label}.execution_trace[{index}] re-entered full levelization")
        require(mixed["n_new_nodes"] > 0
                and mixed["old_new"] > 0
                and mixed["new_old"] > 0
                and mixed["new_new"] > 0
                and mixed["kahn_processed_count"]
                    == mixed["mixed_vertex_count"]
                and mixed["fallback_count"] == 0,
                f"{label}.execution_trace[{index}] lacks frozen mixed coverage")
        receipt = observations[index]
        graph = receipt["functional"]["graph"]
        descriptor_new = [entry["key"] for entry
                          in transaction["descriptor_trace"][index]
                          ["node_appends"]]
        require(len(descriptor_new) == mixed["n_new_nodes"],
                f"{label}.generation[{index + 1}] n_new differs from descriptor")
        classes = semantic_edge_classes(
            graph, mixed["n_last_nodes"], descriptor_new,
            f"{label}.generation[{index + 1}]")
        for category, keys in classes.items():
            require(len(keys) == mixed[category],
                    f"{label}.generation[{index + 1}] {category} count differs")
        aux = semantic_aux_keys(
            observations[index]["structural"], graph,
            f"{label}.generation[{index + 1}].aux")
        require(len(aux) == mixed["aux_out"],
                f"{label}.generation[{index + 1}] Aux visits differ by ArcKey")
        evidence.append({
            "generation": index + 1,
            "n_last_nodes": mixed["n_last_nodes"],
            "n_new_nodes": mixed["n_new_nodes"],
            "semantic_edge_classes": classes,
            "execution_observation_binding": bindings[index],
            "eligible_aux_arc_keys": aux,
            "kahn_processed_count": mixed["kahn_processed_count"],
            "mixed_vertex_count": mixed["mixed_vertex_count"],
            "full_levelizer_entry_delta": 0,
            "fallback_count": 0,
        })
    if trace_name == "synthetic_two_batch.json" and prefix >= 2:
        evidence[-1]["high_water_classification"] \
            = validate_synthetic_second_batch(
                transaction, observations[-1]["functional"]["graph"], label)
    return evidence


def validate_stage6_targeted(value: dict[str, Any], prefix: int,
                             batches: list[dict[str, Any]], label: str
                             ) -> tuple[dict[str, Any], list[dict[str, Any]],
                                        list[dict[str, Any]], dict[str, Any]]:
    _, timings, resources = validate_targeted(
        value, prefix, batches, label, wire_schema=WIRE_SCHEMA)
    validate_exact_semantic_trace(
        value, batches, first_exact_difference, require, label)
    for index, observation in enumerate(value["published_observations"]):
        functional, levels = validate_functional(
            observation["functional"],
            f"{label}.published_observations[{index}].functional")
    return functional, timings, resources, levels


def validate_stage6_fresh(value: dict[str, Any], prefix: int,
                          label: str) -> tuple[dict[str, Any], dict[str, Any]]:
    full = validate_fresh(value, prefix, label, wire_schema=WIRE_SCHEMA)
    return validate_functional(full, f"{label}.functional")


def compare_functional(targeted: dict[str, Any], fresh: dict[str, Any],
                       label: str) -> dict[str, int]:
    graph_difference = first_exact_difference(
        targeted["graph"], fresh["graph"], f"{label}.graph_reference")
    require(graph_difference is None,
            f"{label} graph/reference mismatch: {graph_difference}")
    return compare_timing_with_admissible_winners(
        targeted["graph"], targeted["timing"], fresh["timing"],
        f"{label}.timing_values", first_difference, require)


def level_assignments_differ(targeted: dict[str, Any],
                             fresh: dict[str, Any]) -> bool:
    return targeted["node_to_level"] != fresh["node_to_level"]


def replay(args: argparse.Namespace) -> None:
    probe = args.probe.resolve(strict=True)
    trace = (args.replay or args.trace).resolve(strict=True)
    input_flag = "--replay" if args.replay else "--trace"
    trace_value = read_json(trace)
    program = trace_value if args.replay else trace_value["replay"]
    batches = program["workload"]["batches"]
    require(isinstance(batches, list) and batches,
            "Stage 6 correctness trace must contain a nonempty batch")
    root = pathlib.Path(tempfile.mkdtemp(
        prefix="run-", dir=args.run_root.resolve()))
    summaries: list[dict[str, Any]] = []
    prior_observations: list[dict[str, Any]] = []
    all_pids = {os.getpid()}
    for prefix in range(1, len(batches) + 1):
        target_path = root / f"prefix-{prefix}" / "targeted.json"
        fresh_path = root / f"prefix-{prefix}" / "fresh.json"
        # Each role retains its own fresh process; avoid overlapping the full
        # snapshots, which exceeded the real-design WSL memory budget.
        targeted, targeted_wall_ms = run_child(
            probe, "targeted", trace, prefix, target_path,
            args.timeout_seconds, input_flag=input_flag)
        fresh, fresh_wall_ms = run_child(
            probe, "fresh", trace, prefix, fresh_path,
            args.timeout_seconds, input_flag=input_flag)
        targeted_functional, timings, resources, targeted_levels \
            = validate_stage6_targeted(
                targeted, prefix, batches[:prefix],
                f"targeted prefix {prefix}")
        fresh_functional, fresh_levels = validate_stage6_fresh(
            fresh, prefix, f"fresh prefix {prefix}")
        legal_noncompact = level_assignments_differ(
            targeted_levels, fresh_levels)
        winner_evidence = compare_functional(
            targeted_functional, fresh_functional, f"prefix {prefix}")
        mixed = validate_mixed_execution(
            targeted, prefix, trace.name, f"targeted prefix {prefix}")
        target_source = normalized_source(
            targeted["source_isolation"], f"targeted prefix {prefix} source")
        fresh_source = normalized_source(
            fresh["source_isolation"], f"fresh prefix {prefix} source")
        source_difference = first_exact_difference(target_source, fresh_source)
        require(source_difference is None,
                f"prefix {prefix} source mismatch: {source_difference}")
        pids = {targeted["run"]["child_pid"], fresh["run"]["child_pid"]}
        require(len(pids) == 2 and not pids & all_pids,
                f"prefix {prefix} did not use two new isolated processes")
        all_pids |= pids
        observations = targeted["published_observations"]
        for index in range(len(prior_observations)):
            difference = first_exact_difference(observations[index], prior_observations[index])
            require(difference is None,
                    f"prefix {prefix} changed prior observation: {difference}")
        prior_observations = observations
        summaries.append({
            "prefix_batches": prefix,
            "targeted_pid": targeted["run"]["child_pid"],
            "fresh_pid": fresh["run"]["child_pid"],
            "graph_reference_fields_equal": True,
            "timing_value_fields_equal_with_stage2_tolerance": True,
            "legal_noncompact": legal_noncompact,
            "legal_noncompact_report_only": True,
            "level_assignments_compared_across_processes": False,
            "targeted_levelization_independently_validated": True,
            "semantic_winner_policy":
                "graph_numeric_equal_cost_admissible_candidate_sets",
            "fresh_levelization_independently_validated": True,
            "semantic_winner_evidence": winner_evidence,
            "mixed_execution": mixed,
            "structural_receipt_validated": True,
            "transaction_receipt_validated": True,
            "transaction_semantic_trace_exact": True,
            "targeted_wall_ms": duration_ms(
                targeted_wall_ms, f"prefix {prefix}.targeted_wall_ms",
                require, positive=True),
            "fresh_oracle_ms": duration_ms(
                fresh_wall_ms, f"prefix {prefix}.fresh_oracle_ms",
                require, positive=True),
            "batch_phase_timings": timings,
            "batch_resource_observations": resources,
        })
        del targeted, fresh, targeted_functional, fresh_functional
        del targeted_levels, fresh_levels, target_source, fresh_source
    write_json(root / "controller-summary.json", {
        "schema_version": "ista-stage6-differential-verdict/4.0.0",
        "trace": str(trace),
        "controller_pid": os.getpid(),
        "all_process_ids_distinct": True,
        "absolute_tolerance": ABS_TOLERANCE,
        "relative_tolerance": REL_TOLERANCE,
        "prefixes": summaries,
        "functional_correctness_primary": True,
        "fresh_oracle_time_excluded_from_incremental_samples": True,
    })


def canary_receipt(level_offsets: list[int], level_nodes: list[dict[str, str]],
                   inverse: list[int]
                   ) -> dict[str, Any]:
    keys = [{"kind": "port", "full_name": name,
             "instance_name": "", "pin_name": ""}
            for name in ("a", "b", "c")]
    graph = {"nodes": [{"key": key} for key in keys],
             "active_arcs": [{"key": {"canary": "a-to-c"},
                              "source": keys[0], "sink": keys[2],
                              "eligibility": {"levelization": True}}],
             "reference_payload": {}}
    timing = {field: {} for field in TIMING_VALUE_FIELDS}
    timing["schema_version"] = "canary"
    timing.update({
        "level_offsets": level_offsets, "level_nodes": level_nodes,
        "node_to_level": inverse})
    return {"graph": graph, "timing": timing}


def expect_rejected(value: dict[str, Any], label: str) -> None:
    try:
        validate_functional(value, label)
    except ControllerError:
        return
    raise ControllerError(f"{label} canary was accepted")


def self_test() -> None:
    keys = [{"kind": "port", "full_name": name,
             "instance_name": "", "pin_name": ""}
            for name in ("a", "b", "c")]
    compact = canary_receipt([0, 2, 3], keys, [1, 1, 2])
    noncompact = canary_receipt([0, 1, 2, 3], keys, [1, 2, 3])
    left, _ = validate_functional(compact, "compact")
    right, _ = validate_functional(noncompact, "noncompact")
    require(first_exact_difference(left, right) is None,
            "different legal levels changed Stage 6 functional equality")
    require(level_assignments_differ(compact["timing"],
                                     noncompact["timing"])
            and not level_assignments_differ(compact["timing"],
                                             compact["timing"]),
            "legal_noncompact report-only comparison differs")
    same_level_order = copy.deepcopy(compact)
    same_level_order["timing"]["level_nodes"] = [keys[1], keys[0], keys[2]]
    validate_functional(same_level_order, "same-level-order")
    missing = copy.deepcopy(compact)
    missing["timing"]["level_nodes"].pop()
    expect_rejected(missing, "missing-node")
    inverse = copy.deepcopy(compact)
    inverse["timing"]["node_to_level"][2] = 1
    expect_rejected(inverse, "inverse")
    order = copy.deepcopy(compact)
    order["timing"]["level_nodes"][2] = {"unknown": "node"}
    expect_rejected(order, "level-order")
    reverse = copy.deepcopy(compact)
    reverse["timing"]["level_nodes"] = [keys[2], keys[0], keys[1]]
    expect_rejected(reverse, "level-order-reverse")
    edge = canary_receipt([0, 1, 2, 3], [keys[2], keys[1], keys[0]],
                          [3, 2, 1])
    expect_rejected(edge, "eligible-edge")
    run_equal_cost_canary(
        compare_timing_with_admissible_winners, first_difference,
        require, ControllerError)
    run_execution_binding_canary(
        first_exact_difference, require, ControllerError)
    run_semantic_trace_exact_canary(
        first_exact_difference, first_difference, require, ControllerError,
        ABS_TOLERANCE / 2.0)
    graph_float_left = {
        "graph": {"reference_payload": {"delay": 1.0}}, "timing": {}}
    graph_float_right = copy.deepcopy(graph_float_left)
    graph_float_right["graph"]["reference_payload"]["delay"] \
        += ABS_TOLERANCE / 2.0
    require(first_difference(graph_float_left["graph"],
                             graph_float_right["graph"]) is None,
            "graph float canary must be inside Stage2 timing tolerance")
    rejected = False
    try:
        compare_functional(graph_float_left, graph_float_right,
                           "graph-float-tamper")
    except ControllerError:
        rejected = True
    require(rejected,
            "exact graph/reference comparator accepted float tamper")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--probe", type=pathlib.Path)
    inputs = parser.add_mutually_exclusive_group()
    inputs.add_argument("--trace", type=pathlib.Path)
    inputs.add_argument("--replay", type=pathlib.Path)
    parser.add_argument("--run-root", type=pathlib.Path)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    return parser.parse_args()


def main() -> int:
    try:
        args = parse_args()
        if args.self_test:
            self_test()
        else:
            require(args.probe is not None and (args.trace is not None or args.replay is not None)
                    and args.run_root is not None,
                    "--probe, --trace or --replay, and --run-root are required")
            args.run_root.mkdir(parents=True, exist_ok=True)
            replay(args)
        return 0
    except (ControllerError, OSError, subprocess.SubprocessError,
            KeyError, TypeError, ValueError) as error:
        print(f"Stage 6 levelization controller: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
