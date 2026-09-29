#!/usr/bin/env python3
"""Field-addressable validation for Stage 5 Dual-CSR transaction receipts."""

import json
from collections.abc import Callable
from typing import Any

from dual_csr_arc_group_validator import validate_arc_group_semantics
from dual_csr_descriptor_replay_validator import validate_descriptor_replay
from dual_csr_execution_shape_validator import (
    validate_execution_levels,
    validate_mixed_execution,
)
from dual_csr_reference_replay_validator import validate_reference_replay
from dual_csr_resolver_semantics_validator import validate_resolver_semantics
from dual_csr_structural_semantics_validator import validate_structural_semantics
from dual_csr_typed_json_validator import (
    validate_arc_payload,
    validate_descriptor_schema,
    validate_node_record,
    validate_reference_delta,
)

Require = Callable[[bool, str], None]
Difference = Callable[[object, object, str], str | None]


def _object(value: object, keys: set[str], label: str,
            require: Require) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(set(value) == keys, f"{label} fields differ: {sorted(value)}")
    return value


def _array(value: object, label: str, require: Require) -> list[Any]:
    require(isinstance(value, list), f"{label} must be an array")
    return value


def _count(value: object, label: str, require: Require) -> int:
    require(type(value) is int and value >= 0,
            f"{label} must be a nonnegative integer")
    return value


def _node_key(value: object, label: str, require: Require) -> None:
    item = _object(value, {"kind", "full_name", "instance_name", "pin_name"},
                   label, require)
    require(all(isinstance(item[key], str) for key in item),
            f"{label} contains a non-string semantic key field")


def _arc_key(value: object, label: str, require: Require) -> None:
    keys = {"source", "sink", "kind", "owner_name", "library_source_port",
            "library_sink_port", "library_arcs", "parallel_ordinal"}
    item = _object(value, keys, label, require)
    _node_key(item["source"], f"{label}.source", require)
    _node_key(item["sink"], f"{label}.sink", require)
    _count(item["parallel_ordinal"], f"{label}.parallel_ordinal", require)
    require(all(isinstance(item[key], str) for key in (
        "kind", "owner_name", "library_source_port", "library_sink_port")),
        f"{label} contains an invalid semantic arc field")
    _array(item["library_arcs"], f"{label}.library_arcs", require)


def _creation_token(value: object, label: str, require: Require) -> None:
    keys = {"schema_version", "case_id", "profile_id", "seed",
            "batch_ordinal", "accepted_ordinal"}
    item = _object(value, keys, label, require)
    require(isinstance(item["case_id"], str)
            and isinstance(item["profile_id"], str),
            f"{label} contains an invalid string field")
    for key in ("schema_version", "seed", "batch_ordinal", "accepted_ordinal"):
        _count(item[key], f"{label}.{key}", require)


def _stable_node(value: object, label: str, require: Require) -> None:
    require(isinstance(value, dict), f"{label} must be an object")
    if value.get("kind") == "existing":
        item = _object(value, {"kind", "baseline_key"}, label, require)
        _node_key(item["baseline_key"], f"{label}.baseline_key", require)
    else:
        item = _object(value, {"kind", "creation_token", "role"}, label, require)
        require(item["kind"] == "created" and item["role"] in {"input", "output"},
                f"{label} contains an invalid created-node reference")
        _creation_token(item["creation_token"], f"{label}.creation_token", require)


def _stable_net(value: object, label: str, require: Require) -> None:
    require(isinstance(value, dict), f"{label} must be an object")
    if value.get("kind") == "existing":
        item = _object(value, {"kind", "baseline_name"}, label, require)
        require(isinstance(item["baseline_name"], str),
                f"{label}.baseline_name must be a string")
    else:
        item = _object(value, {"kind", "creation_token", "role"}, label, require)
        require(item["kind"] == "created" and item["role"] == "output_net",
                f"{label} contains an invalid created-net reference")
        _creation_token(item["creation_token"], f"{label}.creation_token", require)


