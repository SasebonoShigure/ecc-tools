#!/usr/bin/env python3
"""Field-level functional checks for Stage 4 production buffer replay receipts."""

import json
import math
from typing import Any


class FunctionalEvidenceError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:  # noqa: FBT001 - assertion predicate
    if not condition:
        raise FunctionalEvidenceError(message)


def require_equal(left: object, right: object, label: str) -> None:
    require(left == right, f"{label} differs")


def canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def workload_kind(trace: dict[str, Any]) -> str:
    return trace["replay"]["header"]["semantic_workload_kind"]


def validate_derived_freshness(value: object, kind: str, label: str) -> None:
    require(isinstance(value, dict), f"{label} must be an object")
    expected = {
        "exercised",
        "timing_point",
        "baseline_arrival",
        "tampered_arrival",
        "recovered_arrival",
        "csr_node_count",
        "csr_arc_count",
        "tamper_observed",
        "rerun_input_exact",
        "rerun_cpu_exact",
        "rerun_identity_exact",
        "rollback_owner_restored",
        "rollback_input_exact",
        "rollback_cpu_exact",
        "rollback_identity_exact",
        "session_remained_baseline_executed",
        "authority_remained_active",
    }
    require(set(value) == expected, f"{label} fields differ")
    if kind == "synthetic":
        require(value["exercised"] is False, f"{label} unexpectedly exercised")
        return
    require(kind == "real_design", f"unknown workload kind {kind}")
    require(value["exercised"] is True, f"{label} was not exercised")
    require(isinstance(value["timing_point"], str) and value["timing_point"],
            f"{label} has no timing point")
    for field in ("baseline_arrival", "tampered_arrival", "recovered_arrival"):
        arrival = value[field]
        require(isinstance(arrival, (int, float)) and not isinstance(arrival, bool)
                and math.isfinite(float(arrival)),
                f"{label}.{field} is not a finite number")
    require(value["csr_node_count"] > 0 and value["csr_arc_count"] > 0,
            f"{label} has an empty CSR projection")
    for field in expected - {
        "exercised", "timing_point", "baseline_arrival", "tampered_arrival",
        "recovered_arrival", "csr_node_count", "csr_arc_count",
    }:
        require(value[field] is True, f"{label}.{field} is not true")
    require_equal(value["baseline_arrival"], value["recovered_arrival"],
                  f"{label} recovered arrival")
    require(value["tampered_arrival"] != value["baseline_arrival"],
            f"{label} tamper did not change arrival")


def native_name(ref: dict[str, Any], resolver: dict[str, Any]) -> str:
    if ref.get("kind") == "existing":
        key = ref.get("baseline_key")
        require(isinstance(key, dict), "existing node ref lacks baseline key")
        name = key.get("full_name")
        require(isinstance(name, str) and name, "existing node ref lacks name")
        return name
    for entry in resolver["node_bindings"]:
        if entry["ref"] == ref:
            require(entry["binding"]["tombstone"] is False,
                    "candidate node binding is tombstoned")
            return entry["binding"]["native_name"]
    raise FunctionalEvidenceError("created node ref is absent from resolver")


def net_name(ref: dict[str, Any]) -> str:
    if ref.get("kind") == "existing":
        return ref["baseline_name"]
    token = ref["creation_token"]
    require(ref.get("role") == "output_net", "created net has the wrong role")
    return f"__ista_net_b{token['batch_ordinal']}_e{token['accepted_ordinal']}"


def finite_value(vector: dict[str, Any], index: int, label: str) -> float:
    require(vector["presence"][index] == 1, f"{label} is absent")
    value = vector["values"][index]
    require(value["kind"] == "finite", f"{label} is not finite")
    return value["value"]


def validate_identity_source(identity: dict[str, Any],
                             csr_input: dict[str, Any], label: str) -> None:
    fingerprints = identity["current_fingerprints"]
    source = csr_input["source_identity"]
    mapping = {
        "schema_version": "fingerprint_schema_version",
        "topology": "topology_fingerprint",
        "library": "library_fingerprint",
        "clock_constraint": "clock_constraint_fingerprint",
        "parasitic": "parasitic_fingerprint",
        "corner": "corner_fingerprint",
        "timing_input": "timing_input_fingerprint",
    }
    for identity_field, source_field in mapping.items():
        require_equal(fingerprints[identity_field], source[source_field],
                      f"{label} {identity_field} fingerprint")


