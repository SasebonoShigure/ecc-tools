#!/usr/bin/env python3
"""Shadow replay for Stage 5 Dual-CSR descriptor receipts."""

import bisect
import copy
import json
from typing import Any, Callable

from dual_csr_descriptor_row_validator import (
    validate_candidate_rows, validate_expected_rows)

Require = Callable[[bool, str], None]


def _canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _node_id_keys(resolver: dict[str, Any], functional: dict[str, Any],
                  label: str, require: Require) -> dict[int, dict[str, Any]]:
    nodes = functional["graph"]["nodes"]
    by_name: dict[str, dict[str, Any]] = {}
    for index, record in enumerate(nodes):
        key = record["key"]
        name = key["full_name"]
        require(name not in by_name,
                f"{label}.functional.nodes[{index}] repeats a full name")
        by_name[name] = key
    result: dict[int, dict[str, Any]] = {}
    for index, entry in enumerate(resolver["node_bindings"]):
        binding = entry["binding"]
        slot = binding["slot"]
        name = binding["native_name"]
        require(name in by_name,
                f"{label}.resolver.node_bindings[{index}] has no semantic node")
        require(slot not in result,
                f"{label}.resolver.node_bindings[{index}] repeats a NodeId")
        result[slot] = by_name[name]
        ref = entry["ref"]
        if ref["kind"] == "existing":
            require(ref["baseline_key"] == result[slot],
                    f"{label}.resolver.node_bindings[{index}] baseline key differs")
    require(set(result) == set(range(len(nodes))),
            f"{label}.resolver node bindings do not cover functional nodes")
    return result


def _semantic_payload(value: dict[str, Any],
                      node_keys: dict[int, dict[str, Any]],
                      label: str, require: Require) -> dict[str, Any]:
    result = copy.deepcopy(value)
    for field in ("source", "sink"):
        node = result[field]
        if type(node) is int:
            require(node in node_keys, f"{label}.{field} is out of NodeId range")
            result[field] = copy.deepcopy(node_keys[node])
    require(result["source"] == result["key"]["source"]
            and result["sink"] == result["key"]["sink"],
            f"{label} endpoint IDs differ from the semantic ArcKey")
    return result


def _published_payloads(structural: dict[str, Any],
                        functional: dict[str, Any],
                        node_keys: dict[int, dict[str, Any]],
                        label: str, require: Require
                        ) -> tuple[dict[int, dict[str, Any]], set[int]]:
    graph = functional["graph"]
    active_by_key: dict[str, dict[str, Any]] = {}
    for index, payload in enumerate(graph["active_arcs"]):
        encoded = _canonical(payload["key"])
        require(encoded not in active_by_key,
                f"{label}.functional.active_arcs[{index}] repeats an ArcKey")
        active_by_key[encoded] = payload
    payloads: dict[int, dict[str, Any]] = {}
    active_ids: set[int] = set()
    for slot in structural["arc_group_index"]["slots"]:
        arc_id = slot["arc_id"]
        key = slot["current_active_key"]
        if key is None:
            continue
        encoded = _canonical(key)
        require(encoded in active_by_key,
                f"{label}.index ArcId {arc_id} lacks a functional payload")
        payloads[arc_id] = copy.deepcopy(active_by_key[encoded])
        active_ids.add(arc_id)
    for position, inactive in enumerate(structural["inactive_arcs"]):
        arc_id = inactive["id"]
        require(arc_id not in payloads,
                f"{label}.inactive_arcs[{position}] is also active")
        payloads[arc_id] = _semantic_payload(
            inactive["historical_payload"], node_keys,
            f"{label}.inactive_arcs[{position}].historical_payload", require)
    extent = structural["high_water"]["arc_slots"]
    require(set(payloads) == set(range(extent)),
            f"{label} published payloads do not cover stable ArcIds")
    require(len(active_by_key) == len(active_ids),
            f"{label} functional graph has an unindexed active payload")
    return payloads, active_ids


