#!/usr/bin/env python3
"""Physical adjacency and required incoming-row validation."""

import copy
import json
from collections.abc import Callable
from typing import Any

Require = Callable[[bool, str], None]


def _canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _row(slots: list[object], offsets: list[int], owner: int, label: str,
         require: Require) -> list[int]:
    require(type(owner) is int and owner >= 0 and owner + 1 < len(offsets),
            f"{label} owner is out of row range")
    if type(owner) is not int or owner < 0 or owner + 1 >= len(offsets):
        return []
    begin, end = offsets[owner], offsets[owner + 1]
    require(type(begin) is int and type(end) is int
            and 0 <= begin <= end <= len(slots),
            f"{label} row offsets are invalid")
    if not (type(begin) is int and type(end) is int
            and 0 <= begin <= end <= len(slots)):
        return []
    return [value for value in slots[begin:end] if value is not None]


def _endpoint_ids(payloads: dict[int, dict[str, Any]],
                  node_keys: dict[int, dict[str, Any]], label: str,
                  require: Require) -> dict[int, tuple[int, int]]:
    node_ids = {_canonical(key): node_id for node_id, key in node_keys.items()}
    require(len(node_ids) == len(node_keys), f"{label} NodeKeys repeat")
    result: dict[int, tuple[int, int]] = {}
    for arc_id, payload in payloads.items():
        source = node_ids.get(_canonical(payload["source"]))
        sink = node_ids.get(_canonical(payload["sink"]))
        require(source is not None and sink is not None,
                f"{label} ArcId {arc_id} has an unknown endpoint")
        if source is not None and sink is not None:
            result[arc_id] = (source, sink)
    return result


def _sequence(values: list[int], owner: int, endpoint: int,
              endpoints: dict[int, tuple[int, int]], extent: int,
              label: str, require: Require) -> None:
    require(len(values) == len(set(values)), f"{label} repeats an ArcId")
    for position, arc_id in enumerate(values):
        require(type(arc_id) is int and 0 <= arc_id < extent
                and arc_id in endpoints and endpoints[arc_id][endpoint] == owner,
                f"{label}[{position}] has a wrong ArcId/endpoint")


def _new_nodes(descriptor: dict[str, Any]) -> set[int]:
    before = descriptor["before"]["node_slots"]
    after = descriptor["after"]["node_slots"]
    return set(range(before, after))


def _affected(descriptor: dict[str, Any], endpoint: str) -> set[int]:
    result = _new_nodes(descriptor)
    result.update(record[endpoint]
                  for record in descriptor["logical_deactivations"])
    result.update(record[endpoint] for record in descriptor["arc_appends"])
    writes = (("removes", "changes", "aux_rows") if endpoint == "source"
              else ("reverse_changes",))
    result.update(record[endpoint] for field in writes
                  for record in descriptor.get(field, []))
    return result


def _physical_rows(
        payloads: dict[int, dict[str, Any]],
        node_keys: dict[int, dict[str, Any]], base_out: list[object],
        out_offsets: list[int], base_in: list[object], in_offsets: list[int],
        aux: dict[int, list[int]], nodes: int, arcs: int, label: str,
        require: Require) -> tuple[dict[int, list[int]], dict[int, list[int]]]:
    endpoints = _endpoint_ids(payloads, node_keys, label, require)
    outgoing: dict[int, list[int]] = {}
    incoming: dict[int, list[int]] = {}
    all_out: list[int] = []
    all_in: list[int] = []
    for owner in range(nodes):
        outgoing[owner] = (_row(base_out, out_offsets, owner,
                                f"{label}.base_out[{owner}]", require)
                           + list(aux.get(owner, [])))
        incoming[owner] = _row(base_in, in_offsets, owner,
                               f"{label}.base_in[{owner}]", require)
        _sequence(outgoing[owner], owner, 0, endpoints, arcs,
                  f"{label}.outgoing[{owner}]", require)
        _sequence(incoming[owner], owner, 1, endpoints, arcs,
                  f"{label}.incoming[{owner}]", require)
        all_out.extend(outgoing[owner])
        all_in.extend(incoming[owner])
    require(len(all_out) == len(set(all_out)),
            f"{label} outgoing adjacency repeats an ArcId")
    require(len(all_in) == len(set(all_in)),
            f"{label} incoming adjacency repeats an ArcId")
    require(set(all_out) == set(all_in),
            f"{label} outgoing/incoming ArcId coverage differs")
    return outgoing, incoming


def _post_members(descriptor: dict[str, Any], owner: int, endpoint: str,
                  expected: list[int]) -> set[int]:
    removed = {record["id"] for record
               in descriptor["logical_deactivations"]}
    result = {arc_id for arc_id in expected if arc_id not in removed}
    result.update(record["id"] for record in descriptor["arc_appends"]
                  if record[endpoint] == owner)
    return result