def validate_database_csr(database: dict[str, Any], csr_input: dict[str, Any],
                          label: str) -> None:
    pin_names = {pin["full_name"] for pin in database["pins"]}
    node_names = {node["key"]["full_name"] for node in csr_input["nodes"]}
    require_equal(pin_names, node_names, f"{label} native/CSR node closure")
    native_net_arcs = {
        (net["name"], driver, load)
        for net in database["nets"]
        for driver in net["ordered_drivers"]
        for load in net["ordered_loads"]
    }
    csr_net_arcs = {
        (arc["key"]["owner_name"], arc["key"]["source"]["full_name"],
         arc["key"]["sink"]["full_name"])
        for arc in csr_input["arcs"] if arc["kind"] == "net"
    }
    require_equal(native_net_arcs, csr_net_arcs,
                  f"{label} native/CSR net driver-load closure")


def validate_csr(input_value: dict[str, Any], timing: dict[str, Any], label: str) -> None:
    nodes = input_value["nodes"]
    arcs = input_value["arcs"]
    require(input_value["unsupported_reasons"] == [],
            f"{label} has unsupported CSR reasons")
    require([node["id"] for node in nodes] == list(range(len(nodes))),
            f"{label} node ids are not dense")
    require([arc["id"] for arc in arcs] == list(range(len(arcs))),
            f"{label} arc ids are not dense")
    require(
        {canonical({"id": node["id"], "key": node["key"]}) for node in nodes}
        == {canonical(item) for item in input_value["node_ids_by_key"]},
        f"{label} node key map is not closed",
    )
    require(
        {canonical({"id": arc["id"], "key": arc["key"]}) for arc in arcs}
        == {canonical(item) for item in input_value["arc_ids_by_key"]},
        f"{label} arc key map is not closed",
    )
    for direction in ("in", "out"):
        offsets = input_value[f"{direction}_row_offsets"]
        ids = input_value[f"{direction}_arc_ids"]
        require(len(offsets) == len(nodes) + 1 and offsets[0] == 0,
                f"{label} {direction} row offsets are malformed")
        require(offsets[-1] == len(ids) == len(arcs),
                f"{label} {direction} adjacency is not closed")
        require(sorted(ids) == list(range(len(arcs))),
                f"{label} {direction} adjacency does not cover each arc")
    require_equal(input_value["source_identity"], timing["source_identity"],
                  f"{label} source identity")
    require(len(timing["node_to_level"]) == len(nodes),
            f"{label} node levels have the wrong size")
    require(timing["level_offsets"][-1] == len(timing["level_nodes"]),
            f"{label} level rows are not closed")
    for field, vector in timing["nodes"].items():
        require(len(vector["presence"]) == len(nodes),
                f"{label} node {field} presence has the wrong size")
        require(len(vector["values"]) == len(nodes),
                f"{label} node {field} values have the wrong size")
    require(len(timing["data_slews"]["presence"])
            == len(timing["data_slews"]["values"]),
            f"{label} slew vectors differ in size")


def operation_tokens(plan: dict[str, Any], kind: str) -> list[dict[str, Any]]:
    return [op["creation_token"] for op in plan["operations"]
            if op["kind"] == kind]


def operation_count(plan: dict[str, Any], kind: str) -> int:
    return sum(op["kind"] == kind for op in plan["operations"])


def operation_payloads(plan: dict[str, Any], kind: str, field: str) -> list[str]:
    return sorted(canonical(op[field]) for op in plan["operations"]
                  if op["kind"] == kind)


def database_node_name(ref: dict[str, Any], pins: dict[str, dict[str, Any]]) -> str:
    if ref["kind"] == "existing":
        return ref["baseline_key"]["full_name"]
    token = ref["creation_token"]
    instance = f"__ista_buf_b{token['batch_ordinal']}_e{token['accepted_ordinal']}"
    direction = 1 if ref["role"] == "input" else 2
    matches = [pin["full_name"] for pin in pins.values()
               if pin["instance_name"] == instance and pin["direction"] == direction]
    require(len(matches) == 1, "created node does not resolve to one native pin")
    return matches[0]