def _stable_arc(value: object, label: str, require: Require) -> None:
    require(isinstance(value, dict), f"{label} must be an object")
    kind = value.get("kind")
    if kind == "existing":
        item = _object(value, {"kind", "baseline_key"}, label, require)
        _arc_key(item["baseline_key"], f"{label}.baseline_key", require)
    elif kind == "created_cell":
        keys = {"kind", "creation_token", "library_source_port",
                "library_sink_port", "ordered_library_arcs",
                "stable_equivalence_ordinal"}
        item = _object(value, keys, label, require)
        _creation_token(item["creation_token"], f"{label}.creation_token", require)
        _array(item["ordered_library_arcs"],
               f"{label}.ordered_library_arcs", require)
        _count(item["stable_equivalence_ordinal"],
               f"{label}.stable_equivalence_ordinal", require)
    elif kind == "created_old_net":
        item = _object(value, {"kind", "net", "driver", "sink",
                               "stable_ordinal"}, label, require)
        _stable_net(item["net"], f"{label}.net", require)
        _stable_node(item["driver"], f"{label}.driver", require)
        _stable_node(item["sink"], f"{label}.sink", require)
        _count(item["stable_ordinal"], f"{label}.stable_ordinal", require)
    else:
        item = _object(value, {"kind", "creation_token", "driver", "sink",
                               "stable_ordinal"}, label, require)
        require(item["kind"] == "created_new_net",
                f"{label} contains an unknown stable arc kind")
        _creation_token(item["creation_token"], f"{label}.creation_token", require)
        _stable_node(item["driver"], f"{label}.driver", require)
        _stable_node(item["sink"], f"{label}.sink", require)
        _count(item["stable_ordinal"], f"{label}.stable_ordinal", require)


def _canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _validate_resolver(value: object, structural: dict[str, Any], label: str,
                       require: Require) -> None:
    keys = {"high_water", "node_bindings", "net_bindings", "arc_bindings",
            "materializations"}
    item = _object(value, keys, label, require)
    high = structural["high_water"]
    require(item["high_water"] == high, f"{label}.high_water differs")
    validators = ((_stable_node, "node", high["node_slots"]),
                  (_stable_net, "net", high["net_slots"]),
                  (_stable_arc, "arc", high["arc_slots"]))
    arc_refs: dict[int, object] = {}
    arc_tombstones: dict[int, bool] = {}
    for validator, name, extent in validators:
        entries = _array(item[f"{name}_bindings"],
                         f"{label}.{name}_bindings", require)
        require(len(entries) == extent,
                f"{label}.{name}_bindings does not cover high-water")
        slots: set[int] = set()
        for index, entry in enumerate(entries):
            entry_label = f"{label}.{name}_bindings[{index}]"
            record = _object(entry, {"ref", "binding"}, entry_label, require)
            validator(record["ref"], f"{entry_label}.ref", require)
            binding = _object(record["binding"],
                              {"slot", "native_name", "tombstone"},
                              f"{entry_label}.binding", require)
            slot = _count(binding["slot"], f"{entry_label}.binding.slot", require)
            require(slot < extent and slot not in slots,
                    f"{entry_label} has a duplicate/out-of-range slot")
            require(isinstance(binding["native_name"], str)
                    and type(binding["tombstone"]) is bool,
                    f"{entry_label} has invalid native binding fields")
            slots.add(slot)
            if name == "arc":
                arc_refs[slot] = record["ref"]
                arc_tombstones[slot] = binding["tombstone"]
        require(slots == set(range(extent)),
                f"{label}.{name}_bindings leaves a slot uncovered")
    index_slots = structural["arc_group_index"]["slots"]
    for slot in index_slots:
        arc_id = slot["arc_id"]
        require(arc_refs[arc_id] == slot["ref"],
                f"{label}.arc_bindings[{arc_id}] ref differs from the index")
        require(arc_tombstones[arc_id]
                == (slot["current_active_key"] is None),
                f"{label}.arc_bindings[{arc_id}] tombstone differs from the index")
    materializations = _array(item["materializations"],
                              f"{label}.materializations", require)
    seen: set[str] = set()
    for index, entry in enumerate(materializations):
        entry_label = f"{label}.materializations[{index}]"
        record = _object(entry, {"key", "value"}, entry_label, require)
        _creation_token(record["key"], f"{entry_label}.key", require)
        materialized = _object(
            record["value"], {"creation_token", "instance_name",
                              "output_net_name", "input_pin_name",
                              "output_pin_name"}, f"{entry_label}.value", require)
        require(materialized["creation_token"] == record["key"],
                f"{entry_label} materialization token differs")
        token = _canonical(record["key"])
        require(token not in seen, f"{label} repeats a materialization token")
        seen.add(token)


