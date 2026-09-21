#!/usr/bin/env python3
"""Exact final-to-baseline replay for Dual-CSR reference payload deltas."""

import copy
import json
from collections.abc import Callable
from typing import Any

from dual_csr_baseline_reference_validator import validate_baseline_reference_input
from dual_csr_typed_json_validator import REFERENCE_APPEND_KEYS

Require = Callable[[bool, str], None]


def _canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _node_maps(resolver: dict[str, Any], functional: dict[str, Any],
               label: str, require: Require
               ) -> tuple[dict[int, dict[str, Any]], dict[str, int]]:
    by_name = {record["key"]["full_name"]: record["key"]
               for record in functional["graph"]["nodes"]}
    require(len(by_name) == len(functional["graph"]["nodes"]),
            f"{label} functional graph repeats a node name")
    by_id: dict[int, dict[str, Any]] = {}
    for position, record in enumerate(resolver["node_bindings"]):
        slot = record["binding"]["slot"]
        name = record["binding"]["native_name"]
        require(name in by_name and slot not in by_id,
                f"{label}.resolver.node_bindings[{position}] is not semantic")
        by_id[slot] = by_name[name]
    require(set(by_id) == set(range(len(by_name))),
            f"{label} node bindings do not cover the functional graph")
    by_key = {_canonical(key): node_id for node_id, key in by_id.items()}
    require(len(by_key) == len(by_id), f"{label} node keys are not unique")
    return by_id, by_key


def _node_id(value: object, by_key: dict[str, int], label: str,
             require: Require) -> int | None:
    if value is None:
        return None
    encoded = _canonical(value)
    require(encoded in by_key, f"{label} has no stable NodeId")
    return by_key[encoded]


def _records_with_node_ids(values: list[dict[str, Any]], fields: tuple[str, ...],
                           by_key: dict[str, int], label: str,
                           require: Require) -> list[dict[str, Any]]:
    result = copy.deepcopy(values)
    for index, record in enumerate(result):
        for field in fields:
            record[field] = _node_id(
                record[field], by_key, f"{label}[{index}].{field}", require)
    return result


def _ordered_node_records(values: list[dict[str, Any]],
                                ordered_nodes: list[dict[str, Any]], node_field: str,
                                label: str, require: Require
                                ) -> list[dict[str, Any]]:
    expected = [_canonical(key) for key in ordered_nodes]
    require(len(set(expected)) == len(expected),
            f"{label}.ordered_nodes repeats a NodeKey")
    by_node: dict[str, dict[str, Any]] = {}
    for index, record in enumerate(values):
        require(isinstance(record, dict) and node_field in record,
                f"{label}.records[{index}] has no record NodeKey")
        if not isinstance(record, dict) or node_field not in record:
            continue
        encoded = _canonical(record[node_field])
        require(encoded not in by_node,
                f"{label}.records[{index}] repeats a record NodeKey")
        if encoded not in by_node:
            by_node[encoded] = record
    exact_cover = (len(by_node) == len(values)
                   and set(by_node) == set(expected))
    require(exact_cover,
            f"{label}.records does not exactly cover ordered_nodes")
    if not exact_cover or len(set(expected)) != len(expected):
        return []
    return [by_node[key] for key in expected]