def _published_nodes(functional: dict[str, Any],
                     node_keys: dict[int, dict[str, Any]],
                     label: str, require: Require
                     ) -> dict[int, dict[str, Any]]:
    by_key = {_canonical(record["key"]): record
              for record in functional["graph"]["nodes"]}
    require(len(by_key) == len(functional["graph"]["nodes"]),
            f"{label}.functional graph repeats a NodeKey")
    result: dict[int, dict[str, Any]] = {}
    for node_id, key in node_keys.items():
        encoded = _canonical(key)
        require(encoded in by_key,
                f"{label}.NodeId {node_id} lacks a functional node record")
        result[node_id] = {"id": node_id, **copy.deepcopy(by_key[encoded])}
    return result



def _row_for_slot(offsets: list[int], physical_slot: int, label: str,
                  require: Require) -> int:
    require(type(physical_slot) is int
            and 0 <= physical_slot < offsets[-1],
            f"{label} physical slot is out of range")
    return bisect.bisect_right(offsets, physical_slot) - 1


def _aux_shadow(value: dict[str, Any]) -> dict[int, list[int]]:
    result: dict[int, list[int]] = {}
    offsets = value["row_offsets"]
    for row, source in enumerate(value["row_sources"]):
        result[source] = list(value["arc_ids"][offsets[row]:offsets[row + 1]])
    return result


def _base_occurrence_rows(slots: list[object], offsets: list[int],
                          arc_id: int) -> list[int]:
    result: list[int] = []
    for row in range(len(offsets) - 1):
        result.extend(row for value in slots[offsets[row]:offsets[row + 1]]
                      if value == arc_id)
    return result


def _aux_occurrence_rows(aux: dict[int, list[int]], arc_id: int) -> list[int]:
    return [source for source, row in aux.items()
            for value in row if value == arc_id]


def _verify_appended_adjacency(descriptor: dict[str, Any],
                               base_out: list[object], out_offsets: list[int],
                               base_in: list[object], in_offsets: list[int],
                               aux: dict[int, list[int]], label: str,
                               require: Require) -> set[int]:
    result: set[int] = set()
    for position, record in enumerate(descriptor["arc_appends"]):
        arc_id = record["id"]
        source = record["payload"]["source"]
        sink = record["payload"]["sink"]
        item_label = f"{label}.arc_appends[{position}]"
        require(arc_id not in result,
                f"{item_label} repeats an appended ArcId")
        result.add(arc_id)
        outgoing = (_base_occurrence_rows(base_out, out_offsets, arc_id)
                    + _aux_occurrence_rows(aux, arc_id))
        incoming = _base_occurrence_rows(base_in, in_offsets, arc_id)
        require(outgoing == [source] and incoming == [sink],
                f"{item_label} does not have one endpoint-correct adjacency")
    return result


def _erase_appended_adjacency(appended: set[int],
                              base_out: list[object], base_in: list[object],
                              aux: dict[int, list[int]]) -> None:
    for position, arc_id in enumerate(base_out):
        if arc_id in appended:
            base_out[position] = None
    for position, arc_id in enumerate(base_in):
        if arc_id in appended:
            base_in[position] = None
    for source in list(aux):
        aux[source] = [arc_id for arc_id in aux[source]
                       if arc_id not in appended]
        if not aux[source]:
            del aux[source]


def _active_adjacency(base_out: list[object], base_in: list[object],
                      aux: dict[int, list[int]],
                      index: dict[int, object], label: str,
                      require: Require) -> None:
    out_values = [item for item in base_out if item is not None]
    auxiliary_values = [item for row in aux.values() for item in row]
    incoming_values = [item for item in base_in if item is not None]
    out = set(out_values)
    auxiliary = set(auxiliary_values)
    incoming = set(incoming_values)
    active = {arc_id for arc_id, key in index.items() if key is not None}
    require(len(out_values) == len(out)
            and len(auxiliary_values) == len(auxiliary)
            and len(incoming_values) == len(incoming)
            and not out & auxiliary and out | auxiliary == incoming == active,
            f"{label} adjacency shadow differs from the active index")