def validate_identity_sequences(nodes: list[object], active_arcs: list[object],
                                published_nodes: list[object],
                                published_arcs: list[object], label: str,
                                require: Require) -> None:
    """Validate semantic-key bijections without guessing C++ byte ordering."""
    for index, key in enumerate(nodes):
        _node_key(key, f"{label}.nodes[{index}]", require)
    for index, key in enumerate(active_arcs):
        _arc_key(key, f"{label}.active_arcs[{index}]", require)
    encoded_nodes = [_canonical(key) for key in nodes]
    encoded_arcs = [_canonical(key) for key in active_arcs]
    require(len(encoded_nodes) == len(set(encoded_nodes))
            and len(encoded_arcs) == len(set(encoded_arcs)),
            f"{label} contains duplicate semantic keys")
    require(sorted(encoded_nodes) == sorted(map(_canonical, published_nodes))
            and sorted(encoded_arcs) == sorted(map(_canonical, published_arcs)),
            f"{label} differs from the published functional graph")


def _validate_identity(value: object, generation: int,
                       structural: dict[str, Any], functional: dict[str, Any],
                       resolver: dict[str, Any], label: str,
                       require: Require) -> None:
    item = _object(value, {"generation", "current_fingerprints",
                           "canonical_pre_state"}, label, require)
    require(item["generation"] == generation, f"{label}.generation differs")
    fingerprints = _object(
        item["current_fingerprints"], {"schema_version", "topology",
        "clock_constraint", "library", "corner", "parasitic", "timing_input"},
        f"{label}.current_fingerprints", require)
    require(all(isinstance(value, str) for value in fingerprints.values()),
            f"{label}.current_fingerprints contains a non-string field")
    state = _object(item["canonical_pre_state"],
                    {"instances", "nets", "nodes", "active_arcs"},
                    f"{label}.canonical_pre_state", require)
    for field in ("instances", "nets", "nodes", "active_arcs"):
        values = _array(state[field], f"{label}.canonical_pre_state.{field}",
                        require)
        canonical = [_canonical(value) for value in values]
        require(len(canonical) == len(set(canonical)),
                f"{label}.canonical_pre_state.{field} contains duplicates")
    graph = functional["graph"]
    published_nodes = [record["key"] for record in graph["nodes"]]
    published_arcs = [record["key"] for record in graph["active_arcs"]]
    validate_identity_sequences(state["nodes"], state["active_arcs"],
                                published_nodes, published_arcs,
                                f"{label}.canonical_pre_state", require)
    reference_instances = functional["graph"]["reference_payload"]["instances"]
    published_instances = [record["instance_name"]
                           for record in reference_instances]
    published_nets = [record["net_name"] for record in graph["nodes"]
                      if record["net_name"]]
    require(all(isinstance(name, str) for name in state["instances"]
                + state["nets"]),
            f"{label}.canonical_pre_state contains a non-string name")
    require(set(state["instances"]) == set(published_instances),
            f"{label}.canonical_pre_state instances differ from the reference")
    resolved_nets = {record["binding"]["native_name"] for record in resolver["net_bindings"]}
    require(set(state["nets"]) == resolved_nets and set(published_nets) <= resolved_nets,
            f"{label}.canonical_pre_state nets differ from resolver or omit published node nets")
    high = structural["high_water"]
    require(len(state["nets"]) == high["net_slots"]
            and len(state["nodes"]) == high["node_slots"],
            f"{label}.canonical_pre_state high-water differs")
    index_keys = [slot["current_active_key"]
                  for slot in structural["arc_group_index"]["slots"]
                  if slot["current_active_key"] is not None]
    require(sorted(map(_canonical, state["active_arcs"]))
            == sorted(map(_canonical, index_keys)),
            f"{label}.canonical_pre_state active arcs differ from the index")