def validate_batch(batch: dict[str, Any], trace_batch: dict[str, Any],
                   batch_index: int) -> None:
    label = f"functional batch {batch_index}"
    require_equal(batch["normalized_edits"], trace_batch["edits"],
                  f"{label} normalized edits")
    require(batch["batch_ordinal"] == batch_index,
            f"{label} ordinal differs")
    require(batch["expected_generation"] == batch_index,
            f"{label} expected generation differs")
    require(batch["target_generation"] == batch_index + 1,
            f"{label} target generation differs")
    require(batch["post_identity"]["generation"] == batch_index + 1,
            f"{label} post identity generation differs")
    edits = batch["normalized_edits"]
    materializations = batch["materializations"]
    require(len(materializations) == len(edits),
            f"{label} materialization count differs")
    edit_tokens = [edit["creation_token"] for edit in edits]
    require_equal([item["creation_token"] for item in materializations],
                  edit_tokens, f"{label} materialization tokens")
    require_equal(operation_tokens(batch["forward"], "add_instance"),
                  edit_tokens, f"{label} forward instance tokens")
    require_equal(operation_tokens(batch["reverse"], "remove_instance"),
                  edit_tokens, f"{label} reverse instance tokens")
    moved_count = sum(len(edit["moved_sinks"]) for edit in edits)
    require(operation_count(batch["forward"], "remove_net_arc") == moved_count,
            f"{label} forward old-arc removal count differs")
    require(operation_count(batch["forward"], "reconnect_sink") == moved_count,
            f"{label} forward sink reconnect count differs")
    require(operation_count(batch["reverse"], "add_net_arc") == moved_count,
            f"{label} reverse old-arc restoration count differs")
    require(operation_count(batch["reverse"], "reconnect_sink") == moved_count,
            f"{label} reverse sink reconnect count differs")
    require_equal(
        operation_payloads(batch["forward"], "add_timing_point", "node"),
        operation_payloads(batch["reverse"], "remove_timing_point", "node"),
        f"{label} timing-point inverse set",
    )
    require_equal(
        operation_payloads(batch["forward"], "add_cell_arc", "arc"),
        operation_payloads(batch["reverse"], "remove_cell_arc", "arc"),
        f"{label} cell-arc inverse set",
    )
    require_equal(
        operation_payloads(batch["forward"], "add_net_arc", "arc"),
        operation_payloads(batch["reverse"], "remove_net_arc", "arc"),
        f"{label} new-net arc inverse set",
    )
    require_equal(
        operation_payloads(batch["forward"], "remove_net_arc", "arc"),
        operation_payloads(batch["reverse"], "add_net_arc", "arc"),
        f"{label} original-net arc inverse set",
    )
    forward_reconnects = sorted(canonical({
        "sink": op["sink"], "old_net": op["new_net"],
        "new_net": op["old_net"]}) for op in batch["forward"]["operations"]
        if op["kind"] == "reconnect_sink")
    reverse_reconnects = sorted(canonical({
        "sink": op["sink"], "old_net": op["old_net"],
        "new_net": op["new_net"]}) for op in batch["reverse"]["operations"]
        if op["kind"] == "reconnect_sink")
    require_equal(forward_reconnects, reverse_reconnects,
                  f"{label} reconnect inverse set")

    instances = {item["name"]: item for item in batch["native_database"]["instances"]}
    nets = {item["name"]: item for item in batch["native_database"]["nets"]}
    pins = {item["full_name"]: item for item in batch["native_database"]["pins"]}
    for edit, item in zip(edits, materializations, strict=True):
        token = edit["creation_token"]
        instance_name = f"__ista_buf_b{token['batch_ordinal']}_e{token['accepted_ordinal']}"
        output_net_name = f"__ista_net_b{token['batch_ordinal']}_e{token['accepted_ordinal']}"
        require(item["instance_name"] == instance_name,
                f"{label} instance materialization name differs")
        require(item["output_net_name"] == output_net_name,
                f"{label} net materialization name differs")
        require(instances[instance_name]["cell"] == edit["library_cell"]["cell_name"],
                f"{label} materialized cell differs")
        require(pins[item["input_pin_name"]]["net_name"] == net_name(edit["original_net"]),
                f"{label} buffer input net differs")
        require(pins[item["output_pin_name"]]["net_name"] == output_net_name,
                f"{label} buffer output net differs")
        moved_names = [database_node_name(ref, pins) for ref in edit["moved_sinks"]]
        require(set(moved_names).issubset(set(nets[output_net_name]["ordered_loads"])),
                f"{label} moved sink is absent from output net")
        for moved_name in moved_names:
            require(pins[moved_name]["net_name"] == output_net_name,
                    f"{label} moved sink pin was not reconnected")
    validate_csr(batch["csr_input"], batch["csr_timing"], label)
    validate_identity_source(batch["post_identity"], batch["csr_input"], label)
    validate_database_csr(batch["native_database"], batch["csr_input"], label)


