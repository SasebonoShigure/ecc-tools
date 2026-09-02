#!/usr/bin/env python3
"""Semantic row and tombstone validation for Stage 5 structural receipts."""

import copy
from typing import Any, Callable

from dual_csr_typed_json_validator import validate_arc_payload

Require = Callable[[bool, str], None]


def _node_map(resolver: dict[str, Any], descriptors: list[dict[str, Any]],
              node_slots: int, label: str, require: Require) -> dict[int, object]:
    result: dict[int, object] = {}
    for position, record in enumerate(resolver["node_bindings"]):
        slot = record["binding"]["slot"]
        ref = record["ref"]
        if ref.get("kind") == "existing":
            require(slot not in result,
                    f"{label}.resolver.node_bindings[{position}] repeats a slot")
            result[slot] = ref["baseline_key"]
    for generation, descriptor in enumerate(descriptors):
        for position, record in enumerate(descriptor["node_appends"]):
            node_id = record["id"]
            require(node_id not in result,
                    f"{label}.descriptor[{generation}].node_appends[{position}] repeats a slot")
            result[node_id] = record["key"]
    require(set(result) == set(range(node_slots)),
            f"{label} cannot reconstruct every stable NodeId")
    return result


def _rows(offsets: list[int], entries: list[object]):
    for row in range(len(offsets) - 1):
        yield row, entries[offsets[row]:offsets[row + 1]]


def validate_structural_semantics(structural: dict[str, Any],
                                  resolver: dict[str, Any],
                                  descriptors: list[dict[str, Any]],
                                  label: str, require: Require) -> None:
    node_slots = structural["high_water"]["node_slots"]
    nodes = _node_map(resolver, descriptors, node_slots, label, require)
    slots = structural["arc_group_index"]["slots"]

    for row, arc_ids in _rows(structural["base_out"]["row_offsets"],
                              structural["base_out"]["physical_slots"]):
        for arc_id in arc_ids:
            if arc_id is None:
                continue
            key = slots[arc_id]["current_active_key"]
            require(key is not None and key["source"] == nodes[row],
                    f"{label}.base_out row {row} exposes a foreign source")
    for row, arc_ids in _rows(structural["base_in"]["row_offsets"],
                              structural["base_in"]["physical_slots"]):
        for arc_id in arc_ids:
            if arc_id is None:
                continue
            key = slots[arc_id]["current_active_key"]
            require(key is not None and key["sink"] == nodes[row],
                    f"{label}.base_in row {row} exposes a foreign sink")

    aux = structural["aux_out"]
    for aux_row, arc_ids in _rows(aux["row_offsets"], aux["arc_ids"]):
        source = aux["row_sources"][aux_row]
        for arc_id in arc_ids:
            key = slots[arc_id]["current_active_key"]
            require(key is not None and key["source"] == nodes[source],
                    f"{label}.aux_out row {aux_row} exposes a foreign source")
    expected_mapping = [-1] * node_slots
    for aux_row, source in enumerate(aux["row_sources"]):
        require(expected_mapping[source] == -1,
                f"{label}.aux_out repeats a source row")
        expected_mapping[source] = aux_row
    require(aux["source_to_aux_row"] == expected_mapping,
            f"{label}.aux_out source mapping is not exact")

    deactivations: dict[int, dict[str, Any]] = {}
    for descriptor in descriptors:
        for record in descriptor["logical_deactivations"]:
            deactivations[record["id"]] = record
    inactive_ids: set[int] = set()
    for position, record in enumerate(structural["inactive_arcs"]):
        arc_id = record["id"]
        inactive_ids.add(arc_id)
        require(slots[arc_id]["current_active_key"] is None,
                f"{label}.inactive_arcs[{position}] is active in the index")
        validate_arc_payload(record["historical_payload"],
                             f"{label}.inactive_arcs[{position}].historical_payload",
                             require)
        require(arc_id in deactivations,
                f"{label}.inactive_arcs[{position}] lacks a deactivation record")
        deactivation = deactivations[arc_id]
        payload = record["historical_payload"]
        require(payload["key"] == deactivation["key"]
                and payload["source"] == deactivation["source"]
                and payload["sink"] == deactivation["sink"],
                f"{label}.inactive_arcs[{position}] differs from its deactivation")
    expected_inactive = {slot["arc_id"] for slot in slots
                         if slot["current_active_key"] is None}
    require(inactive_ids == expected_inactive,
            f"{label}.inactive_arcs does not cover every tombstone")


def run_structural_semantics_self_test(require: Require) -> None:
    node0 = {"kind": "port", "full_name": "a", "instance_name": "",
             "pin_name": ""}
    node1 = {"kind": "port", "full_name": "b", "instance_name": "",
             "pin_name": ""}
    key = {"source": node0, "sink": node1, "kind": "net", "owner_name": "n",
           "library_source_port": "", "library_sink_port": "",
           "library_arcs": [], "parallel_ordinal": 0}
    number = {"kind": "finite", "value": 0.0, "negative_zero": False}
    payload = {"key": key, "source": 0, "sink": 1, "kind": "net",
        "is_clock_arc": False, "is_disabled": False, "is_loop_disabled": False,
        "eligibility": {"levelization": True, "data_arrival": True,
                        "data_slew": True, "clock": False},
        "transfer": {"delay": number, "delay_min": number,
                     "delay_max": number, "is_clock_arc": False,
                     "is_disabled": False, "is_clear_or_preset": False,
                     "children": []}, "output_load": None}
    structural = {"high_water": {"node_slots": 2},
        "arc_group_index": {"slots": [
            {"arc_id": 0, "current_active_key": key},
            {"arc_id": 1, "current_active_key": None}]},
        "base_out": {"row_offsets": [0, 1, 1], "physical_slots": [0]},
        "base_in": {"row_offsets": [0, 0, 1], "physical_slots": [0]},
        "aux_out": {"row_offsets": [0], "row_sources": [], "arc_ids": [],
                    "source_to_aux_row": [-1, -1]},
        "inactive_arcs": [{"id": 1, "historical_payload": payload}]}
    resolver = {"node_bindings": [
        {"ref": {"kind": "existing", "baseline_key": node0},
         "binding": {"slot": 0}},
        {"ref": {"kind": "existing", "baseline_key": node1},
         "binding": {"slot": 1}}]}
    descriptors = [{"node_appends": [], "logical_deactivations": [
        {"id": 1, "key": key, "source": 0, "sink": 1}]}]

    def errors(candidate: dict[str, Any]) -> list[str]:
        found: list[str] = []
        try:
            validate_structural_semantics(
                candidate, resolver, descriptors, "structural-canary",
                lambda ok, message: found.append(message) if not ok else None)
        except (KeyError, TypeError):
            found.append("typed structural rejection")
        return found

    require(not errors(structural), "valid structural semantics were rejected")
    row_swap = copy.deepcopy(structural)
    row_swap["base_out"]["row_offsets"] = [0, 0, 1]
    require(errors(row_swap), "foreign BaseOut row ArcId was accepted")
    mapping = copy.deepcopy(structural)
    mapping["aux_out"]["source_to_aux_row"][0] = 0
    require(errors(mapping), "extra Aux source mapping was accepted")
    historical = copy.deepcopy(structural)
    historical["inactive_arcs"][0]["historical_payload"] = {}
    require(errors(historical), "empty inactive historical payload was accepted")