def _high_water(value: object, label: str, require: Require) -> dict[str, Any]:
    item = _object(value, {"node_slots", "net_slots", "arc_slots"},
                   label, require)
    for key in item:
        _count(item[key], f"{label}.{key}", require)
    return item


def _payload(value: object, label: str, require: Require) -> dict[str, Any]:
    validate_arc_payload(value, label, require)
    return value


def _validate_descriptor(value: object, index: int, session: int,
                         previous: dict[str, Any], label: str,
                         require: Require,
                         reference_chain: dict[str, object]) -> dict[str, Any]:
    keys = {"schema", "session_token", "expected_generation",
            "target_generation", "before", "after", "node_appends",
            "arc_appends", "node_payload_changes", "removes", "changes",
            "reverse_changes", "payload_changes", "logical_deactivations",
            "aux_rows", "appended_base_out_rows", "affected_base_out_rows",
            "affected_base_in_rows", "reference_payload",
            "arc_group_index_delta"}
    item = _object(value, keys, label, require)
    validate_descriptor_schema(item["schema"], f"{label}.schema", require)
    require(item["session_token"] == session
            and item["expected_generation"] == index
            and item["target_generation"] == index + 1,
            f"{label} session/generation differs")
    before = _high_water(item["before"], f"{label}.before", require)
    after = _high_water(item["after"], f"{label}.after", require)
    require(before == previous, f"{label}.before breaks the high-water chain")
    for key in before:
        require(after[key] >= before[key], f"{label}.{key} moved backwards")
    nodes = _array(item["node_appends"], f"{label}.node_appends", require)
    require(len(nodes) == after["node_slots"] - before["node_slots"],
            f"{label}.node_appends does not match high-water")
    node_ids: list[int] = []
    for position, value in enumerate(nodes):
        record = _object(value, {"id", "key", "payload"},
                         f"{label}.node_appends[{position}]", require)
        node_ids.append(_count(record["id"], f"{label}.node_appends[{position}].id", require))
        _node_key(record["key"], f"{label}.node_appends[{position}].key", require)
        validate_node_record(record["payload"],
                             f"{label}.node_appends[{position}].payload", require)
        payload = record["payload"]
        require(payload["id"] == record["id"] and payload["key"] == record["key"],
                f"{label}.node_appends[{position}] payload identity differs")
    require(node_ids == list(range(before["node_slots"], after["node_slots"])),
            f"{label}.node_appends IDs are not append-only")
    arcs = _array(item["arc_appends"], f"{label}.arc_appends", require)
    require(len(arcs) == after["arc_slots"] - before["arc_slots"],
            f"{label}.arc_appends does not match high-water")
    arc_ids: list[int] = []
    for position, value in enumerate(arcs):
        record = _object(value, {"id", "key", "source", "sink", "payload"},
                         f"{label}.arc_appends[{position}]", require)
        arc_ids.append(_count(record["id"], f"{label}.arc_appends[{position}].id", require))
        _arc_key(record["key"], f"{label}.arc_appends[{position}].key", require)
        payload = _payload(record["payload"],
                           f"{label}.arc_appends[{position}].payload", require)
        require(payload["key"] == record["key"]
                and payload["source"] == record["source"]
                and payload["sink"] == record["sink"],
                f"{label}.arc_appends[{position}] payload identity differs")
    require(arc_ids == list(range(before["arc_slots"], after["arc_slots"])),
            f"{label}.arc_appends IDs are not append-only")
    record_keys = {
        "node_payload_changes": {"id", "expected_key", "expected_net_name",
                                 "replacement_net_name"},
        "removes": {"source", "physical_slot", "expected_arc"},
        "changes": {"source", "physical_slot", "expected_arc",
                    "replacement_arc"},
        "reverse_changes": {"sink", "physical_slot", "expected_arc",
                            "replacement_arc"},
        "payload_changes": {"id", "expected", "replacement"},
        "logical_deactivations": {"id", "key", "source", "sink", "reason",
                                  "expected_active"},
        "aux_rows": {"source", "expected_arc_ids", "replacement_arc_ids"},
        "appended_base_out_rows": {"source", "arc_ids"},
        "affected_base_out_rows": {"source", "expected_arc_ids",
                                   "replacement_arc_ids"},
        "affected_base_in_rows": {"sink", "expected_arc_ids",
                                  "replacement_arc_ids"},
        "arc_group_index_delta": {"id", "ref", "expected_active_key",
                                  "replacement_active_key"},
    }
    row_owner = {"appended_base_out_rows": "source",
                 "affected_base_out_rows": "source",
                 "affected_base_in_rows": "sink"}
    for field, expected_keys in record_keys.items():
        records = _array(item[field], f"{label}.{field}", require)
        owners: list[int] = []
        for position, record in enumerate(records):
            entry = _object(record, expected_keys,
                            f"{label}.{field}[{position}]", require)
            if field == "payload_changes":
                _payload(entry["expected"], f"{label}.{field}[{position}].expected", require)
                _payload(entry["replacement"], f"{label}.{field}[{position}].replacement", require)
            elif field == "arc_group_index_delta":
                _stable_arc(entry["ref"], f"{label}.{field}[{position}].ref", require)
                for key in ("expected_active_key", "replacement_active_key"):
                    if entry[key] is not None:
                        _arc_key(entry[key], f"{label}.{field}[{position}].{key}", require)
            elif field in row_owner:
                owner = row_owner[field]
                owners.append(_count(entry[owner],
                                     f"{label}.{field}[{position}].{owner}", require))
                sequences = ("arc_ids",) if field == "appended_base_out_rows" \
                    else ("expected_arc_ids", "replacement_arc_ids")
                for sequence in sequences:
                    values = _array(entry[sequence],
                                    f"{label}.{field}[{position}].{sequence}", require)
                    for value_index, arc_id in enumerate(values):
                        _count(arc_id, f"{label}.{field}[{position}].{sequence}"
                                      f"[{value_index}]", require)
        if field in row_owner:
            require(owners == sorted(set(owners)),
                    f"{label}.{field} owners are not unique and ordered")
    validate_reference_delta(item["reference_payload"],
                             f"{label}.reference_payload", require, reference_chain)
    return after


