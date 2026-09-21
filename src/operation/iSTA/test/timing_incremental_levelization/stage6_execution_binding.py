#!/usr/bin/env python3
"""Exact per-generation execution-to-publication bindings for Stage 6."""

import copy
from collections.abc import Callable
from itertools import pairwise
from typing import Any

from stage6_dense_node_map import canonical_key, generation_dense_keys


def _bind_one(record: dict[str, Any], observation: dict[str, Any],
              dense_keys: list[dict[str, Any]],
              exact_difference: Callable,
              require: Callable, label: str) -> dict[str, Any]:
    graph_nodes = observation["functional"]["graph"]["nodes"]
    require(isinstance(graph_nodes, list)
            and all(isinstance(item, dict)
                    and isinstance(item.get("key"), dict)
                    for item in graph_nodes),
            f"{label}.graph.nodes is malformed")
    graph_keys = [item["key"] for item in graph_nodes]
    levels = record["levels"]
    require(isinstance(levels, dict)
            and set(levels) == {"complete", "level_offsets", "level_nodes",
                                "node_to_level", "view_error"}
            and levels["complete"] is True
            and levels["view_error"] is None,
            f"{label}.levels is not a complete successful receipt")
    dense_nodes = levels["level_nodes"]
    inverse = levels["node_to_level"]
    require(isinstance(dense_nodes, list)
            and isinstance(inverse, list)
            and len(dense_nodes) == len(dense_keys) == len(inverse)
            and all(type(item) is int for item in dense_nodes)
            and sorted(dense_nodes) == list(range(len(dense_keys)))
            and all(type(item) is int for item in inverse),
            f"{label}.levels dense NodeId permutation/inverse is malformed")
    receipt = observation["functional"]["timing"]
    require(record["generation"] == observation["structural"]["generation"],
            f"{label} generation differs from level receipt")
    level_by_key = {
        canonical_key(node): inverse[index]
        for index, node in enumerate(dense_keys)
    }
    expected = {
        "level_offsets": levels["level_offsets"],
        "node_to_level": [
            level_by_key[canonical_key(node)] for node in graph_keys],
    }
    actual = {field: receipt[field] for field in expected}
    difference = exact_difference(actual, expected, f"{label}.levels")
    require(difference is None,
            f"{label} execution/public level binding differs: {difference}")
    offsets = levels["level_offsets"]
    require(isinstance(offsets, list) and len(offsets) >= 2
            and offsets[0] == 0 and offsets[-1] == len(dense_keys)
            and all(type(item) is int for item in offsets)
            and all(left < right for left, right
                    in pairwise(offsets)),
            f"{label}.levels offsets are malformed")
    for level, (begin, end) in enumerate(
            pairwise(offsets), start=1):
        require(all(inverse[node] == level
                    for node in dense_nodes[begin:end]),
                f"{label}.levels dense permutation crosses level {level}")
        mapped = sorted(
            (dense_keys[node] for node in dense_nodes[begin:end]),
            key=canonical_key)
        observed = sorted(receipt["level_nodes"][begin:end],
                          key=canonical_key)
        difference = exact_difference(
            observed, mapped, f"{label}.levels.level[{level}]")
        require(difference is None,
                f"{label} execution/public level set differs: {difference}")
    published_traversal = observation["structural"]["update_telemetry"][
        "traversal"]
    difference = exact_difference(
        record["traversal"], published_traversal,
        f"{label}.traversal")
    require(difference is None,
            f"{label} execution/public traversal binding differs: "
            f"{difference}")
    return {"generation": record["generation"],
            "dense_id_map_reconstructed_from_transaction": True,
            "per_level_nodekey_sets_exactly_bound": True,
            "level_offsets_and_inverse_exactly_bound": True,
            "traversal_bound_to_published_telemetry": True}


def validate_execution_observation_bindings(
        transaction: dict[str, Any], observations: list[dict[str, Any]],
        exact_difference: Callable,
        require: Callable, label: str) -> list[dict[str, Any]]:
    records = transaction["execution_trace"]
    require(isinstance(records, list)
            and len(records) == len(observations),
            f"{label} execution/publication cardinality differs")
    dense_generations = generation_dense_keys(
        transaction, observations, exact_difference, require,
        f"{label}.dense_map")
    return [
        _bind_one(record, observation, dense_keys,
                  exact_difference, require,
                  f"{label}.generation[{index + 1}]")
        for index, (record, observation, dense_keys) in enumerate(
            zip(records, observations, dense_generations,
                strict=True))
    ]