def _raw_reference(functional: dict[str, Any], identity: dict[str, Any],
                   resolver: dict[str, Any], session: int, generation: int,
                   label: str, require: Require
                   ) -> tuple[dict[str, object], dict[int, dict[str, Any]]]:
    reference = functional["graph"]["reference_payload"]
    by_id, by_key = _node_maps(resolver, functional, label, require)
    fingerprints = identity["current_fingerprints"]
    raw: dict[str, object] = {
        "schema_version": reference["schema_version"],
        "source_identity": {
            "fingerprint_schema_version": fingerprints["schema_version"],
            "session_token": session,
            "graph_generation": generation,
            "topology_fingerprint": fingerprints["topology"],
            "clock_constraint_fingerprint": fingerprints["clock_constraint"],
            "library_fingerprint": fingerprints["library"],
            "corner_fingerprint": fingerprints["corner"],
            "parasitic_fingerprint": fingerprints["parasitic"],
            "timing_input_fingerprint": fingerprints["timing_input"],
        },
        "analysis_context": copy.deepcopy(reference["analysis_context"]),
        "analysis_order": copy.deepcopy(reference["analysis_order"]),
        "path_source_order": copy.deepcopy(reference["path_source_order"]),
        "transition_order": copy.deepcopy(reference["transition_order"]),
        "clock_tags": copy.deepcopy(reference["clock_tags"]),
        "referenced_library_cells": copy.deepcopy(
            reference["referenced_library_cells"]),
        "parasitics": copy.deepcopy(reference["parasitics"]),
        "unsupported_reasons": copy.deepcopy(reference["unsupported_reasons"]),
    }
    instances = copy.deepcopy(reference["instances"])
    for index, record in enumerate(instances):
        record["ordered_pins"] = [
            _node_id(node, by_key,
                     f"{label}.instances[{index}].ordered_pins[{position}]",
                     require)
            for position, node in enumerate(record["ordered_pins"])
        ]
    raw["instances"] = instances
    for source, target in (("ordered_roots", "ordered_root_ids"),
                           ("ordered_starts", "ordered_start_ids"),
                           ("ordered_ends", "ordered_end_ids")):
        raw[target] = [
            _node_id(node, by_key, f"{label}.{source}[{index}]", require)
            for index, node in enumerate(reference[source])]

    clocks = copy.deepcopy(reference["clocks"])
    for clock_index, clock in enumerate(clocks):
        for source_index, source in enumerate(clock["ordered_sources"]):
            source["resolved_node"] = _node_id(
                source["resolved_node"], by_key,
                f"{label}.clocks[{clock_index}].ordered_sources[{source_index}]",
                require)
    raw["clocks"] = clocks
    owners: list[object] = [None] * len(by_id)
    seen_owners: set[int] = set()
    for index, owner in enumerate(reference["clock_owners"]):
        node_id = _node_id(owner["node"], by_key,
                           f"{label}.clock_owners[{index}].node", require)
        require(node_id not in seen_owners,
                f"{label}.clock_owners[{index}] repeats a node")
        seen_owners.add(node_id)
        owners[node_id] = owner["clock_name"]
    require(seen_owners == set(by_id),
            f"{label}.clock_owners does not cover every node")
    raw["clock_owner_by_node"] = owners
    raw["port_constraints"] = _records_with_node_ids(
        reference["port_constraints"], ("resolved_node",), by_key,
        f"{label}.port_constraints", require)
    raw["case_analysis_constraints"] = _records_with_node_ids(
        reference["case_analysis_constraints"], ("resolved_node",), by_key,
        f"{label}.case_analysis_constraints", require)
    raw["timing_check_endpoints"] = _records_with_node_ids(
        reference["timing_check_endpoints"],
        ("resolved_clock_node", "resolved_data_node"), by_key,
        f"{label}.timing_check_endpoints", require)
    ordered_seed_records = _ordered_node_records(
        reference["start_seeds"], reference["ordered_starts"], "seed_node",
        f"{label}.start_seeds", require)
    raw["start_seeds"] = _records_with_node_ids(
        ordered_seed_records,
        ("seed_node", "path_start_node", "crpr_node"), by_key,
        f"{label}.start_seeds", require)
    output_ports = {_canonical(record["key"]) for record in functional["graph"]["nodes"]
                    if record["is_port"] and record["direction"] in {"output", "inout"}}
    ordered_endpoints = [node for node in reference["ordered_ends"]
                         if _canonical(node) in output_ports]
    ordered_required = _ordered_node_records(
        reference["endpoint_required"], ordered_endpoints, "node",
        f"{label}.endpoint_required", require)
    raw["endpoint_required"] = _records_with_node_ids(
        ordered_required, ("node",), by_key, f"{label}.endpoint_required", require)
    raw["cell_pin_capacitances"] = _records_with_node_ids(
        reference["cell_pin_capacitances"], ("node",), by_key,
        f"{label}.cell_pin_capacitances", require)
    return raw, by_id


def _field_equal(field: str, semantic: object, raw: object) -> bool:
    if field != "cell_pin_capacitances":
        return semantic == raw
    if not isinstance(semantic, list) or not isinstance(raw, list):
        return False
    semantic_by_node = {record["node"]: record for record in semantic}
    raw_by_node = {record["node"]: record for record in raw}
    return (len(semantic_by_node) == len(semantic)
            and len(raw_by_node) == len(raw)
            and semantic_by_node == raw_by_node)


def _anchor_final_reference(descriptors: list[dict[str, Any]],
                            shadow: dict[str, object], label: str,
                            require: Require) -> None:
    for field in shadow:
        if field in REFERENCE_APPEND_KEYS:
            continue
        replacement = None
        replacement_index = -1
        for descriptor_index, descriptor in enumerate(descriptors):
            delta = descriptor["reference_payload"][field]
            if delta is not None:
                replacement = delta["replacement"]
                replacement_index = descriptor_index
        if replacement is None:
            continue
        require(_field_equal(field, shadow[field], replacement),
                f"{label}.descriptor_trace[{replacement_index}]"
                f".reference_payload.{field}.replacement differs from "
                "published functional reference")
        shadow[field] = copy.deepcopy(replacement)