def validate_traversal_counts(value: object, structural: dict[str, Any],
                              latest_mode: str, label: str,
                              require: Require) -> None:
    require(latest_mode in {"full_stage5", "mixed_stage6"},
            f"{label} execution mode is invalid")
    stages = _object(value, {"levelization", "arc_initialization",
                             "forward_propagation", "required_propagation"},
                     label, require)
    for stage, visits in stages.items():
        entry = _object(visits, {"base_out", "aux_out", "base_in"},
                        f"{label}.{stage}", require)
        for key in entry:
            _count(entry[key], f"{label}.{stage}.{key}", require)
    base_active = sum(slot is not None
                      for slot in structural["base_out"]["physical_slots"])
    aux_active = sum(map(len, structural["aux_out"]["row_arcs"]))
    active = base_active + aux_active
    full_levelization = {"base_out": base_active, "aux_out": aux_active,
                         "base_in": active}
    mixed_levelization = {"base_out": 0, "aux_out": 0, "base_in": 0}
    expected = {
        "levelization": (mixed_levelization
                         if latest_mode == "mixed_stage6"
                         else full_levelization),
        "arc_initialization": {"base_out": base_active, "aux_out": aux_active,
                               "base_in": 0},
        "forward_propagation": {"base_out": 0, "aux_out": 0,
                                "base_in": 2 * active},
        "required_propagation": {"base_out": base_active,
                                 "aux_out": aux_active, "base_in": 0},
    }
    require(stages == expected, f"{label} does not match structural visits")