def baseline_identity(trace: dict[str, Any]) -> dict[str, Any]:
    workload = trace["replay"]["workload"]
    if workload["kind"] == "real_design":
        return workload["baseline"]
    return workload["batches"][0]["baseline"]


def validate_prefix(trace: dict[str, Any], functional: dict[str, Any],
                    prefix: int) -> None:
    replay = trace["replay"]
    trace_batches = replay["workload"]["batches"]
    require_equal(functional["header"], replay["header"],
                  f"prefix {prefix} replay header")
    require(functional["prefix_batches"] == prefix,
            f"prefix {prefix} receipt prefix differs")
    require(len(functional["batches"]) == prefix,
            f"prefix {prefix} receipt batch count differs")
    require(functional["committed_identity"]["generation"] == prefix,
            f"prefix {prefix} committed generation differs")
    for index, batch in enumerate(functional["batches"]):
        validate_batch(batch, trace_batches[index], index)
    validate_csr(functional["final_csr_input"], functional["final_csr_timing"],
                 f"prefix {prefix} final CSR")
    validate_identity_source(functional["committed_identity"],
                             functional["final_csr_input"], f"prefix {prefix}")
    validate_database_csr(functional["final_database"],
                          functional["final_csr_input"], f"prefix {prefix}")
    if prefix == 0:
        baseline = baseline_identity(trace)
        identity = functional["committed_identity"]
        require_equal(identity["canonical_pre_state"], baseline["canonical_pre_state"],
                      "generation-zero canonical state")
        require_equal(identity["current_fingerprints"], baseline["fingerprints"],
                      "generation-zero fingerprints")
        pre_state = baseline["canonical_pre_state"]
        require_equal(sorted(pre_state["instances"]),
                      sorted(item["name"] for item in functional["final_database"]["instances"]),
                      "generation-zero instance names")
        require_equal(sorted(pre_state["nets"]),
                      sorted(item["name"] for item in functional["final_database"]["nets"]),
                      "generation-zero net names")
        require_equal({canonical(item) for item in pre_state["nodes"]},
                      {canonical(item["key"]) for item in functional["final_csr_input"]["nodes"]},
                      "generation-zero CSR nodes")
        require_equal({canonical(item) for item in pre_state["arcs"]},
                      {canonical(item["key"]) for item in functional["final_csr_input"]["arcs"]},
                      "generation-zero CSR arcs")
        require(not any(item["name"].startswith("__ista_buf_")
                        for item in functional["final_database"]["instances"]),
                "generation-zero database contains a materialized buffer")
    else:
        final_batch = functional["batches"][-1]
        require_equal(final_batch["native_database"], functional["final_database"],
                      "active final database")
        require_equal(final_batch["csr_input"], functional["final_csr_input"],
                      "active final CSR input")
        require_equal(final_batch["csr_timing"], functional["final_csr_timing"],
                      "active final CSR timing")
        require_equal(final_batch["post_identity"], functional["committed_identity"],
                      "active final committed identity")


def numeric_values(value: object) -> set[float]:
    result: set[float] = set()
    if isinstance(value, dict):
        if value.get("kind") == "finite" and isinstance(value.get("value"), (int, float)):
            result.add(float(value["value"]))
        else:
            for child in value.values():
                result.update(numeric_values(child))
    elif isinstance(value, list):
        for child in value:
            result.update(numeric_values(child))
    return result