def _reverse_reference(descriptors: list[dict[str, Any]],
                       shadow: dict[str, object], label: str,
                       require: Require) -> None:
    for descriptor_index in range(len(descriptors) - 1, -1, -1):
        delta = descriptors[descriptor_index]["reference_payload"]
        for field, record in delta.items():
            if record is None:
                continue
            field_label = (f"{label}.descriptor_trace[{descriptor_index}]"
                           f".reference_payload.{field}")
            if field in REFERENCE_APPEND_KEYS:
                values = record["values"]
                current = shadow[field]
                require(isinstance(current, list), f"{field_label} published shadow is not a list")
                if not isinstance(current, list):
                    return
                require(len(current) == record["expected_size"] + len(values),
                        f"{field_label} append extent differs from published shadow")
                if field == "clock_owner_by_node":
                    require(current[record["expected_size"]:] == values,
                            f"{field_label} appended owners differ from published shadow")
                    shadow[field] = current[:record["expected_size"]]
                else:
                    key = {"instances": "instance_name",
                           "referenced_library_cells": "cell_name",
                           "cell_pin_capacitances": "node"}[field]
                    appended = {value[key]: value for value in values}
                    published = {value[key]: value for value in current}
                    require(len(appended) == len(values) and len(published) == len(current),
                            f"{field_label} contains duplicate record identities")
                    require(all(published.get(identity) == value
                                for identity, value in appended.items()),
                            f"{field_label} appended values differ from published shadow")
                    shadow[field] = [value for value in current if value[key] not in appended]
                continue
            require(field in shadow and shadow[field] == record["replacement"],
                    f"{field_label}.replacement differs from published shadow")
            shadow[field] = copy.deepcopy(record["expected"])


def _referenced_node_ids(reference: dict[str, object]) -> list[int]:
    result: list[int] = []
    result.extend(reference["ordered_root_ids"])
    result.extend(reference["ordered_start_ids"])
    result.extend(reference["ordered_end_ids"])
    for instance in reference["instances"]:
        result.extend(instance["ordered_pins"])
    for clock in reference["clocks"]:
        result.extend(source["resolved_node"] for source in clock["ordered_sources"]
                      if source["resolved_node"] is not None)
    for field in ("port_constraints", "case_analysis_constraints"):
        result.extend(record["resolved_node"] for record in reference[field]
                      if record["resolved_node"] is not None)
    for record in reference["timing_check_endpoints"]:
        result.extend(value for value in
                      (record["resolved_clock_node"], record["resolved_data_node"])
                      if value is not None)
    for record in reference["start_seeds"]:
        result.extend((record["seed_node"], record["path_start_node"]))
        if record["crpr_node"] is not None:
            result.append(record["crpr_node"])
    result.extend(record["node"] for record in reference["endpoint_required"])
    result.extend(record["node"]
                  for record in reference["cell_pin_capacitances"])
    return result