def _key(name: str) -> dict[str, str]:
    return {"kind": "port", "full_name": name,
            "instance_name": "", "pin_name": ""}


def _traversal(value: int) -> dict[str, dict[str, int]]:
    family = {"base_out": value, "aux_out": 0, "base_in": 0}
    return {phase: copy.deepcopy(family) for phase in (
        "levelization", "arc_initialization", "forward_propagation",
        "required_propagation")}


def _fixture() -> tuple[dict[str, Any], list[dict[str, Any]]]:
    z_key, a_key, b_key, c_key = map(_key, ("z", "a", "b", "c"))
    generations = [
        ([z_key, a_key, b_key], [0, 2, 3], [2, 1, 0], [2, 1, 1]),
        ([z_key, a_key, b_key, c_key],
         [0, 1, 3, 4], [1, 3, 2, 0], [3, 1, 2, 2]),
    ]
    records: list[dict[str, Any]] = []
    observations: list[dict[str, Any]] = []
    for generation, (physical, offsets, dense, inverse) in enumerate(
            generations, start=1):
        traversal = _traversal(generation)
        canonical = sorted(physical, key=canonical_key)
        level_by_key = {
            canonical_key(key): inverse[index]
            for index, key in enumerate(physical)
        }
        records.append({
            "generation": generation,
            "levels": {"complete": True, "level_offsets": offsets,
                       "level_nodes": dense, "node_to_level": inverse,
                       "view_error": None},
            "traversal": copy.deepcopy(traversal),
        })
        observations.append({
            "functional": {
                "graph": {"nodes": [{"key": key} for key in canonical]},
                "timing": {
                    "level_offsets": offsets,
                    "level_nodes": sorted(
                        physical,
                        key=lambda key: (level_by_key[canonical_key(key)],
                                         canonical_key(key))),
                    "node_to_level": [level_by_key[canonical_key(key)]
                                      for key in canonical],
                },
            },
            "structural": {"generation": generation,
                           "update_telemetry": {"traversal": traversal}},
        })
    transaction = {
        "baseline_reference_input": {"nodes": [
            {"id": 0, "key": z_key}, {"id": 1, "key": a_key},
            {"id": 2, "key": b_key},
        ]},
        "descriptor_trace": [
            {"after": {"node_slots": 3}, "node_appends": []},
            {"after": {"node_slots": 4},
             "node_appends": [{"id": 3, "key": c_key}]},
        ],
        "execution_trace": records,
    }
    return transaction, observations


def _expect_rejected(transaction: dict[str, Any],
                     observations: list[dict[str, Any]],
                     exact_difference: Callable, require: Callable,
                     error_type: type[Exception], label: str) -> None:
    try:
        validate_execution_observation_bindings(
            transaction, observations, exact_difference,
            require, label)
    except error_type:
        return
    raise error_type(f"{label} execution/publication tamper was accepted")


def run_execution_binding_canary(exact_difference: Callable,
                                 require: Callable,
                                 error_type: type[Exception]) -> None:
    transaction, observations = _fixture()
    baseline_keys = [
        item["key"]
        for item in transaction["baseline_reference_input"]["nodes"]
    ]
    graph_keys = [
        item["key"] for item in
        observations[0]["functional"]["graph"]["nodes"]
    ]
    require(baseline_keys != graph_keys,
            "binding canary must separate dense-id and canonical orders")
    validate_execution_observation_bindings(
        transaction, observations, exact_difference,
        require, "binding-canary")
    for record_index, positions in ((0, (0, 1)), (1, (1, 2))):
        same_level = copy.deepcopy(transaction)
        nodes = same_level["execution_trace"][record_index]["levels"][
            "level_nodes"]
        left, right = positions
        nodes[left], nodes[right] = nodes[right], nodes[left]
        validate_execution_observation_bindings(
            same_level, observations, exact_difference,
            require, f"same-level-order-{record_index}")
    for record_index, positions in ((0, (1, 2)), (1, (0, 1))):
        tampered = copy.deepcopy(transaction)
        nodes = tampered["execution_trace"][record_index]["levels"][
            "level_nodes"]
        left, right = positions
        nodes[left], nodes[right] = nodes[right], nodes[left]
        _expect_rejected(tampered, observations,
                         exact_difference, require, error_type,
                         f"cross-level-tamper-{record_index}")
    for record_index in (0, 1):
        tampered = copy.deepcopy(transaction)
        tampered["execution_trace"][record_index]["traversal"][
            "forward_propagation"]["base_out"] += 1
        _expect_rejected(tampered, observations,
                         exact_difference, require, error_type,
                         f"traversal-tamper-{record_index}")