def _baseline_rows(nodes: dict[int, dict[str, Any]],
                   index: dict[int, dict[str, Any]],
                   base_out: list[object], out_offsets: list[int],
                   base_in: list[object], in_offsets: list[int],
                   aux: dict[int, list[int]], label: str,
                   require: Require) -> None:
    ids = {_canonical(record["key"]): node_id
           for node_id, record in nodes.items()}
    expected_out = {node_id: set() for node_id in nodes}
    expected_in = {node_id: set() for node_id in nodes}
    for arc_id, key in index.items():
        source = ids[_canonical(key["source"])]
        sink = ids[_canonical(key["sink"])]
        expected_out[source].add(arc_id)
        expected_in[sink].add(arc_id)
    for node_id in nodes:
        actual_out = {item for item in
                      base_out[out_offsets[node_id]:out_offsets[node_id + 1]]
                      if item is not None}
        actual_in = {item for item in
                     base_in[in_offsets[node_id]:in_offsets[node_id + 1]]
                     if item is not None}
        require(actual_out == expected_out[node_id]
                and actual_in == expected_in[node_id],
                f"{label} generation-zero row {node_id} differs")
    require(not any(aux.values()),
            f"{label} generation-zero AuxOut is not empty")


def _generation_zero_anchor(
        nodes: dict[int, dict[str, Any]],
        payloads: dict[int, dict[str, Any]],
        index: dict[int, dict[str, Any]], base_out: list[object],
        out_offsets: list[int], base_in: list[object], in_offsets: list[int],
        aux: dict[int, list[int]], anchor: dict[str, Any], label: str,
        require: Require) -> None:
    anchor_nodes = anchor["nodes"]
    require(set(nodes) == set(range(len(anchor_nodes))),
            f"{label}.nodes extent differs from the full anchor")
    for node_id, expected in enumerate(anchor_nodes):
        require(nodes.get(node_id) == expected,
                f"{label}.nodes[{node_id}] differs from the full anchor")
    expected_node_lookup = {
        _canonical(record["key"]): record["id"] for record in anchor_nodes}
    actual_node_lookup = {
        _canonical(record["key"]): record["id"]
        for record in anchor["node_ids_by_key"]}
    require(actual_node_lookup == expected_node_lookup,
            f"{label}.node_ids_by_key differs from the rewound nodes")

    anchor_arcs = anchor["arcs"]
    require(set(payloads) == set(index) == set(range(len(anchor_arcs))),
            f"{label}.arcs extent differs from the full anchor")
    node_keys = {node_id: record["key"] for node_id, record in nodes.items()}
    for arc_id, expected in enumerate(anchor_arcs):
        require(index.get(arc_id) == expected["key"],
                f"{label}.arcs[{arc_id}].key differs from the rewound index")
        raw_payload = {name: copy.deepcopy(field)
                       for name, field in expected.items()
                       if name not in {"id", "native_traversal_rank"}}
        semantic = _semantic_payload(
            raw_payload, node_keys, f"{label}.arcs[{arc_id}]", require)
        require(payloads.get(arc_id) == semantic,
                f"{label}.arcs[{arc_id}] payload differs from the full anchor")
    expected_arc_lookup = {
        _canonical(record["key"]): record["id"] for record in anchor_arcs}
    actual_arc_lookup = {
        _canonical(record["key"]): record["id"]
        for record in anchor["arc_ids_by_key"]}
    require(actual_arc_lookup == expected_arc_lookup,
            f"{label}.arc_ids_by_key differs from the rewound arcs")

    node_count = len(anchor_nodes)
    require(len(out_offsets) >= node_count + 1
            and out_offsets[:node_count + 1] == anchor["out_row_offsets"],
            f"{label}.out_row_offsets differs from the full anchor")
    require(len(in_offsets) >= node_count + 1
            and in_offsets[:node_count + 1] == anchor["in_row_offsets"],
            f"{label}.in_row_offsets differs from the full anchor")
    if len(out_offsets) >= node_count + 1:
        boundary = out_offsets[node_count]
        require(base_out[:boundary] == anchor["out_arc_ids"]
                and all(value is None for value in base_out[boundary:]),
                f"{label}.out_arc_ids differs from the rewound BaseOut")
    if len(in_offsets) >= node_count + 1:
        boundary = in_offsets[node_count]
        require(base_in[:boundary] == anchor["in_arc_ids"]
                and all(value is None for value in base_in[boundary:]),
                f"{label}.in_arc_ids differs from the rewound BaseIn")
    require(not any(aux.values()),
            f"{label}.aux_out differs from the generation-zero anchor")


