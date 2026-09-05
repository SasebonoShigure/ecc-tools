#!/usr/bin/env python3
"""Recursive typed validators for the Stage 5 exact-receipt JSON wire."""

import copy
import math
from typing import Any, Callable
from dual_csr_reference_model_validator import (
    run_reference_model_self_test,
    validate_reference_value as validate_exact_reference_value)

Require = Callable[[bool, str], None]


def _object(value: object, keys: set[str], label: str,
            require: Require) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(set(value) == keys, f"{label} fields differ")
    return value


def _array(value: object, label: str, require: Require) -> list[Any]:
    require(isinstance(value, list), f"{label} must be an array")
    return value


def _count(value: object, label: str, require: Require) -> int:
    require(type(value) is int and value >= 0,
            f"{label} must be a nonnegative integer")
    return value


def _text(value: object, label: str, require: Require) -> str:
    require(isinstance(value, str), f"{label} must be a string")
    return value


def _boolean(value: object, label: str, require: Require) -> bool:
    require(type(value) is bool, f"{label} must be a boolean")
    return value


def _raw_number(value: object, label: str, require: Require) -> None:
    require(type(value) in {int, float} and math.isfinite(float(value)),
            f"{label} must be a finite raw number")


def validate_number(value: object, label: str, require: Require) -> None:
    require(isinstance(value, dict), f"{label} must be a semantic number")
    kind = value.get("kind")
    if kind == "finite":
        item = _object(value, {"kind", "value", "negative_zero"}, label, require)
        _raw_number(item["value"], f"{label}.value", require)
        _boolean(item["negative_zero"], f"{label}.negative_zero", require)
        require(not item["negative_zero"] or float(item["value"]) == 0.0,
                f"{label} marks a nonzero value as negative zero")
    else:
        item = _object(value, {"kind"}, label, require)
        require(item["kind"] in {"pos_inf", "neg_inf"},
                f"{label} has an invalid semantic number kind")


def _nested_numbers(value: object, label: str, require: Require) -> None:
    if isinstance(value, list):
        for index, item in enumerate(value):
            _nested_numbers(item, f"{label}[{index}]", require)
    else:
        validate_number(value, label, require)


def _node_key(value: object, label: str, require: Require) -> None:
    item = _object(value, {"kind", "full_name", "instance_name", "pin_name"},
                   label, require)
    for key in item:
        _text(item[key], f"{label}.{key}", require)



def _library_signature(value: object, label: str, require: Require) -> None:
    keys = {"library_name", "arc_index", "sense", "trigger_transition",
            "check_transition", "sdf_condition", "delay_tables", "slew_tables"}
    item = _object(value, keys, label, require)
    for key in ("library_name", "sense", "trigger_transition",
                "check_transition", "sdf_condition"):
        _text(item[key], f"{label}.{key}", require)
    _count(item["arc_index"], f"{label}.arc_index", require)
    for field in ("delay_tables", "slew_tables"):
        for index, table in enumerate(_array(item[field], f"{label}.{field}", require)):
            typed = _object(table, {"present", "variable1", "variable2"},
                            f"{label}.{field}[{index}]", require)
            _boolean(typed["present"], f"{label}.{field}[{index}].present", require)
            _text(typed["variable1"], f"{label}.{field}[{index}].variable1", require)
            _text(typed["variable2"], f"{label}.{field}[{index}].variable2", require)


def _arc_key(value: object, label: str, require: Require) -> None:
    keys = {"source", "sink", "kind", "owner_name", "library_source_port",
            "library_sink_port", "library_arcs", "parallel_ordinal"}
    item = _object(value, keys, label, require)
    _node_key(item["source"], f"{label}.source", require)
    _node_key(item["sink"], f"{label}.sink", require)
    for key in ("kind", "owner_name", "library_source_port", "library_sink_port"):
        _text(item[key], f"{label}.{key}", require)
    _count(item["parallel_ordinal"], f"{label}.parallel_ordinal", require)
    for index, signature in enumerate(_array(
            item["library_arcs"], f"{label}.library_arcs", require)):
        _library_signature(signature, f"{label}.library_arcs[{index}]", require)


def _timing_table(value: object, label: str, require: Require) -> None:
    item = _object(value, {"present", "variable1", "variable2", "axes", "values"},
                   label, require)
    _boolean(item["present"], f"{label}.present", require)
    _text(item["variable1"], f"{label}.variable1", require)
    _text(item["variable2"], f"{label}.variable2", require)
    _nested_numbers(item["axes"], f"{label}.axes", require)
    _nested_numbers(item["values"], f"{label}.values", require)


