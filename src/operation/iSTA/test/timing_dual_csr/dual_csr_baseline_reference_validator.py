#!/usr/bin/env python3
"""Typed structural validator for the generation-zero reference input."""

import copy
import json
from collections.abc import Callable
from typing import Any

from dual_csr_reference_model_validator import validate_reference_value
from dual_csr_typed_json_validator import validate_arc_payload, validate_node_record

Require = Callable[[bool, str], None]

REFERENCE_FIELDS = {"schema_version", "source_identity", "analysis_context",
    "analysis_order", "path_source_order", "transition_order", "instances",
    "ordered_root_ids", "ordered_start_ids", "ordered_end_ids", "clock_tags",
    "clocks", "clock_owner_by_node", "port_constraints",
    "case_analysis_constraints", "timing_check_endpoints",
    "referenced_library_cells", "parasitics", "start_seeds",
    "endpoint_required", "cell_pin_capacitances", "unsupported_reasons"}
GRAPH_FIELDS = {"nodes", "node_ids_by_key", "arcs", "arc_ids_by_key",
                "out_row_offsets", "out_arc_ids", "in_row_offsets",
                "in_arc_ids"}


def _canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _count(value: object, label: str, require: Require) -> int:
    require(type(value) is int and value >= 0,
            f"{label} must be a nonnegative integer")
    return value


def _offsets(value: object, rows: int, entries: int, label: str,
             require: Require) -> list[int]:
    if not isinstance(value, list) or len(value) != rows + 1:
        require(isinstance(value, list) and len(value) == rows + 1,
                f"{label} row extent differs")
        return []
    require(value[0] == 0 and value[-1] == entries
            and all(type(item) is int and item >= 0 for item in value)
            and all(left <= right for left, right in zip(value, value[1:], strict=False)),
            f"{label} is not a valid CSR offset vector")
    return value


def _lookup(entries: object, records: list[dict[str, Any]], key_field: str,
            label: str, require: Require) -> None:
    require(isinstance(entries, list) and len(entries) == len(records),
            f"{label} extent differs")
    expected = {_canonical(record[key_field]): record["id"]
                for record in records}
    require(len(expected) == len(records), f"{label} record keys repeat")
    actual: dict[str, int] = {}
    for index, entry in enumerate(entries):
        require(isinstance(entry, dict) and set(entry) == {"key", "id"},
                f"{label}[{index}] fields differ")
        encoded = _canonical(entry["key"])
        slot = _count(entry["id"], f"{label}[{index}].id", require)
        require(encoded not in actual, f"{label}[{index}] repeats a key")
        actual[encoded] = slot
    require(actual == expected, f"{label} is not the exact key-to-ID bijection")


def validate_baseline_reference_input(value: object,
                                      expected_baseline: dict[str, Any],
                                      session: int, label: str,
                                      require: Require
                                      ) -> dict[str, object]:
    require(isinstance(value, dict)
            and set(value) == REFERENCE_FIELDS | GRAPH_FIELDS,
            f"{label} fields differ")
    item = value
    for field in REFERENCE_FIELDS:
        validate_reference_value(field, item[field], f"{label}.{field}", require)
    source = item["source_identity"]
    fingerprints = expected_baseline["fingerprints"]
    expected_source = {
        "fingerprint_schema_version": fingerprints["schema_version"],
        "session_token": session,
        "graph_generation": 0,
        "topology_fingerprint": fingerprints["topology"],
        "clock_constraint_fingerprint": fingerprints["clock_constraint"],
        "library_fingerprint": fingerprints["library"],
        "corner_fingerprint": fingerprints["corner"],
        "parasitic_fingerprint": fingerprints["parasitic"],
        "timing_input_fingerprint": fingerprints["timing_input"],
    }
    require(source == expected_source,
            f"{label}.source_identity differs from the frozen baseline")

    nodes = item["nodes"]
    require(isinstance(nodes, list), f"{label}.nodes must be an array")
    for index, node in enumerate(nodes):
        validate_node_record(node, f"{label}.nodes[{index}]", require)
        require(node["id"] == index,
                f"{label}.nodes[{index}] is not append-only")
    _lookup(item["node_ids_by_key"], nodes, "key",
            f"{label}.node_ids_by_key", require)

    arcs = item["arcs"]
    require(isinstance(arcs, list), f"{label}.arcs must be an array")
    ranks: set[int] = set()
    for index, arc in enumerate(arcs):
        require(isinstance(arc, dict)
                and set(arc) == {"id", "native_traversal_rank", "key",
                    "source", "sink", "kind", "is_clock_arc", "is_disabled",
                    "is_loop_disabled", "eligibility", "transfer", "output_load",
                    "native_cell_arc_ordinal"},
                f"{label}.arcs[{index}] fields differ")
        require(arc["id"] == index,
                f"{label}.arcs[{index}] is not append-only")
        rank = _count(arc["native_traversal_rank"],
                      f"{label}.arcs[{index}].native_traversal_rank", require)
        require(rank not in ranks, f"{label}.arcs repeats traversal rank")
        ranks.add(rank)
        payload = {key: field for key, field in arc.items()
                   if key not in {"id", "native_traversal_rank"}}
        validate_arc_payload(payload, f"{label}.arcs[{index}].payload", require)
    require(ranks == set(range(len(arcs))),
            f"{label}.arcs traversal ranks are not contiguous")
    _lookup(item["arc_ids_by_key"], arcs, "key",
            f"{label}.arc_ids_by_key", require)

    out_ids = item["out_arc_ids"]
    in_ids = item["in_arc_ids"]
    require(isinstance(out_ids, list) and isinstance(in_ids, list)
            and sorted(out_ids) == list(range(len(arcs)))
            and sorted(in_ids) == list(range(len(arcs))),
            f"{label} adjacency is not a pair of ArcId permutations")
    out_offsets = _offsets(item["out_row_offsets"], len(nodes), len(arcs),
                           f"{label}.out_row_offsets", require)
    in_offsets = _offsets(item["in_row_offsets"], len(nodes), len(arcs),
                          f"{label}.in_row_offsets", require)
    for node_id in range(len(nodes)):
        require(all(arcs[arc_id]["source"] == node_id
                    for arc_id in out_ids[out_offsets[node_id]:out_offsets[node_id + 1]]),
                f"{label}.out adjacency row {node_id} has a wrong source")
        require(all(arcs[arc_id]["sink"] == node_id
                    for arc_id in in_ids[in_offsets[node_id]:in_offsets[node_id + 1]]),
                f"{label}.in adjacency row {node_id} has a wrong sink")

    state = expected_baseline["canonical_pre_state"]
    require(sorted(map(_canonical, (node["key"] for node in nodes)))
            == sorted(map(_canonical, state["nodes"])),
            f"{label}.nodes differ from the frozen baseline")
    require(sorted(map(_canonical, (arc["key"] for arc in arcs)))
            == sorted(map(_canonical, state["arcs"])),
            f"{label}.arcs differ from the frozen baseline")
    require(sorted(record["instance_name"] for record in item["instances"])
            == sorted(state["instances"]),
            f"{label}.instances differ from the frozen baseline")
    return {field: copy.deepcopy(item[field])
            for field in REFERENCE_FIELDS}