def validate_descriptor_replay(descriptors: list[dict[str, Any]],
                               structural: dict[str, Any],
                               functional: dict[str, Any],
                               resolver: dict[str, Any],
                               baseline_reference_input: dict[str, Any],
                               expected_batches: list[dict[str, Any]],
                               label: str, require: Require) -> None:
    """Reverse every descriptor from the published state to generation zero."""
    node_keys = _node_id_keys(resolver, functional, label, require)
    payloads, final_active = _published_payloads(
        structural, functional, node_keys, label, require)
    nodes = _published_nodes(functional, node_keys, label, require)
    index = {slot["arc_id"]: copy.deepcopy(slot["current_active_key"])
             for slot in structural["arc_group_index"]["slots"]}
    base_out = list(structural["base_out"]["physical_slots"])
    out_offsets = structural["base_out"]["row_offsets"]
    base_in = list(structural["base_in"]["physical_slots"])
    in_offsets = structural["base_in"]["row_offsets"]
    aux = _aux_shadow(structural["aux_out"])
    _active_adjacency(base_out, base_in, aux, index, label, require)
    final_inactive = set(payloads) - final_active

    for descriptor_index in range(len(descriptors) - 1, -1, -1):
        descriptor = descriptors[descriptor_index]
        item_label = f"{label}.descriptor_trace[{descriptor_index}]"

        validate_candidate_rows(descriptor, payloads, node_keys, base_out,
            out_offsets, base_in, in_offsets, aux, item_label, require)

        appended = _verify_appended_adjacency(
            descriptor, base_out, out_offsets, base_in, in_offsets, aux,
            item_label, require)

        seen: set[int] = set()
        for position, record in enumerate(descriptor["logical_deactivations"]):
            arc_id = record["id"]
            require(arc_id not in seen,
                    f"{item_label}.logical_deactivations repeats ArcId {arc_id}")
            seen.add(arc_id)
            require(arc_id in final_inactive and arc_id in payloads,
                    f"{item_label}.logical_deactivations[{position}] is not inactive")
            current = payloads[arc_id]
            require(current["key"] == record["key"]
                    and current["source"] == node_keys[record["source"]]
                    and current["sink"] == node_keys[record["sink"]],
                    f"{item_label}.logical_deactivations[{position}] payload differs")

        seen.clear()
        for position in range(len(descriptor["payload_changes"]) - 1, -1, -1):
            record = descriptor["payload_changes"][position]
            arc_id = record["id"]
            require(arc_id not in seen and arc_id in payloads,
                    f"{item_label}.payload_changes[{position}] ArcId differs")
            seen.add(arc_id)
            replacement = _semantic_payload(
                record["replacement"], node_keys,
                f"{item_label}.payload_changes[{position}].replacement", require)
            expected = _semantic_payload(
                record["expected"], node_keys,
                f"{item_label}.payload_changes[{position}].expected", require)
            require(payloads[arc_id] == replacement,
                    f"{item_label}.payload_changes[{position}] replacement "
                    "does not match the published shadow")
            payloads[arc_id] = expected

        seen.clear()
        for position in range(len(descriptor["arc_group_index_delta"]) - 1, -1, -1):
            record = descriptor["arc_group_index_delta"][position]
            arc_id = record["id"]
            require(arc_id not in seen and arc_id in index,
                    f"{item_label}.arc_group_index_delta[{position}] ArcId differs")
            seen.add(arc_id)
            require(index[arc_id] == record["replacement_active_key"],
                    f"{item_label}.arc_group_index_delta[{position}] "
                    "replacement does not match the published shadow")
            index[arc_id] = copy.deepcopy(record["expected_active_key"])


        seen_slots: set[int] = set()
        for position in range(len(descriptor["aux_rows"]) - 1, -1, -1):
            record = descriptor["aux_rows"][position]
            source = record["source"]
            require(source not in seen_slots,
                    f"{item_label}.aux_rows[{position}] repeats a source")
            seen_slots.add(source)
            require(aux.get(source, []) == record["replacement_arc_ids"],
                    f"{item_label}.aux_rows[{position}] replacement differs")
            if record["expected_arc_ids"]:
                aux[source] = list(record["expected_arc_ids"])
            else:
                aux.pop(source, None)

        seen_slots.clear()
        for position in range(len(descriptor["reverse_changes"]) - 1, -1, -1):
            record = descriptor["reverse_changes"][position]
            slot = record["physical_slot"]
            row = _row_for_slot(in_offsets, slot,
                                f"{item_label}.reverse_changes[{position}]",
                                require)
            require(slot not in seen_slots and row == record["sink"]
                    and base_in[slot] == record["replacement_arc"],
                    f"{item_label}.reverse_changes[{position}] replacement differs")
            seen_slots.add(slot)
            base_in[slot] = record["expected_arc"]

        seen_slots.clear()
        for position in range(len(descriptor["changes"]) - 1, -1, -1):
            record = descriptor["changes"][position]
            slot = record["physical_slot"]
            row = _row_for_slot(out_offsets, slot,
                                f"{item_label}.changes[{position}]", require)
            require(slot not in seen_slots and row == record["source"]
                    and base_out[slot] == record["replacement_arc"],
                    f"{item_label}.changes[{position}] replacement differs")
            seen_slots.add(slot)
            base_out[slot] = record["expected_arc"]

        for position in range(len(descriptor["removes"]) - 1, -1, -1):
            record = descriptor["removes"][position]
            slot = record["physical_slot"]
            row = _row_for_slot(out_offsets, slot,
                                f"{item_label}.removes[{position}]", require)
            require(slot not in seen_slots and row == record["source"]
                    and base_out[slot] is None,
                    f"{item_label}.removes[{position}] replacement differs")
            seen_slots.add(slot)
            base_out[slot] = record["expected_arc"]

        for position in range(len(descriptor["node_payload_changes"]) - 1, -1, -1):
            record = descriptor["node_payload_changes"][position]
            node_id = record["id"]
            require(node_id in nodes
                    and nodes[node_id]["key"] == record["expected_key"]
                    and nodes[node_id]["net_name"] == record["replacement_net_name"],
                    f"{item_label}.node_payload_changes[{position}] "
                    "replacement does not match the published shadow")
            nodes[node_id]["net_name"] = record["expected_net_name"]

        _erase_appended_adjacency(appended, base_out, base_in, aux)
        validate_expected_rows(descriptor, payloads, node_keys, base_out,
                               out_offsets, base_in, in_offsets, aux,
                               item_label, require)
        for position in range(len(descriptor["arc_appends"]) - 1, -1, -1):
            record = descriptor["arc_appends"][position]
            arc_id = record["id"]
            appended = _semantic_payload(
                record["payload"], node_keys,
                f"{item_label}.arc_appends[{position}].payload", require)
            require(arc_id in payloads and payloads[arc_id] == appended,
                    f"{item_label}.arc_appends[{position}] payload differs")
            require(index.get(arc_id) is None,
                    f"{item_label}.arc_appends[{position}] index did not rewind")
            del payloads[arc_id]
            del index[arc_id]

        for position in range(len(descriptor["node_appends"]) - 1, -1, -1):
            record = descriptor["node_appends"][position]
            node_id = record["id"]
            require(node_id in nodes and nodes[node_id] == record["payload"],
                    f"{item_label}.node_appends[{position}] payload differs")
            del nodes[node_id]

        _active_adjacency(base_out, base_in, aux, index,
                          f"{item_label}.before_adjacency", require)
        before = descriptor["before"]
        require(set(nodes) == set(range(before["node_slots"]))
                and set(payloads) == set(range(before["arc_slots"]))
                and set(index) == set(range(before["arc_slots"])),
                f"{item_label} did not rewind to its before high-water")

    baseline = expected_batches[0]["baseline"]["canonical_pre_state"]
    require(sorted(map(_canonical, (record["key"] for record in nodes.values())))
            == sorted(map(_canonical, baseline["nodes"])),
            f"{label} generation-zero node shadow differs from the trace")
    require(all(key is not None for key in index.values())
            and sorted(map(_canonical, index.values()))
            == sorted(map(_canonical, baseline["arcs"])),
            f"{label} generation-zero index shadow differs from the trace")
    require(sorted(map(_canonical, (payload["key"] for payload in payloads.values())))
            == sorted(map(_canonical, baseline["arcs"])),
            f"{label} generation-zero payload shadow differs from the trace")
    _baseline_rows(nodes, index, base_out, out_offsets, base_in, in_offsets,
                   aux, f"{label}.generation_zero_adjacency", require)
    _generation_zero_anchor(
        nodes, payloads, index, base_out, out_offsets, base_in, in_offsets, aux,
        baseline_reference_input, f"{label}.generation_zero_anchor", require)