def normalize_plan(value: dict[str, Any]) -> dict[str, Any]:
    # Comparison consumers only read nested model records; copy changed containers.
    plan = dict(value)
    plan["baseline"] = dict(value["baseline"])
    state = dict(value["baseline"]["canonical_pre_state"])
    plan["baseline"]["canonical_pre_state"] = state
    plan["edits"] = [dict(edit) for edit in value["edits"]]
    state["active_arcs"] = state.pop("arcs")
    for edit in plan["edits"]:
        rc_inputs = edit["rc_inputs"]
        if rc_inputs["kind"] == "no_parasitic":
            edit["rc_inputs"] = {"kind": "none"}
        elif "parasitic_policy" in rc_inputs:
            edit["rc_inputs"] = {"kind": "parasitic",
                                 "policy": rc_inputs["parasitic_policy"]}
    return plan


def _validate_audit(value: object, descriptor: dict[str, Any], label: str,
                    require: Require) -> None:
    keys = {"requested_nodes", "node_reads", "accepted_nodes",
        "requested_instances", "instance_reads", "accepted_instances",
        "requested_arcs", "native_arc_candidate_reads", "accepted_arcs",
        "requested_deactivations", "deactivated_arc_before_reads",
        "accepted_deactivations", "captured_arc_records", "requested_load_nets",
        "net_reads", "accepted_load_nets", "requested_library_cells",
        "library_cell_reads", "accepted_library_cells",
        "requested_cell_pin_capacitances", "cell_pin_capacitance_reads",
        "accepted_cell_pin_capacitances", "requested_node_roles",
        "node_role_reads", "accepted_node_roles", "index_group_lookups",
        "native_arc_candidate_upper_bound", "arc_identity_tie_breaks",
        "rejected_reads", "full_export_guard_active", "full_export_observed",
        "native_full_graph_exports", "native_full_reference_exports"}
    item = _object(value, keys, label, require)
    counts = {key: _count(item[key], f"{label}.{key}", require)
              for key in keys if key not in {"full_export_guard_active",
                                             "full_export_observed"}}
    require(item["full_export_guard_active"] is True
            and item["full_export_observed"] is False,
            f"{label} full-export guard evidence differs")
    for field in ("native_full_graph_exports", "native_full_reference_exports",
                  "rejected_reads"):
        require(counts[field] == 0, f"{label}.{field} must be zero")
    require(counts["captured_arc_records"]
            == counts["accepted_arcs"] + counts["accepted_deactivations"],
            f"{label} captured arc count differs")
    require(counts["requested_nodes"] == counts["accepted_nodes"]
            <= counts["node_reads"] <= descriptor["after"]["node_slots"],
            f"{label} node reads are not bounded")
    for requested, reads, accepted in (
        ("requested_instances", "instance_reads", "accepted_instances"),
        ("requested_load_nets", "net_reads", "accepted_load_nets"),
        ("requested_library_cells", "library_cell_reads", "accepted_library_cells"),
        ("requested_cell_pin_capacitances", "cell_pin_capacitance_reads",
         "accepted_cell_pin_capacitances"),
        ("requested_node_roles", "node_role_reads", "accepted_node_roles")):
        require(counts[requested] == counts[reads] == counts[accepted],
                f"{label}.{requested} capture is not exact")
    require(counts["requested_arcs"] == counts["accepted_arcs"]
            <= counts["native_arc_candidate_reads"]
            <= counts["native_arc_candidate_upper_bound"]
            <= 2 * descriptor["after"]["arc_slots"],
            f"{label} arc candidate reads are not bounded")
    require(counts["requested_deactivations"]
            == counts["deactivated_arc_before_reads"]
            == counts["accepted_deactivations"]
            == len(descriptor["logical_deactivations"]),
            f"{label} deactivation capture is not exact")
    requests = counts["requested_arcs"] + counts["requested_deactivations"]
    if requests == 0:
        require(counts["index_group_lookups"] == 0
                and counts["native_arc_candidate_reads"] == 0
                and counts["native_arc_candidate_upper_bound"] == 0,
                f"{label} empty group lookup audit differs")
    else:
        require(0 < counts["index_group_lookups"] <= requests,
                f"{label} group lookup count is not bounded")
    require(counts["arc_identity_tie_breaks"] <= counts["accepted_arcs"],
            f"{label} arc tie-break count differs")