def _child_arc(value: object, label: str, require: Require) -> None:
    keys = {"signature", "time_unit_scale", "capacitance_unit_scale",
            "slew_derate", "slew_lower_threshold_pct", "slew_upper_threshold_pct",
            "input_threshold_pct", "output_threshold_pct", "delay_tables",
            "slew_tables", "check_tables"}
    item = _object(value, keys, label, require)
    _library_signature(item["signature"], f"{label}.signature", require)
    for field in ("time_unit_scale", "capacitance_unit_scale", "slew_derate",
                  "slew_lower_threshold_pct", "slew_upper_threshold_pct",
                  "input_threshold_pct", "output_threshold_pct"):
        _nested_numbers(item[field], f"{label}.{field}", require)
    for field in ("delay_tables", "slew_tables", "check_tables"):
        for index, table in enumerate(_array(item[field], f"{label}.{field}", require)):
            _timing_table(table, f"{label}.{field}[{index}]", require)


def _transfer(value: object, label: str, require: Require) -> None:
    keys = {"delay", "delay_min", "delay_max", "is_clock_arc", "is_disabled",
            "is_clear_or_preset", "children"}
    item = _object(value, keys, label, require)
    for field in ("delay", "delay_min", "delay_max"):
        validate_number(item[field], f"{label}.{field}", require)
    for field in ("is_clock_arc", "is_disabled", "is_clear_or_preset"):
        _boolean(item[field], f"{label}.{field}", require)
    for index, child in enumerate(_array(item["children"], f"{label}.children", require)):
        _child_arc(child, f"{label}.children[{index}]", require)


def validate_arc_payload(value: object, label: str, require: Require) -> None:
    keys = {"key", "source", "sink", "kind", "is_clock_arc", "is_disabled",
            "is_loop_disabled", "eligibility", "transfer", "output_load"}
    item = _object(value, keys, label, require)
    _arc_key(item["key"], f"{label}.key", require)
    _count(item["source"], f"{label}.source", require)
    _count(item["sink"], f"{label}.sink", require)
    _text(item["kind"], f"{label}.kind", require)
    for field in ("is_clock_arc", "is_disabled", "is_loop_disabled"):
        _boolean(item[field], f"{label}.{field}", require)
    eligibility = _object(item["eligibility"],
                          {"levelization", "data_arrival", "data_slew", "clock"},
                          f"{label}.eligibility", require)
    for field in eligibility:
        _boolean(eligibility[field], f"{label}.eligibility.{field}", require)
    _transfer(item["transfer"], f"{label}.transfer", require)
    if item["output_load"] is not None:
        _nested_numbers(item["output_load"], f"{label}.output_load", require)


def validate_node_record(value: object, label: str, require: Require) -> None:
    keys = {"id", "key", "direction", "instance_cell", "net_name", "is_port",
            "is_startpoint", "is_endpoint", "is_sequential", "is_clock_pin",
            "is_data_pin", "is_output_pin"}
    item = _object(value, keys, label, require)
    _count(item["id"], f"{label}.id", require)
    _node_key(item["key"], f"{label}.key", require)
    for field in ("direction", "instance_cell", "net_name"):
        _text(item[field], f"{label}.{field}", require)
    for field in keys - {"id", "key", "direction", "instance_cell", "net_name"}:
        _boolean(item[field], f"{label}.{field}", require)


REFERENCE_KEYS = {"schema_version", "source_identity", "analysis_context",
    "analysis_order", "path_source_order", "transition_order", "instances",
    "ordered_root_ids", "ordered_start_ids", "ordered_end_ids", "clock_tags",
    "clocks", "clock_owner_by_node", "port_constraints",
    "case_analysis_constraints", "timing_check_endpoints",
    "referenced_library_cells", "parasitics", "start_seeds",
    "endpoint_required", "cell_pin_capacitances", "unsupported_reasons"}


def validate_reference_delta(value: object, label: str, require: Require,
                             chain: dict[str, object]) -> None:
    item = _object(value, REFERENCE_KEYS, label, require)
    for field, replacement in item.items():
        if replacement is None:
            continue
        typed = _object(replacement, {"expected", "replacement"},
                        f"{label}.{field}", require)
        validate_exact_reference_value(field, typed["expected"],
                                       f"{label}.{field}.expected", require)
        validate_exact_reference_value(field, typed["replacement"],
                                       f"{label}.{field}.replacement", require)
        if field in chain:
            require(typed["expected"] == chain[field],
                    f"{label}.{field} breaks the replacement chain")
        chain[field] = copy.deepcopy(typed["replacement"])