def validate_generation_timing(trace: dict[str, Any],
                               functionals: list[dict[str, Any]]) -> None:
    rounds = trace["generation_only"]["rounds"]
    batches = trace["replay"]["workload"]["batches"]
    extra_empty_round = len(rounds) == len(batches) + 1
    require(len(rounds) == len(batches) or extra_empty_round,
            "generation rounds do not match replay batches")
    if extra_empty_round:
        require(trace["generation_only"]["stop_reason"] == "empty_accepted_batch",
                "extra generation round lacks empty stop reason")
        require(rounds[-1]["accepted_tokens"] == [],
                "extra generation round accepted edits")
    for index, round_value in enumerate(rounds):
        if index < len(batches):
            edit_tokens = [edit["creation_token"] for edit in batches[index]["edits"]]
            require_equal(round_value["accepted_tokens"], edit_tokens,
                          f"generation round {index} accepted tokens")
            accepted = [item["accepted_creation_token"]
                        for item in round_value["evaluations"]
                        if item["decision"] == "accepted"]
            require({canonical(item) for item in accepted}
                    == {canonical(item) for item in edit_tokens},
                    f"generation round {index} accepted evaluations differ")
        before = functionals[index]
        nodes = before["final_csr_input"]["nodes"]
        timing_nodes = before["final_csr_timing"]["nodes"]
        by_name = {node["key"]["full_name"]: node["id"] for node in nodes}
        for evaluation in round_value["evaluations"]:
            numeric = evaluation.get("numeric")
            if numeric is None:
                continue
            name = native_name(evaluation["candidate_pin"], before["resolver_state"])
            require(name in by_name,
                    f"generation round {index} candidate is absent from pre-state CSR")
            node_id = by_name[name]
            require_equal(numeric["arrival_ns"],
                          finite_value(timing_nodes["arrivals"], node_id,
                                       f"round {index} candidate arrival"),
                          f"generation round {index} candidate arrival")
            require_equal(numeric["required_ns"],
                          finite_value(timing_nodes["requireds"], node_id,
                                       f"round {index} candidate required"),
                          f"generation round {index} candidate required")
            require_equal(numeric["slack_ns"],
                          finite_value(timing_nodes["slacks"], node_id,
                                       f"round {index} candidate slack"),
                          f"generation round {index} candidate slack")


def validate_real_same_net_high_fanout(
    functionals: list[dict[str, Any]],
) -> int:
    require(functionals, "real functional prefixes are empty")
    database = functionals[0]["final_database"]
    matching_nets = [net for net in database["nets"]
                     if net["name"] == "data_in"]
    require(len(matching_nets) == 1,
            "real baseline lacks exactly one data_in net")
    loads = matching_nets[0]["ordered_loads"]
    require(len(loads) == len(set(loads)),
            "real baseline data_in loads are not distinct")
    pins = {pin["full_name"]: pin for pin in database["pins"]}
    instances = {item["name"]: item for item in database["instances"]}
    or2_loads = []
    for load in loads:
        require(load in pins, "real baseline data_in load is absent from pins")
        pin = pins[load]
        instance = instances.get(pin["instance_name"])
        if (instance is not None and instance["cell"] == "OR2_X1"
                and pin["direction"] == 1 and pin["port"] is False):
            or2_loads.append(load)
    require(len(or2_loads) >= 4,
            "real baseline data_in net has fewer than four distinct OR2 input loads")
    return len(or2_loads)


def validate_real_electrical_change(functionals: list[dict[str, Any]]) -> None:
    require(len(functionals) >= 2, "real trace has no edited prefix")
    before = functionals[0]
    after = functionals[1]
    before_load = numeric_values([item["output_load"] for group in ("arcs", "start_seeds")
                                 for item in before["final_csr_input"][group]])
    after_load = numeric_values([item["output_load"] for group in ("arcs", "start_seeds")
                                for item in after["final_csr_input"][group]])
    before_delay = numeric_values(before["final_csr_timing"]["arcs"]["timing_arc_delays"])
    after_delay = numeric_values(after["final_csr_timing"]["arcs"]["timing_arc_delays"])
    before_slew = numeric_values(before["final_csr_timing"]["data_slews"])
    after_slew = numeric_values(after["final_csr_timing"]["data_slews"])
    require(before_load != after_load and any(value > 0.0 for value in after_load),
            "real prefix 0->1 did not change finite electrical loads")
    require(before_delay != after_delay and any(value > 0.0 for value in after_delay),
            "real prefix 0->1 did not change finite arc delays")
    require(before_slew != after_slew and any(value > 0.0 for value in after_slew),
            "real prefix 0->1 did not change finite data slews")


