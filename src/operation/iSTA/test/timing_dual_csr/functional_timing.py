"""Validate complete functional timing and independent legal level layouts."""

import json
from itertools import pairwise
from typing import Any

from trace_process import require

LEVEL_FIELDS = {"level_offsets", "level_nodes", "node_to_level"}
TIMING_VALUE_FIELDS = {
    "schema_version",
    "nodes",
    "effective_clock",
    "physical_clock",
    "data_slews",
    "paths",
    "arcs",
}


def exact_object(value: object, keys: set[str], label: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(set(value) == keys, f"{label} fields differ: {sorted(value)}")
    return value


def canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def node_keys(graph: dict[str, Any], label: str) -> list[object]:
    nodes = graph.get("nodes")
    require(isinstance(nodes, list), f"{label}.nodes must be an array")
    result: list[object] = []
    for index, node in enumerate(nodes):
        require(
            isinstance(node, dict) and isinstance(node.get("key"), dict),
            f"{label}.nodes[{index}].key is absent",
        )
        result.append(node["key"])
    encoded = list(map(canonical, result))
    require(len(encoded) == len(set(encoded)), f"{label} repeats a NodeKey")
    return result


def complete_key_permutation(values: object, expected: list[object], label: str) -> list[object]:
    require(isinstance(values, list), f"{label} must be an array")
    encoded = list(map(canonical, values))
    require(
        len(encoded) == len(expected)
        and len(encoded) == len(set(encoded))
        and set(encoded) == set(map(canonical, expected)),
        f"{label} must be the complete unique NodeKey permutation",
    )
    return values


def validate_offsets(value: object, node_count: int, label: str) -> list[int]:
    require(
        isinstance(value, list) and all(type(item) is int and item >= 0 for item in value),
        f"{label} must be nonnegative integer offsets",
    )
    offsets = value
    require(
        bool(offsets) and offsets[0] == 0 and offsets[-1] == node_count,
        f"{label} endpoints do not cover all nodes",
    )
    if node_count == 0:
        require(offsets == [0], f"{label} empty graph form differs")
    else:
        require(
            len(offsets) >= 2 and all(left < right for left, right in pairwise(offsets)),
            f"{label} contains an empty or reversed level",
        )
    return offsets


def validate_levelization(
    graph: dict[str, Any], timing: dict[str, Any], label: str
) -> dict[str, Any]:
    item = timing
    graph_nodes = node_keys(graph, f"{label}.graph")
    offsets = validate_offsets(item["level_offsets"], len(graph_nodes), f"{label}.level_offsets")
    level_nodes = complete_key_permutation(item["level_nodes"], graph_nodes, f"{label}.level_nodes")

    inverse_raw = item["node_to_level"]
    require(
        isinstance(inverse_raw, list)
        and len(inverse_raw) == len(graph_nodes)
        and all(type(level) is int and 1 <= level < len(offsets) for level in inverse_raw),
        f"{label}.node_to_level is malformed or out of range",
    )
    inverse = dict(zip(map(canonical, graph_nodes), inverse_raw, strict=True))
    for level, (begin, end) in enumerate(pairwise(offsets), start=1):
        for position in range(begin, end):
            require(
                inverse[canonical(level_nodes[position])] == level,
                f"{label}.node_to_level is not the exact inverse",
            )

    arcs = graph.get("active_arcs")
    require(isinstance(arcs, list), f"{label}.graph.active_arcs is malformed")
    eligible_count = 0
    for index, arc in enumerate(arcs):
        require(
            isinstance(arc, dict) and isinstance(arc.get("eligibility"), dict),
            f"{label}.graph.active_arcs[{index}] is malformed",
        )
        if arc["eligibility"].get("levelization") is not True:
            continue
        eligible_count += 1
        source = canonical(arc.get("source"))
        sink = canonical(arc.get("sink"))
        require(
            source in inverse and sink in inverse and inverse[source] < inverse[sink],
            f"{label}.graph.active_arcs[{index}] violates forward levels",
        )
    return {
        "level_count": len(offsets) - 1,
        "node_count": len(graph_nodes),
        "eligible_arc_count": eligible_count,
    }


def validate_functional(value: object, label: str) -> tuple[dict[str, Any], dict[str, Any]]:
    full = exact_object(value, {"graph", "timing"}, label)
    graph = exact_object(
        full["graph"], {"nodes", "active_arcs", "reference_payload"}, f"{label}.graph"
    )
    timing = exact_object(full["timing"], TIMING_VALUE_FIELDS | LEVEL_FIELDS, f"{label}.timing")
    validate_levelization(graph, timing, f"{label}.timing")
    # Shallow views: level layout is checked independently of timing values.
    return (
        {"graph": graph, "timing": {field: timing[field] for field in TIMING_VALUE_FIELDS}},
        timing,
    )
