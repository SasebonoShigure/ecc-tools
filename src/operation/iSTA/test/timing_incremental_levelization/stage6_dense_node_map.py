#!/usr/bin/env python3
"""Reconstruct Stage 6 dense NodeId maps from exact transaction evidence."""

import json
from typing import Any, Callable


def canonical_key(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _entry(value: object, require: Callable,
           label: str) -> tuple[int, dict[str, Any]]:
    require(isinstance(value, dict)
            and type(value.get("id")) is int
            and isinstance(value.get("key"), dict),
            f"{label} does not contain an exact dense id/NodeKey pair")
    return value["id"], value["key"]


def generation_dense_keys(
        transaction: dict[str, Any], observations: list[dict[str, Any]],
        exact_difference: Callable, require: Callable,
        label: str) -> list[list[dict[str, Any]]]:
    baseline = transaction["baseline_reference_input"]["nodes"]
    descriptors = transaction["descriptor_trace"]
    require(isinstance(baseline, list)
            and isinstance(descriptors, list)
            and len(descriptors) == len(observations),
            f"{label} dense-map source cardinality differs")
    by_id: dict[int, dict[str, Any]] = {}
    for index, raw in enumerate(baseline):
        node_id, key = _entry(raw, require, f"{label}.baseline[{index}]")
        require(node_id not in by_id,
                f"{label}.baseline duplicates dense NodeId {node_id}")
        by_id[node_id] = key

    generations: list[list[dict[str, Any]]] = []
    for index, (descriptor, observation) in enumerate(
            zip(descriptors, observations, strict=True), start=1):
        appends = descriptor["node_appends"]
        require(isinstance(appends, list),
                f"{label}.descriptor[{index}].node_appends is malformed")
        for append_index, raw in enumerate(appends):
            node_id, key = _entry(
                raw, require,
                f"{label}.descriptor[{index}].node_appends[{append_index}]")
            require(node_id not in by_id,
                    f"{label}.descriptor[{index}] reuses NodeId {node_id}")
            by_id[node_id] = key
        slots = descriptor["after"]["node_slots"]
        require(type(slots) is int and slots >= 0
                and set(by_id) == set(range(slots)),
                f"{label}.descriptor[{index}] dense NodeId domain differs")
        dense = [by_id[node_id] for node_id in range(slots)]
        graph_nodes = observation["functional"]["graph"]["nodes"]
        require(isinstance(graph_nodes, list)
                and all(isinstance(node, dict)
                        and isinstance(node.get("key"), dict)
                        for node in graph_nodes),
                f"{label}.observation[{index}].graph.nodes is malformed")
        graph_keys = [node["key"] for node in graph_nodes]
        difference = exact_difference(
            sorted(dense, key=canonical_key),
            sorted(graph_keys, key=canonical_key),
            f"{label}.observation[{index}].node_domain")
        require(difference is None,
                f"{label}.observation[{index}] NodeKey domain differs: "
                f"{difference}")
        generations.append(dense)
    return generations