def _validate_execution_trace(value: object, generation: int,
                              descriptor_node_bounds: list[tuple[int, int, int]],
                              final_node_count: int,
                              final_traversal: dict[str, Any], label: str,
                              require: Require) -> list[str]:
    records = _array(value, label, require)
    require(len(records) == generation,
            f"{label} must contain one record per generation")
    require(len(descriptor_node_bounds) == generation,
            f"{label} descriptor node-count history differs")
    modes: list[str] = []
    keys = {"generation", "mode", "levels", "traversal",
            "full_levelizer_entry_delta", "fallback_count",
            "supplied_levels_validated", "mixed"}
    for index, raw in enumerate(records):
        item = _object(raw, keys, f"{label}[{index}]", require)
        require(item["generation"] == index + 1,
                f"{label}[{index}].generation differs")
        mode = item["mode"]
        require(mode in {"full_stage5", "mixed_stage6"},
                f"{label}[{index}].mode differs")
        modes.append(mode)
        node_count = validate_execution_levels(
            item["levels"], f"{label}[{index}].levels", require)
        last_nodes, current_nodes, new_nodes = descriptor_node_bounds[index]
        require(node_count == current_nodes,
                f"{label}[{index}] node extent differs from descriptor.after")
        level_count = len(item["levels"]["level_offsets"]) - 1
        _count(item["full_levelizer_entry_delta"],
               f"{label}[{index}].full_levelizer_entry_delta", require)
        _count(item["fallback_count"],
               f"{label}[{index}].fallback_count", require)
        traversal = _object(item["traversal"], {
            "levelization", "arc_initialization", "forward_propagation",
            "required_propagation"}, f"{label}[{index}].traversal", require)
        for stage, visits in traversal.items():
            entry = _object(visits, {"base_out", "aux_out", "base_in"},
                            f"{label}[{index}].traversal.{stage}", require)
            for key in entry:
                _count(entry[key],
                       f"{label}[{index}].traversal.{stage}.{key}", require)
        if mode == "full_stage5":
            require(item["full_levelizer_entry_delta"] == 1
                    and item["fallback_count"] == 0
                    and item["supplied_levels_validated"] is False
                    and item["mixed"] is None,
                    f"{label}[{index}] full execution evidence differs")
        else:
            zero_visits = {"base_out": 0, "aux_out": 0, "base_in": 0}
            require(item["full_levelizer_entry_delta"] == 0
                    and item["fallback_count"] == 0
                    and item["supplied_levels_validated"] is True
                    and traversal["levelization"] == zero_visits,
                    f"{label}[{index}] mixed execution evidence differs")
            validate_mixed_execution(
                item["mixed"], last_nodes, node_count, new_nodes,
                level_count, f"{label}[{index}].mixed", require)
    if records:
        require(len(records[-1]["levels"]["level_nodes"]) == final_node_count,
                f"{label} final node extent differs")
        require(records[-1]["traversal"] == final_traversal,
                f"{label} final traversal differs")
    return modes