def validate_cross_prefix(trace: dict[str, Any],
                          functionals: list[dict[str, Any]]) -> dict[str, Any]:
    require(len(functionals) == len(trace["replay"]["workload"]["batches"]) + 1,
            "functional prefix set is incomplete")
    validate_generation_timing(trace, functionals)
    real_change = workload_kind(trace) == "real_design"
    high_fanout_load_count = 0
    if real_change:
        high_fanout_load_count = validate_real_same_net_high_fanout(functionals)
        validate_real_electrical_change(functionals)
    return {
        "active_database_projection_exact": True,
        "csr_closure_exact": True,
        "generation_preselection_timing_exact": True,
        "normalized_edits_and_native_materialization_exact": True,
        "real_load_delay_slew_changed": real_change,
        "real_same_net_or2_load_count": high_fanout_load_count,
    }


def self_test_terminal_empty_round() -> None:
    token = {"schema_version": 1, "case_id": "self", "profile_id": "p",
             "seed": 1, "batch_ordinal": 1, "accepted_ordinal": 0}
    candidate = {"kind": "existing", "baseline_key": {
        "kind": "instance_pin", "full_name": "u0:A",
        "instance_name": "u0", "pin_name": "A"}}
    numeric = {"arrival_ns": 1.0, "required_ns": 2.0, "slack_ns": 1.0}
    trace = {
        "replay": {"workload": {"batches": [{"edits": [
            {"creation_token": token}]}]}},
        "generation_only": {
            "stop_reason": "empty_accepted_batch",
            "rounds": [
                {
                    "accepted_tokens": [token],
                    "evaluations": [{
                        "decision": "accepted",
                        "accepted_creation_token": token,
                        "candidate_pin": candidate,
                        "numeric": dict(numeric),
                    }],
                },
                {
                    "accepted_tokens": [],
                    "evaluations": [{
                        "decision": "rejected",
                        "candidate_pin": candidate,
                        "numeric": dict(numeric),
                    }],
                },
            ],
        },
    }
    node = {"id": 0, "key": candidate["baseline_key"]}
    vector = {"presence": [1], "values": [
        {"kind": "finite", "negative_zero": False, "value": 1.0}]}
    required = {"presence": [1], "values": [
        {"kind": "finite", "negative_zero": False, "value": 2.0}]}
    before = {
        "final_csr_input": {"nodes": [node]},
        "final_csr_timing": {"nodes": {
            "arrivals": vector, "requireds": required, "slacks": vector}},
        "resolver_state": {"node_bindings": []},
    }
    validate_generation_timing(trace, [before, before])
    trace["generation_only"]["rounds"][1]["evaluations"][0]["numeric"][
        "arrival_ns"] = 9.0
    try:
        validate_generation_timing(trace, [before, before])
    except FunctionalEvidenceError:
        return
    raise FunctionalEvidenceError(
        "terminal empty-round numeric tamper was not detected")


def self_test_derived_freshness() -> None:
    value = {
        "exercised": True,
        "timing_point": "u0:A",
        "baseline_arrival": 1.0,
        "tampered_arrival": 2.0,
        "recovered_arrival": 1.0,
        "csr_node_count": 1,
        "csr_arc_count": 1,
        "tamper_observed": True,
        "rerun_input_exact": True,
        "rerun_cpu_exact": True,
        "rerun_identity_exact": True,
        "rollback_owner_restored": True,
        "rollback_input_exact": True,
        "rollback_cpu_exact": True,
        "rollback_identity_exact": True,
        "session_remained_baseline_executed": True,
        "authority_remained_active": True,
    }
    validate_derived_freshness(value, "real_design", "self-test")
    for poison in (None, float("inf"), True):
        broken = dict(value)
        broken["baseline_arrival"] = poison
        try:
            validate_derived_freshness(broken, "real_design", "self-test")
        except FunctionalEvidenceError:
            continue
        raise FunctionalEvidenceError(
            "non-finite derived freshness arrival was accepted")