def validate_reference_replay(descriptors: list[dict[str, Any]],
                              functional: dict[str, Any],
                              identity: dict[str, Any],
                              resolver: dict[str, Any], session: int,
                              baseline_reference_input: dict[str, Any],
                              generation: int,
                              expected_batches: list[dict[str, Any]],
                              label: str, require: Require) -> None:
    """Anchor final typed reference fields and rewind them to generation zero."""
    shadow, by_id = _raw_reference(
        functional, identity, resolver, session, generation, label, require)
    _anchor_final_reference(descriptors, shadow, label, require)
    _reverse_reference(descriptors, shadow, label, require)
    baseline = expected_batches[0]["baseline"]
    anchor = validate_baseline_reference_input(
        baseline_reference_input, baseline, session,
        f"{label}.baseline_reference_input", require)
    require(shadow.keys() == anchor.keys()
            and all(_field_equal(field, value, anchor[field])
                    for field, value in shadow.items()),
            f"{label} generation-zero reference differs from full anchor")
    fingerprints = baseline["fingerprints"]
    expected_identity = {
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
    require(shadow["source_identity"] == expected_identity,
            f"{label} generation-zero source identity differs from the trace")
    state = baseline["canonical_pre_state"]
    baseline_nodes = len(state["nodes"])
    ids = _referenced_node_ids(shadow)
    require(all(type(node_id) is int and 0 <= node_id < baseline_nodes
                for node_id in ids),
            f"{label} generation-zero reference reaches an appended node")
    require(len(shadow["clock_owner_by_node"]) == baseline_nodes,
            f"{label} generation-zero clock-owner extent differs")
    instance_names = [record["instance_name"] for record in shadow["instances"]]
    require(sorted(instance_names) == sorted(state["instances"]),
            f"{label} generation-zero reference instances differ")
    baseline_keys = {_canonical(key) for key in state["nodes"]}
    require(all(_canonical(by_id[node_id]) in baseline_keys for node_id in ids),
            f"{label} generation-zero reference node identity differs")


def run_reference_replay_self_test(require: Require) -> None:
    capacitances = [{"node": 5, "rise_capacitance": 0.25, "fall_capacitance": 0.5},
                    {"node": 2, "rise_capacitance": 0.5, "fall_capacitance": 0.75}]
    require(_field_equal("cell_pin_capacitances", capacitances, capacitances[::-1]),
            "semantic capacitance ordering was treated as a value change")
    changed = copy.deepcopy(capacitances)
    changed[0]["fall_capacitance"] = 0.125
    for invalid in (changed, capacitances[:1], [capacitances[0], capacitances[0]]):
        require(not _field_equal("cell_pin_capacitances", capacitances, invalid),
                "capacitance value or exact coverage tamper was accepted")
    require(not _field_equal("ordered_start_ids", [5, 2], [2, 5]),
            "reference anchor lost order-sensitive start identities")

    expected = [{"instance_name": "u0", "cell_name": "OR2"}]
    replacement = [{"instance_name": "b0", "cell_name": "BUF"}, *expected]
    descriptor: dict[str, Any] = {"reference_payload": {
        "instances": {"expected_size": len(expected), "values": replacement[:1]}}}
    shadow: dict[str, object] = {"instances": copy.deepcopy(replacement)}
    errors: list[str] = []

    def collect_error(ok: object, message: str) -> None:
        if not ok:
            errors.append(message)

    _reverse_reference([descriptor], shadow, "reference-replay-canary",
                       collect_error)
    require(not errors and shadow["instances"] == expected,
            f"valid reference replay was rejected: {errors}")
    tampered = copy.deepcopy(descriptor)
    tampered["reference_payload"]["instances"]["values"][0][
        "cell_name"] = "WRONG_CELL"
    errors = []
    _reverse_reference([tampered], {"instances": copy.deepcopy(replacement)},
                       "reference-replay-tamper",
                       collect_error)
    require(errors, "same-size reference append tamper was accepted")

    before = {"liberty_corner": "c0", "temperature": "25",
              "rc_corner": "r0"}
    sentinel = {"liberty_corner": "c1",
                "temperature": "__unspecified_temperature__",
                "rc_corner": "r1"}
    after = {"liberty_corner": "c2", "temperature": "30",
             "rc_corner": "r2"}
    context_descriptors = [
        {"reference_payload": {"analysis_context": {
            "expected": before, "replacement": sentinel}}},
        {"reference_payload": {"analysis_context": {
            "expected": sentinel, "replacement": after}}},
    ]
    context_shadow: dict[str, object] = {
        "analysis_context": copy.deepcopy(after)}
    errors = []
    _reverse_reference(
        context_descriptors, context_shadow, "analysis-context-wire-canary",
        collect_error)
    require(not errors and context_shadow["analysis_context"] == before,
            f"valid analysis-context wire replay was rejected: {errors}")

    def node(name: str) -> dict[str, str]:
        return {"kind": "port", "full_name": name, "instance_name": "",
                "pin_name": ""}

    def seed(key: dict[str, str]) -> dict[str, object]:
        return {"seed_node": key, "path_start_node": key, "crpr_node": None}

    first = node("first")
    second = node("second")
    unknown = node("unknown")
    errors = []
    reordered = _ordered_node_records(
        [seed(second), seed(first)], [first, second], "seed_node", "seed-order-canary",
        collect_error)
    require(not errors
            and [item["seed_node"] for item in reordered] == [first, second],
            f"projected start seeds did not recover ordered_starts: {errors}")

    for label, candidate in (
        ("duplicate", [seed(first), seed(first)]),
        ("missing", [seed(first)]),
        ("unknown", [seed(first), seed(unknown)]),
    ):
        errors = []
        _ordered_node_records(
            candidate, [first, second], "seed_node", f"seed-{label}-canary",
            collect_error)
        require(errors, f"{label} projected start-seed tamper was accepted")


    required_first = {"node": first, "clock_period": 10.0}
    required_second = {"node": second, "clock_period": 20.0}
    errors = []
    ordered_required = _ordered_node_records(
        [required_second, required_first], [first, second], "node", "required-order-canary",
        collect_error)
    require(not errors and ordered_required == [required_first, required_second],
            "endpoint ordering changed payload values or failed to recover ordered ends")
    for candidate in ([required_first], [required_first, required_first]):
        errors = []
        _ordered_node_records(
            candidate, [first, second], "node", "required-cover-canary",
            collect_error)
        require(bool(errors), "endpoint order reconstruction accepted missing/duplicate records")