def validate_transaction(value: object, generation: int,
                         structural: dict[str, Any],
                         functional: dict[str, Any],
                         expected_batches: list[dict[str, Any]], label: str,
                         require: Require, difference: Difference) -> None:
    keys = {"session_token", "generation", "resolver", "arc_group_index",
            "identity", "descriptor_trace", "semantic_trace",
            "traversal_telemetry", "execution_trace", "update_telemetry", "evidence",
            "baseline_reference_input"}
    item = _object(value, keys, label, require)
    session = _count(item["session_token"], f"{label}.session_token", require)
    require(session > 0 and generation > 0 and item["generation"] == generation,
            f"{label} session/generation differs")
    require(item["arc_group_index"] == structural["arc_group_index"],
            f"{label}.arc_group_index differs from published structure")
    validate_arc_group_semantics(item["arc_group_index"], functional["graph"],
                                 f"{label}.arc_group_index", require)
    require(item["update_telemetry"] == structural["update_telemetry"],
            f"{label}.update_telemetry differs from published structure")
    require(item["traversal_telemetry"] == item["update_telemetry"]["traversal"],
            f"{label}.traversal_telemetry differs from updater evidence")
    _validate_resolver(item["resolver"], structural, f"{label}.resolver", require)
    _validate_identity(item["identity"], generation, structural, functional,
                       item["resolver"], f"{label}.identity", require)
    descriptors = _array(item["descriptor_trace"],
                         f"{label}.descriptor_trace", require)
    require(len(descriptors) == generation, f"{label} descriptor count differs")
    previous = expected_batches[0]["baseline"]["canonical_pre_state"]
    previous = {"node_slots": len(previous["nodes"]),
                "net_slots": len(previous["nets"]),
                "arc_slots": len(previous["arcs"])}
    reference_chain: dict[str, object] = {}
    for index, descriptor in enumerate(descriptors):
        previous = _validate_descriptor(
            descriptor, index, session, previous,
            f"{label}.descriptor_trace[{index}]", require, reference_chain)
    descriptor_node_bounds = [
        (descriptor["before"]["node_slots"],
         descriptor["after"]["node_slots"],
         len(descriptor["node_appends"]))
        for descriptor in descriptors
    ]
    execution_modes = _validate_execution_trace(
        item["execution_trace"], generation, descriptor_node_bounds,
        structural["high_water"]["node_slots"], item["traversal_telemetry"],
        f"{label}.execution_trace", require)
    validate_traversal_counts(item["traversal_telemetry"], structural,
                              execution_modes[-1],
                              f"{label}.traversal_telemetry", require)
    validate_descriptor_replay(
        descriptors, structural, functional, item["resolver"],
        item["baseline_reference_input"], expected_batches,
        f"{label}.descriptor_replay", require)
    validate_reference_replay(
        descriptors, functional, item["identity"], item["resolver"], session,
        item["baseline_reference_input"], generation, expected_batches,
        f"{label}.reference_replay", require)
    validate_structural_semantics(structural, item["resolver"], descriptors,
                                  f"{label}.structural_semantics", require)
    require(previous == structural["high_water"],
            f"{label} final descriptor high-water differs")
    semantic = _array(item["semantic_trace"], f"{label}.semantic_trace", require)
    expected = [normalize_plan(batch) for batch in expected_batches]
    mismatch = difference(semantic, expected, f"{label}.semantic_trace")
    require(mismatch is None, f"{label} normalized semantic trace differs: {mismatch}")
    validate_resolver_semantics(item["resolver"], functional, structural,
        item["baseline_reference_input"], semantic,
        f"{label}.resolver_semantics", require)
    evidence = _object(item["evidence"], {"phase_trace", "capture_audits",
        "operation_counters", "incremental_batch_executed", "propagation_scope"},
        f"{label}.evidence", require)
    expected_phases: list[str] = []
    for mode in execution_modes:
        expected_phases.extend([
            "pre_candidate_prepare", "candidate_apply_execute",
            "post_candidate_capture", "dual_csr_update",
            "mixed_levelization" if mode == "mixed_stage6" else "full_levelization",
            "full_propagation", "intrinsic_verify", "publish"])
    require(evidence["phase_trace"] == expected_phases, f"{label}.evidence phase trace differs")
    audits = _array(evidence["capture_audits"],
                    f"{label}.evidence.capture_audits", require)
    require(len(audits) == generation, f"{label} audit count differs")
    for index, audit in enumerate(audits):
        _validate_audit(audit, descriptors[index],
                        f"{label}.evidence.capture_audits[{index}]", require)
    counter_keys = {"native_full_graph_exports", "native_full_reference_exports",
        "host_snapshot_rebuilds", "baseline_factory_calls", "fallback_calls"}
    counters = _object(evidence["operation_counters"], counter_keys,
                       f"{label}.evidence.operation_counters", require)
    for key in counter_keys:
        _count(counters[key], f"{label}.evidence.operation_counters.{key}", require)
    for key in ("native_full_graph_exports", "native_full_reference_exports",
                "host_snapshot_rebuilds", "baseline_factory_calls", "fallback_calls"):
        require(counters[key] == 0, f"{label}.evidence.{key} must be zero")
    require(evidence["incremental_batch_executed"] is True
            and evidence["propagation_scope"] == "full_graph",
            f"{label}.evidence completion fields differ")