def validate_candidate_rows(
        descriptor: dict[str, Any], payloads: dict[int, dict[str, Any]],
        node_keys: dict[int, dict[str, Any]], base_out: list[object],
        out_offsets: list[int], base_in: list[object], in_offsets: list[int],
        aux: dict[int, list[int]], label: str, require: Require) -> None:
    """Validate exact ordered post-candidate rows before reverse replay."""
    before = descriptor["before"]
    after = descriptor["after"]
    endpoints = _endpoint_ids(payloads, node_keys, label, require)
    outgoing, incoming = _physical_rows(
        payloads, node_keys, base_out, out_offsets, base_in, in_offsets, aux,
        after["node_slots"], after["arc_slots"], label, require)
    sources = _affected(descriptor, "source")
    expected_new = sorted(source for source in sources
                          if source in _new_nodes(descriptor))
    appended = descriptor["appended_base_out_rows"]
    require([record["source"] for record in appended] == expected_new,
            f"{label}.appended_base_out_rows coverage/order differs")
    appended_ids = {record["id"] for record in descriptor["arc_appends"]}
    for position, record in enumerate(appended):
        source = record["source"]
        row_label = f"{label}.appended_base_out_rows[{position}]"
        require(outgoing[source] == record["arc_ids"] and not aux.get(source, []),
                f"{row_label} differs from the candidate BaseOut row")
        require(set(record["arc_ids"]).issubset(appended_ids),
                f"{row_label} contains a non-appended ArcId")
        _sequence(record["arc_ids"], source, 0, endpoints,
                  after["arc_slots"], f"{row_label}.arc_ids", require)
    sinks = sorted(_affected(descriptor, "sink"))
    affected_in = descriptor["affected_base_in_rows"]
    require([record["sink"] for record in affected_in] == sinks,
            f"{label}.affected_base_in_rows coverage/order differs")
    for position, record in enumerate(affected_in):
        sink = record["sink"]
        row_label = f"{label}.affected_base_in_rows[{position}]"
        require(sink < after["node_slots"], f"{row_label}.sink is out of range")
        require(incoming[sink] == record["replacement_arc_ids"],
                f"{row_label}.replacement differs from BaseIn")
        if sink >= before["node_slots"]:
            require(record["expected_arc_ids"] == [],
                    f"{row_label}.expected for a new sink is not empty")
        _sequence(record["expected_arc_ids"], sink, 1, endpoints,
                  before["arc_slots"], f"{row_label}.expected_arc_ids", require)
        _sequence(record["replacement_arc_ids"], sink, 1, endpoints,
                  after["arc_slots"], f"{row_label}.replacement_arc_ids", require)
        require(set(record["replacement_arc_ids"])
                == _post_members(descriptor, sink, "sink",
                                 record["expected_arc_ids"]),
                f"{row_label} replacement membership is not the exact edit")


def validate_expected_rows(
        descriptor: dict[str, Any], payloads: dict[int, dict[str, Any]],
        node_keys: dict[int, dict[str, Any]], base_out: list[object],
        out_offsets: list[int], base_in: list[object], in_offsets: list[int],
        aux: dict[int, list[int]], label: str, require: Require) -> None:
    """Validate ordered committed rows after reversing physical writes."""
    before = descriptor["before"]
    endpoints = _endpoint_ids(payloads, node_keys, label, require)
    _physical_rows(
        payloads, node_keys, base_out, out_offsets, base_in, in_offsets, aux,
        before["node_slots"], before["arc_slots"], label, require)
    for position, record in enumerate(descriptor["affected_base_in_rows"]):
        sink = record["sink"]
        actual = _row(base_in, in_offsets, sink,
                      f"{label}.base_in[{sink}]", require)
        require(actual == record["expected_arc_ids"],
                f"{label}.affected_base_in_rows[{position}].expected differs")
        _sequence(record["expected_arc_ids"], sink, 1, endpoints,
                  before["arc_slots"],
                  f"{label}.affected_base_in_rows[{position}].expected", require)
    for position, record in enumerate(descriptor["appended_base_out_rows"]):
        actual = _row(base_out, out_offsets, record["source"], label, require)
        require(actual == [] and not aux.get(record["source"], []),
                f"{label}.appended_base_out_rows[{position}] did not rewind")


def run_descriptor_row_self_test(require: Require) -> None:
    node_a = {"kind": "port", "full_name": "a", "instance_name": "",
              "pin_name": ""}
    node_b = {"kind": "port", "full_name": "b", "instance_name": "",
              "pin_name": ""}
    node_keys = {0: node_a, 1: node_b}
    payloads: dict[int, dict[str, Any]] = {}
    for arc_id in range(4):
        payloads[arc_id] = {"source": node_a, "sink": node_b}
    descriptor = {"before": {"node_slots": 2, "arc_slots": 2},
        "after": {"node_slots": 2, "arc_slots": 4}, "node_appends": [],
        "logical_deactivations": [{"id": 0, "source": 0, "sink": 1},
                                  {"id": 1, "source": 0, "sink": 1}],
        "arc_appends": [{"id": 2, "source": 0, "sink": 1},
                        {"id": 3, "source": 0, "sink": 1}],
        "appended_base_out_rows": [],
        "affected_base_in_rows": [{"sink": 1,
            "expected_arc_ids": [0, 1], "replacement_arc_ids": [2, 3]}]}
    errors: list[str] = []
    validate_candidate_rows(descriptor, payloads, node_keys, [2, 3],
        [0, 2, 2], [2, 3], [0, 0, 2], {}, "row-canary",
        lambda ok, message: errors.append(message) if not ok else None)
    require(not errors, f"valid candidate rows were rejected: {errors}")
    tampered = copy.deepcopy(descriptor)
    tampered["affected_base_in_rows"][0]["replacement_arc_ids"] = [3, 2]
    errors = []
    validate_candidate_rows(tampered, payloads, node_keys, [2, 3],
        [0, 2, 2], [2, 3], [0, 0, 2], {}, "row-order-tamper",
        lambda ok, message: errors.append(message) if not ok else None)
    require(errors, "same-membership incoming row reorder was accepted")