def validate_descriptor_schema(value: object, label: str,
                               require: Require) -> None:
    require(value == "ista-dual-csr-update/1.2.0",
            f"{label} exact schema differs")


def run_typed_validator_self_test(require: Require) -> None:
    errors: list[str] = []
    validate_descriptor_schema("ista-dual-csr-update/1.2.0", "schema-canary",
                               lambda ok, message: errors.append(message)
                               if not ok else None)
    require(not errors, f"valid descriptor schema was rejected: {errors}")
    validate_descriptor_schema("ista-dual-csr-update/1.1.0", "old-schema-canary",
                               lambda ok, message: errors.append(message)
                               if not ok else None)
    require(errors, "descriptor schema tamper was accepted")
    number = {"kind": "finite", "value": 0.0, "negative_zero": False}
    payload = {"key": {"source": {"kind": "port", "full_name": "a",
        "instance_name": "", "pin_name": ""}, "sink": {"kind": "port",
        "full_name": "b", "instance_name": "", "pin_name": ""},
        "kind": "net", "owner_name": "n", "library_source_port": "",
        "library_sink_port": "", "library_arcs": [], "parallel_ordinal": 0},
        "source": 0, "sink": 1, "kind": "net", "is_clock_arc": False,
        "is_disabled": False, "is_loop_disabled": False,
        "eligibility": {"levelization": True, "data_arrival": True,
        "data_slew": True, "clock": False}, "transfer": {"delay": number,
        "delay_min": number, "delay_max": number, "is_clock_arc": False,
        "is_disabled": False, "is_clear_or_preset": False, "children": []},
        "output_load": None}
    for mutate in ("id", "transfer"):
        tampered = copy.deepcopy(payload)
        if mutate == "id":
            tampered["source"] = "0"
        else:
            tampered["transfer"] = {"same_size": "object"}
        errors = []
        try:
            validate_arc_payload(tampered, f"payload-{mutate}-canary",
                                 lambda ok, message: errors.append(message)
                                 if not ok else None)
        except (KeyError, TypeError):
            errors.append("recursive typed validation rejected malformed payload")
        require(errors, f"nested payload {mutate} tamper was accepted")
    negative_zero = {"kind": "finite", "value": 0.0, "negative_zero": True}
    library_cell = {"cell_name": "BUF", "present": True,
        "library_name": "lib", "is_sequential": False,
        "is_clock_gating": False, "is_macro": False, "clock_pin_name": "",
        "data_pin_name": "", "output_pin_name": "Y", "ports": [{
            "name": "A", "direction": "input", "is_clock": False,
            "is_output": False, "default_capacitance": number,
            "transition_capacitance": [[None, negative_zero], [number, None]]}],
        "cell_arcs": [], "timing_checks": []}
    errors = []
    try:
        validate_exact_reference_value(
            "referenced_library_cells", [library_cell], "optional-lane-canary",
            lambda ok, message: errors.append(message) if not ok else None)
    except (KeyError, TypeError):
        errors.append("valid optional lane matrix raised")
    require(not errors, f"valid optional lane matrix was rejected: {errors}")
    invalid_lanes = copy.deepcopy(library_cell)
    invalid_lanes["ports"][0]["transition_capacitance"][0][0] = {"kind": "bogus"}
    errors = []
    validate_exact_reference_value(
        "referenced_library_cells", [invalid_lanes], "invalid-optional-lane-canary",
        lambda ok, message: errors.append(message) if not ok else None)
    require(errors, "non-null optional lane tamper was accepted")
    for temperature in ("25", "__unspecified_temperature__"):
        errors = []
        validate_exact_reference_value(
            "analysis_context", {"liberty_corner": "c", "temperature": temperature,
             "rc_corner": "r"}, "temperature-wire-positive",
            lambda ok, message: errors.append(message) if not ok else None)
        require(not errors,
                f"valid wire temperature {temperature!r} was rejected: {errors}")
    for temperature in (25.0, True, "25.0", "-0", " 25", "25 ", "025",
                        "25.10", "1e2", "NaN", "Inf"):
        errors = []
        validate_exact_reference_value(
            "analysis_context", {"liberty_corner": "c", "temperature": temperature,
             "rc_corner": "r"}, "temperature-wire-negative",
            lambda ok, message: errors.append(message) if not ok else None)
        require(errors,
                f"noncanonical wire temperature {temperature!r} was accepted")
    chain = {"analysis_context": {"liberty_corner": "c", "temperature": "25",
                                  "rc_corner": "r"}}
    delta = {field: None for field in REFERENCE_KEYS}
    delta["analysis_context"] = {"expected": {"liberty_corner": "x",
        "temperature": "25", "rc_corner": "r"}, "replacement": {
        "liberty_corner": "c2", "temperature": "25", "rc_corner": "r"}}
    errors = []
    validate_reference_delta(delta, "reference-chain-canary",
                             lambda ok, message: errors.append(message)
                             if not ok else None, chain)
    require(errors, "reference analysis_context chain tamper was accepted")
    from dual_csr_arc_group_validator import validate_arc_group_semantics

    node_a = {"kind": "port", "full_name": "a", "instance_name": "",
              "pin_name": ""}
    node_b = {"kind": "port", "full_name": "b", "instance_name": "",
              "pin_name": ""}
    node_c = {"kind": "port", "full_name": "c", "instance_name": "",
              "pin_name": ""}

    def arc_key(sink: dict[str, str], owner: str, ordinal: int) -> dict[str, Any]:
        return {"source": node_a, "sink": sink, "kind": "net",
                "owner_name": owner, "library_source_port": "",
                "library_sink_port": "", "library_arcs": [],
                "parallel_ordinal": ordinal}

    key0 = arc_key(node_b, "n0", 0)
    key1 = arc_key(node_c, "n1", 0)
    key2 = arc_key(node_b, "n0", 1)
    base0 = dict(key0)
    base0.pop("parallel_ordinal")
    base1 = dict(key1)
    base1.pop("parallel_ordinal")
    net_sink0 = {"owner_name": "n0", "sink": node_b}
    net_sink1 = {"owner_name": "n1", "sink": node_c}
    group_index = {
        "slots": [{"arc_id": 0, "current_active_key": key0},
                  {"arc_id": 1, "current_active_key": key1},
                  {"arc_id": 2, "current_active_key": key2}],
        "active_groups": [{"base": base0, "active_arc_ids": [0, 2]},
                          {"base": base1, "active_arc_ids": [1]}],
        "active_sink_net_groups": [
            {"sink_net_name": "nb", "active_arc_ids": [0, 2]},
            {"sink_net_name": "nc", "active_arc_ids": [1]}],
        "active_net_sink_groups": [
            {"key": net_sink0, "active_arc_ids": [0, 2]},
            {"key": net_sink1, "active_arc_ids": [1]}],
    }
    functional_graph = {"nodes": [
        {"key": node_a, "net_name": "na"},
        {"key": node_b, "net_name": "nb"},
        {"key": node_c, "net_name": "nc"}], "active_arcs": []}

    def group_errors(candidate: dict[str, Any]) -> list[str]:
        found: list[str] = []
        validate_arc_group_semantics(
            candidate, functional_graph, "arc-group-canary",
            lambda ok, message: found.append(message) if not ok else None)
        return found

    require(not group_errors(group_index), "valid semantic groups were rejected")
    swapped = copy.deepcopy(group_index)
    swapped["active_sink_net_groups"][0]["active_arc_ids"] = [1]
    swapped["active_sink_net_groups"][1]["active_arc_ids"] = [0, 2]
    require(group_errors(swapped), "sink-net member swap was accepted")
    split = copy.deepcopy(group_index)
    split["active_net_sink_groups"] = [
        {"key": net_sink0, "active_arc_ids": [0]},
        {"key": net_sink0, "active_arc_ids": [2]},
        {"key": net_sink1, "active_arc_ids": [1]}]
    require(group_errors(split), "split net-sink group was accepted")
    ordinal_gap = copy.deepcopy(group_index)
    ordinal_gap["slots"][2]["current_active_key"]["parallel_ordinal"] = 2
    require(group_errors(ordinal_gap),
            "non-contiguous active ArcKey ordinals were accepted")
    from dual_csr_structural_semantics_validator import run_structural_semantics_self_test
    run_structural_semantics_self_test(require)
    from dual_csr_descriptor_replay_validator import run_descriptor_replay_self_test
    run_descriptor_replay_self_test(require)
    from dual_csr_descriptor_row_validator import run_descriptor_row_self_test
    run_descriptor_row_self_test(require)
    run_reference_model_self_test(require)
    from dual_csr_reference_replay_validator import run_reference_replay_self_test
    run_reference_replay_self_test(require)
    from dual_csr_resolver_semantics_validator import run_resolver_semantics_self_test
    run_resolver_semantics_self_test(require)