def run_descriptor_replay_self_test(require: Require) -> None:
    number0 = {"kind": "finite", "value": 0.0, "negative_zero": False}
    number1 = {"kind": "finite", "value": 1.0, "negative_zero": False}
    node_a = {"kind": "port", "full_name": "a", "instance_name": "",
              "pin_name": ""}
    node_b = {"kind": "port", "full_name": "b", "instance_name": "",
              "pin_name": ""}
    key = {"source": node_a, "sink": node_b, "kind": "net",
           "owner_name": "n", "library_source_port": "",
           "library_sink_port": "", "library_arcs": [], "parallel_ordinal": 0}

    def payload(delay: dict[str, Any], semantic: bool) -> dict[str, Any]:
        return {"key": key, "source": node_a if semantic else 0,
                "sink": node_b if semantic else 1, "kind": "net",
                "is_clock_arc": False, "is_disabled": False,
                "is_loop_disabled": False, "eligibility": {
                    "levelization": True, "data_arrival": True,
                    "data_slew": True, "clock": False}, "transfer": {
                    "delay": delay, "delay_min": delay, "delay_max": delay,
                    "is_clock_arc": False, "is_disabled": False,
                    "is_clear_or_preset": False, "children": []},
                "output_load": None}

    def node_record(key_value: dict[str, Any], net_name: str) -> dict[str, Any]:
        return {"key": key_value, "direction": "input", "instance_cell": "",
                "net_name": net_name, "is_port": True,
                "is_startpoint": False, "is_endpoint": False,
                "is_sequential": False, "is_clock_pin": False,
                "is_data_pin": True, "is_output_pin": False}

    functional_nodes = [node_record(node_a, "n"),
                        node_record(node_b, "n")]
    descriptor = {"before": {"node_slots": 2, "net_slots": 1, "arc_slots": 1},
        "after": {"node_slots": 2, "net_slots": 1, "arc_slots": 1},
        "node_appends": [], "arc_appends": [], "node_payload_changes": [],
        "logical_deactivations": [], "arc_group_index_delta": [],
        "payload_changes": [{"id": 0, "expected": payload(number0, False),
                             "replacement": payload(number1, False)}],
        "removes": [], "changes": [], "reverse_changes": [],
        "aux_rows": [], "appended_base_out_rows": [],
        "affected_base_out_rows": [], "affected_base_in_rows": []}
    functional = {"graph": {"nodes": functional_nodes,
                              "active_arcs": [payload(number1, True)]}}
    structural = {"high_water": {"node_slots": 2, "net_slots": 1, "arc_slots": 1},
        "arc_group_index": {"slots": [{"arc_id": 0,
                                       "current_active_key": key}]},
        "inactive_arcs": [],
        "base_out": {"row_offsets": [0, 1, 1], "physical_slots": [0]},
        "base_in": {"row_offsets": [0, 0, 1], "physical_slots": [0]},
        "aux_out": {"row_offsets": [0], "row_sources": [], "arc_ids": []}}
    resolver = {"node_bindings": [
        {"ref": {"kind": "existing", "baseline_key": node_a},
         "binding": {"slot": 0, "native_name": "a", "tombstone": False}},
        {"ref": {"kind": "existing", "baseline_key": node_b},
         "binding": {"slot": 1, "native_name": "b", "tombstone": False}}]}
    batches = [{"baseline": {"canonical_pre_state": {
        "nodes": [node_a, node_b], "arcs": [key]}}}]
    baseline = {"nodes": [{"id": node_id, **record}
                          for node_id, record in enumerate(functional_nodes)],
        "node_ids_by_key": [{"key": node_a, "id": 0},
                            {"key": node_b, "id": 1}],
        "arcs": [{"id": 0, "native_traversal_rank": 0,
                  **payload(number0, False)}],
        "arc_ids_by_key": [{"key": key, "id": 0}],
        "out_row_offsets": [0, 1, 1], "out_arc_ids": [0],
        "in_row_offsets": [0, 0, 1], "in_arc_ids": [0]}

    errors: list[str] = []
    validate_descriptor_replay([descriptor], structural, functional, resolver,
                               baseline, batches, "descriptor-replay-canary",
                               lambda ok, message: errors.append(message)
                               if not ok else None)
    require(not errors, f"valid descriptor replay was rejected: {errors}")
    tampered = copy.deepcopy(descriptor)
    tampered["payload_changes"][0]["replacement"]["transfer"]["delay"] = {
        "kind": "finite", "value": 123.0, "negative_zero": False}
    errors = []
    validate_descriptor_replay([tampered], structural, functional, resolver,
                               baseline, batches, "descriptor-payload-tamper",
                               lambda ok, message: errors.append(message)
                               if not ok else None)
    require(errors, "descriptor replacement payload tamper was accepted")
    tampered_anchor = copy.deepcopy(baseline)
    tampered_anchor["nodes"][0]["net_name"] = "WRONG_NET"
    errors = []
    validate_descriptor_replay(
        [descriptor], structural, functional, resolver, tampered_anchor,
        batches, "baseline-node-payload-tamper",
        lambda ok, message: errors.append(message) if not ok else None)
    require(errors, "generation-zero node payload tamper was accepted")
    tampered_anchor = copy.deepcopy(baseline)
    tampered_anchor["arcs"][0]["transfer"]["delay"] = {
        "kind": "finite", "value": 123.0, "negative_zero": False}
    errors = []
    validate_descriptor_replay(
        [descriptor], structural, functional, resolver, tampered_anchor,
        batches, "baseline-arc-payload-tamper",
        lambda ok, message: errors.append(message) if not ok else None)
    require(errors, "generation-zero arc payload tamper was accepted")
    key2 = copy.deepcopy(key)
    key2["parallel_ordinal"] = 1
    semantic0 = payload(number0, True)
    semantic1 = payload(number0, True)
    semantic1["key"] = key2
    raw1 = payload(number0, False)
    raw1["key"] = key2
    ordered_anchor = copy.deepcopy(baseline)
    ordered_anchor["arcs"].append(
        {"id": 1, "native_traversal_rank": 1, **raw1})
    ordered_anchor["arc_ids_by_key"].append({"key": key2, "id": 1})
    ordered_anchor["out_row_offsets"] = [0, 2, 2]
    ordered_anchor["out_arc_ids"] = [0, 1]
    ordered_anchor["in_row_offsets"] = [0, 0, 2]
    ordered_anchor["in_arc_ids"] = [0, 1]
    node_shadow = {record["id"]: copy.deepcopy(record)
                   for record in ordered_anchor["nodes"]}
    errors = []
    _generation_zero_anchor(node_shadow, {0: semantic0, 1: semantic1},
        {0: key, 1: key2}, [0, 1], [0, 2, 2], [0, 1], [0, 0, 2], {},
        ordered_anchor, "baseline-order-canary",
        lambda ok, message: errors.append(message) if not ok else None)
    require(not errors, f"valid generation-zero arc order was rejected: {errors}")
    reordered = copy.deepcopy(ordered_anchor)
    reordered["out_arc_ids"] = [1, 0]
    errors = []
    _generation_zero_anchor(node_shadow, {0: semantic0, 1: semantic1},
        {0: key, 1: key2}, [0, 1], [0, 2, 2], [0, 1], [0, 0, 2], {},
        reordered, "baseline-out-order-tamper",
        lambda ok, message: errors.append(message) if not ok else None)

    require(errors, "generation-zero BaseOut FIFO tamper was accepted")
