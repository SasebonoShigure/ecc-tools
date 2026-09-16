#!/usr/bin/env python3
"""Exact validators for every CSR reference-payload JSON model."""

import math
import re
from typing import Any, Callable

Require = Callable[[bool, str], None]
NODE_KINDS = {"port", "instance_pin"}
ARC_KINDS = {"cell", "net"}
DIRECTIONS = {"none", "input", "output", "inout"}
ANALYSES = {"max", "min"}
TRANSITIONS = {"rise", "fall", "none"}
PATH_SOURCES = {"input", "register"}
AVAILABILITY = {"materialized", "deferred_sequential"}
SENSES = {"none_positive_fallback", "positive_unate",
           "negative_unate", "non_unate"}
TABLE_VARIABLES = {"none", "input_transition", "output_capacitance",
                   "constrained_transition"}
CHECK_KINDS = {"none", "setup", "hold", "recovery", "removal", "width",
               "period"}
UNSUPPORTED = {"parasitic_net", "sequential_instance", "sequential_launch",
               "timing_check", "crpr", "propagated_clock",
               "clock_tree_fanout", "clock_gating_or_divider",
               "conditional_timing_arc", "case_analysis"}


def _object(value: object, keys: set[str], label: str,
            require: Require) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(set(value) == keys, f"{label} fields differ")
    return value


def _array(value: object, label: str, require: Require) -> list[Any]:
    require(isinstance(value, list), f"{label} must be an array")
    return value


def _text(value: object, label: str, require: Require) -> str:
    require(isinstance(value, str), f"{label} must be a string")
    return value


def _boolean(value: object, label: str, require: Require) -> bool:
    require(type(value) is bool, f"{label} must be a boolean")
    return value


def _count(value: object, label: str, require: Require) -> int:
    require(type(value) is int and value >= 0,
            f"{label} must be a nonnegative integer")
    return value


def _signed_index(value: object, label: str, require: Require) -> int:
    require(type(value) is int and -(2 ** 31) <= value < 2 ** 31,
            f"{label} must be a signed 32-bit integer")
    return value


def _enum(value: object, choices: set[str], label: str,
          require: Require) -> str:
    token = _text(value, label, require)
    require(token in choices, f"{label} has an unknown enum token")
    return token


def _number(value: object, label: str, require: Require) -> None:
    require(isinstance(value, dict), f"{label} must be a semantic number")
    kind = value.get("kind")
    if kind == "finite":
        item = _object(value, {"kind", "value", "negative_zero"},
                       label, require)
        require(type(item["value"]) in {int, float}
                and math.isfinite(float(item["value"])),
                f"{label}.value must be finite")
        _boolean(item["negative_zero"], f"{label}.negative_zero", require)
        require(not item["negative_zero"] or float(item["value"]) == 0.0,
                f"{label} marks a nonzero value as negative zero")
    else:
        item = _object(value, {"kind"}, label, require)
        require(item["kind"] in {"pos_inf", "neg_inf"},
                f"{label} has an invalid semantic number kind")


def _optional_number(value: object, label: str, require: Require) -> None:
    if value is not None:
        _number(value, label, require)


def _optional_id(value: object, label: str, require: Require) -> None:
    if value is not None:
        _count(value, label, require)


def _fixed_array(value: object, size: int, label: str,
                 require: Require) -> list[Any]:
    result = _array(value, label, require)
    require(len(result) == size, f"{label} must contain {size} items")
    return result


def _optional_analysis(value: object, label: str, require: Require) -> None:
    for index, item in enumerate(_fixed_array(value, 2, label, require)):
        _optional_number(item, f"{label}[{index}]", require)


def _lane_matrix(value: object, label: str, require: Require) -> None:
    for analysis, lanes in enumerate(_fixed_array(value, 2, label, require)):
        for lane, item in enumerate(_fixed_array(
                lanes, 2, f"{label}[{analysis}]", require)):
            _optional_number(item, f"{label}[{analysis}][{lane}]", require)


def _table_signature(value: object, label: str, require: Require) -> None:
    item = _object(value, {"present", "variable1", "variable2"},
                   label, require)
    _boolean(item["present"], f"{label}.present", require)
    _enum(item["variable1"], TABLE_VARIABLES, f"{label}.variable1", require)
    _enum(item["variable2"], TABLE_VARIABLES, f"{label}.variable2", require)


def _library_signature(value: object, label: str, require: Require) -> None:
    keys = {"library_name", "arc_index", "sense", "trigger_transition",
            "check_transition", "sdf_condition", "delay_tables", "slew_tables"}
    item = _object(value, keys, label, require)
    _text(item["library_name"], f"{label}.library_name", require)
    _signed_index(item["arc_index"], f"{label}.arc_index", require)
    _enum(item["sense"], SENSES, f"{label}.sense", require)
    _enum(item["trigger_transition"], TRANSITIONS,
          f"{label}.trigger_transition", require)
    _enum(item["check_transition"], TRANSITIONS,
          f"{label}.check_transition", require)
    _text(item["sdf_condition"], f"{label}.sdf_condition", require)
    for field in ("delay_tables", "slew_tables"):
        for index, table in enumerate(_fixed_array(
                item[field], 2, f"{label}.{field}", require)):
            _table_signature(table, f"{label}.{field}[{index}]", require)


def _timing_table(value: object, label: str, require: Require) -> None:
    item = _object(value, {"present", "variable1", "variable2", "axes", "values"},
                   label, require)
    _boolean(item["present"], f"{label}.present", require)
    _enum(item["variable1"], TABLE_VARIABLES, f"{label}.variable1", require)
    _enum(item["variable2"], TABLE_VARIABLES, f"{label}.variable2", require)
    for axis, values in enumerate(_array(item["axes"], f"{label}.axes", require)):
        for index, number in enumerate(_array(
                values, f"{label}.axes[{axis}]", require)):
            _number(number, f"{label}.axes[{axis}][{index}]", require)
    for index, number in enumerate(_array(item["values"], f"{label}.values", require)):
        _number(number, f"{label}.values[{index}]", require)


def _child(value: object, label: str, require: Require) -> None:
    keys = {"signature", "time_unit_scale", "capacitance_unit_scale",
            "slew_derate", "slew_lower_threshold_pct",
            "slew_upper_threshold_pct", "input_threshold_pct",
            "output_threshold_pct", "delay_tables", "slew_tables",
            "check_tables"}
    item = _object(value, keys, label, require)
    _library_signature(item["signature"], f"{label}.signature", require)
    for field in ("time_unit_scale", "capacitance_unit_scale", "slew_derate"):
        _number(item[field], f"{label}.{field}", require)
    for field in ("slew_lower_threshold_pct", "slew_upper_threshold_pct",
                  "input_threshold_pct", "output_threshold_pct"):
        for index, number in enumerate(_fixed_array(
                item[field], 2, f"{label}.{field}", require)):
            _number(number, f"{label}.{field}[{index}]", require)
    for field in ("delay_tables", "slew_tables", "check_tables"):
        for index, table in enumerate(_fixed_array(
                item[field], 2, f"{label}.{field}", require)):
            _timing_table(table, f"{label}.{field}[{index}]", require)


def _transfer(value: object, label: str, require: Require) -> None:
    keys = {"delay", "delay_min", "delay_max", "is_clock_arc", "is_disabled",
            "is_clear_or_preset", "children"}
    item = _object(value, keys, label, require)
    for field in ("delay", "delay_min", "delay_max"):
        _number(item[field], f"{label}.{field}", require)
    for field in ("is_clock_arc", "is_disabled", "is_clear_or_preset"):
        _boolean(item[field], f"{label}.{field}", require)
    for index, child in enumerate(_array(item["children"],
                                         f"{label}.children", require)):
        _child(child, f"{label}.children[{index}]", require)


def _instance(value: object, label: str, require: Require) -> None:
    keys = {"instance_name", "cell_name", "ordered_pins", "is_sequential",
            "has_timing_checks", "is_clock_gating", "is_macro"}
    item = _object(value, keys, label, require)
    _text(item["instance_name"], f"{label}.instance_name", require)
    _text(item["cell_name"], f"{label}.cell_name", require)
    for index, node in enumerate(_array(item["ordered_pins"],
                                        f"{label}.ordered_pins", require)):
        _count(node, f"{label}.ordered_pins[{index}]", require)
    for field in keys - {"instance_name", "cell_name", "ordered_pins"}:
        _boolean(item[field], f"{label}.{field}", require)


def _library_cell(value: object, label: str, require: Require) -> None:
    keys = {"cell_name", "present", "library_name", "is_sequential",
            "is_clock_gating", "is_macro", "clock_pin_name", "data_pin_name",
            "output_pin_name", "ports", "cell_arcs", "timing_checks"}
    item = _object(value, keys, label, require)
    for field in ("cell_name", "library_name", "clock_pin_name",
                  "data_pin_name", "output_pin_name"):
        _text(item[field], f"{label}.{field}", require)
    for field in ("present", "is_sequential", "is_clock_gating", "is_macro"):
        _boolean(item[field], f"{label}.{field}", require)
    for index, port in enumerate(_array(item["ports"], f"{label}.ports", require)):
        port_label = f"{label}.ports[{index}]"
        typed = _object(port, {"name", "direction", "is_clock", "is_output",
                               "default_capacitance", "transition_capacitance"},
                        port_label, require)
        _text(typed["name"], f"{port_label}.name", require)
        _enum(typed["direction"], DIRECTIONS, f"{port_label}.direction", require)
        _boolean(typed["is_clock"], f"{port_label}.is_clock", require)
        _boolean(typed["is_output"], f"{port_label}.is_output", require)
        _number(typed["default_capacitance"],
                f"{port_label}.default_capacitance", require)
        _lane_matrix(typed["transition_capacitance"],
                     f"{port_label}.transition_capacitance", require)
    for index, arc in enumerate(_array(item["cell_arcs"],
                                       f"{label}.cell_arcs", require)):
        arc_label = f"{label}.cell_arcs[{index}]"
        typed = _object(arc, {"source_port", "sink_port", "transfer"},
                        arc_label, require)
        _text(typed["source_port"], f"{arc_label}.source_port", require)
        _text(typed["sink_port"], f"{arc_label}.sink_port", require)
        _transfer(typed["transfer"], f"{arc_label}.transfer", require)
    for index, check in enumerate(_array(item["timing_checks"],
                                         f"{label}.timing_checks", require)):
        check_label = f"{label}.timing_checks[{index}]"
        typed = _object(check, {"clock_port", "data_port", "kind",
                                "check_time", "clock_transition", "children"},
                        check_label, require)
        _text(typed["clock_port"], f"{check_label}.clock_port", require)
        _text(typed["data_port"], f"{check_label}.data_port", require)
        _enum(typed["kind"], CHECK_KINDS, f"{check_label}.kind", require)
        _number(typed["check_time"], f"{check_label}.check_time", require)
        _enum(typed["clock_transition"], TRANSITIONS,
              f"{check_label}.clock_transition", require)
        for child_index, child in enumerate(_array(
                typed["children"], f"{check_label}.children", require)):
            _child(child, f"{check_label}.children[{child_index}]", require)


def _raw_node(value: object, label: str, require: Require) -> None:
    item = _object(value, {"full_name", "resolved_node"}, label, require)
    _text(item["full_name"], f"{label}.full_name", require)
    _optional_id(item["resolved_node"], f"{label}.resolved_node", require)


def _clock(value: object, label: str, require: Require) -> None:
    keys = {"map_key", "embedded_clock_name", "ordered_sources", "period",
            "edges", "propagated"}
    item = _object(value, keys, label, require)
    _text(item["map_key"], f"{label}.map_key", require)
    _text(item["embedded_clock_name"], f"{label}.embedded_clock_name", require)
    for index, source in enumerate(_array(item["ordered_sources"],
                                          f"{label}.ordered_sources", require)):
        _raw_node(source, f"{label}.ordered_sources[{index}]", require)
    _number(item["period"], f"{label}.period", require)
    for index, edge in enumerate(_fixed_array(item["edges"], 2,
                                               f"{label}.edges", require)):
        _number(edge, f"{label}.edges[{index}]", require)
    _boolean(item["propagated"], f"{label}.propagated", require)


def _port(value: object, label: str, require: Require) -> None:
    keys = {"map_key", "embedded_port_name", "resolved_node", "input_delay",
            "input_transition", "output_delay", "output_load", "clock_name"}
    item = _object(value, keys, label, require)
    for field in ("map_key", "embedded_port_name", "clock_name"):
        _text(item[field], f"{label}.{field}", require)
    _optional_id(item["resolved_node"], f"{label}.resolved_node", require)
    _optional_analysis(item["input_delay"], f"{label}.input_delay", require)
    _optional_number(item["input_transition"], f"{label}.input_transition", require)
    _optional_analysis(item["output_delay"], f"{label}.output_delay", require)
    _optional_number(item["output_load"], f"{label}.output_load", require)


def _case(value: object, label: str, require: Require) -> None:
    item = _object(value, {"raw_name", "resolved_node", "value"},
                   label, require)
    _text(item["raw_name"], f"{label}.raw_name", require)
    _optional_id(item["resolved_node"], f"{label}.resolved_node", require)
    _boolean(item["value"], f"{label}.value", require)


def _timing_check(value: object, label: str, require: Require) -> None:
    keys = {"instance_name", "referenced_check_ordinal", "check_kind",
            "clock_pin_raw_name", "data_pin_raw_name", "clock_pin_full_name",
            "data_pin_full_name", "resolved_clock_node", "resolved_data_node"}
    item = _object(value, keys, label, require)
    for field in ("instance_name", "clock_pin_raw_name", "data_pin_raw_name",
                  "clock_pin_full_name", "data_pin_full_name"):
        _text(item[field], f"{label}.{field}", require)
    _count(item["referenced_check_ordinal"],
           f"{label}.referenced_check_ordinal", require)
    _enum(item["check_kind"], CHECK_KINDS, f"{label}.check_kind", require)
    _optional_id(item["resolved_clock_node"],
                 f"{label}.resolved_clock_node", require)
    _optional_id(item["resolved_data_node"],
                 f"{label}.resolved_data_node", require)


def _seed(value: object, label: str, require: Require) -> None:
    keys = {"seed_node", "path_start_node", "availability", "path_source",
            "clock_tag", "arrival", "slew", "crpr_node", "crpr_transition"}
    item = _object(value, keys, label, require)
    _count(item["seed_node"], f"{label}.seed_node", require)
    _count(item["path_start_node"], f"{label}.path_start_node", require)
    _enum(item["availability"], AVAILABILITY, f"{label}.availability", require)
    if item["path_source"] is not None:
        _enum(item["path_source"], PATH_SOURCES, f"{label}.path_source", require)
    _text(item["clock_tag"], f"{label}.clock_tag", require)
    _lane_matrix(item["arrival"], f"{label}.arrival", require)
    _lane_matrix(item["slew"], f"{label}.slew", require)
    _optional_id(item["crpr_node"], f"{label}.crpr_node", require)
    if item["crpr_transition"] is not None:
        _enum(item["crpr_transition"], TRANSITIONS,
              f"{label}.crpr_transition", require)


def _endpoint(value: object, label: str, require: Require) -> None:
    item = _object(value, {"node", "clock_name", "clock_period",
                           "output_delay"}, label, require)
    _count(item["node"], f"{label}.node", require)
    _text(item["clock_name"], f"{label}.clock_name", require)
    _number(item["clock_period"], f"{label}.clock_period", require)
    _optional_analysis(item["output_delay"], f"{label}.output_delay", require)


def _capacitance(value: object, label: str, require: Require) -> None:
    item = _object(value, {"node", "default_capacitance",
                           "transition_capacitance"}, label, require)
    _count(item["node"], f"{label}.node", require)
    _number(item["default_capacitance"],
            f"{label}.default_capacitance", require)
    _lane_matrix(item["transition_capacitance"],
                 f"{label}.transition_capacitance", require)


def _parasitics(value: object, label: str, require: Require) -> None:
    item = _object(value, {"capacitance_unit", "resistance_unit", "nets"},
                   label, require)
    _text(item["capacitance_unit"], f"{label}.capacitance_unit", require)
    _text(item["resistance_unit"], f"{label}.resistance_unit", require)
    for net_index, net in enumerate(_array(item["nets"], f"{label}.nets", require)):
        net_label = f"{label}.nets[{net_index}]"
        typed = _object(net, {"map_key", "embedded_name",
                              "lumped_capacitance", "nodes", "resistors"},
                        net_label, require)
        _text(typed["map_key"], f"{net_label}.map_key", require)
        _text(typed["embedded_name"], f"{net_label}.embedded_name", require)
        _number(typed["lumped_capacitance"],
                f"{net_label}.lumped_capacitance", require)
        for node_index, node in enumerate(_array(
                typed["nodes"], f"{net_label}.nodes", require)):
            node_label = f"{net_label}.nodes[{node_index}]"
            record = _object(node, {"map_key", "embedded_name", "capacitance",
                                    "x", "y"}, node_label, require)
            _text(record["map_key"], f"{node_label}.map_key", require)
            _text(record["embedded_name"], f"{node_label}.embedded_name", require)
            for field in ("capacitance", "x", "y"):
                _number(record[field], f"{node_label}.{field}", require)
        for resistor_index, resistor in enumerate(_array(
                typed["resistors"], f"{net_label}.resistors", require)):
            resistor_label = f"{net_label}.resistors[{resistor_index}]"
            record = _object(resistor, {"source", "sink", "resistance"},
                             resistor_label, require)
            _text(record["source"], f"{resistor_label}.source", require)
            _text(record["sink"], f"{resistor_label}.sink", require)
            _number(record["resistance"],
                    f"{resistor_label}.resistance", require)


def _records(value: object, validator: Callable[[object, str, Require], None],
             label: str, require: Require) -> None:
    for index, record in enumerate(_array(value, label, require)):
        validator(record, f"{label}[{index}]", require)


def _analysis_context(value: object, label: str, require: Require) -> None:
    item = _object(value, {"liberty_corner", "temperature", "rc_corner"},
                   label, require)
    for field in ("liberty_corner", "temperature", "rc_corner"):
        raw = item[field]
        require(isinstance(raw, str), f"{label}.{field} must be a string")
        if isinstance(raw, str):
            require(bool(raw), f"{label}.{field} must be nonempty")
            require(not raw or (not raw[0].isspace()
                                and not raw[-1].isspace()),
                    f"{label}.{field} has outer whitespace")
    temperature = item["temperature"]
    if not isinstance(temperature, str):
        return
    canonical = (temperature == "__unspecified_temperature__"
                 or (temperature != "-0" and re.fullmatch(
                     r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]*[1-9])?",
                     temperature) is not None))
    require(canonical,
            f"{label}.temperature is not the canonical decimal or "
            "unspecified token")


def validate_reference_value(field: str, value: object, label: str,
                             require: Require) -> None:
    if field == "schema_version":
        require(value == "1.0.0", f"{label} exact schema differs")
    elif field == "source_identity":
        item = _object(value, {"fingerprint_schema_version", "session_token",
            "graph_generation", "topology_fingerprint",
            "clock_constraint_fingerprint", "library_fingerprint",
            "corner_fingerprint", "parasitic_fingerprint",
            "timing_input_fingerprint"}, label, require)
        _text(item["fingerprint_schema_version"],
              f"{label}.fingerprint_schema_version", require)
        _count(item["session_token"], f"{label}.session_token", require)
        _count(item["graph_generation"], f"{label}.graph_generation", require)
        for key in item.keys() - {"session_token", "graph_generation",
                                  "fingerprint_schema_version"}:
            _text(item[key], f"{label}.{key}", require)
    elif field == "analysis_context":
        _analysis_context(value, label, require)
    elif field == "analysis_order":
        require(value == ["max", "min"], f"{label} exact analysis order differs")
    elif field == "path_source_order":
        require(value == ["input", "register"],
                f"{label} exact path-source order differs")
    elif field == "transition_order":
        require(value == ["rise", "fall"],
                f"{label} exact transition order differs")
    elif field in {"ordered_root_ids", "ordered_start_ids", "ordered_end_ids"}:
        for index, node in enumerate(_array(value, label, require)):
            _count(node, f"{label}[{index}]", require)
    elif field == "clock_tags":
        for index, tag in enumerate(_array(value, label, require)):
            _text(tag, f"{label}[{index}]", require)
    elif field == "clock_owner_by_node":
        for index, owner in enumerate(_array(value, label, require)):
            require(owner is None or isinstance(owner, str),
                    f"{label}[{index}] must be null or a string")
    elif field == "unsupported_reasons":
        reasons = _array(value, label, require)
        for index, reason in enumerate(reasons):
            _enum(reason, UNSUPPORTED, f"{label}[{index}]", require)
        require(len(reasons) == len(set(reasons)),
                f"{label} contains duplicate reasons")
    elif field == "instances":
        _records(value, _instance, label, require)
    elif field == "clocks":
        _records(value, _clock, label, require)
    elif field == "port_constraints":
        _records(value, _port, label, require)
    elif field == "case_analysis_constraints":
        _records(value, _case, label, require)
    elif field == "timing_check_endpoints":
        _records(value, _timing_check, label, require)
    elif field == "referenced_library_cells":
        _records(value, _library_cell, label, require)
    elif field == "parasitics":
        _parasitics(value, label, require)
    elif field == "start_seeds":
        _records(value, _seed, label, require)
    elif field == "endpoint_required":
        _records(value, _endpoint, label, require)
    elif field == "cell_pin_capacitances":
        _records(value, _capacitance, label, require)
    else:
        require(False, f"{label} has an unknown reference field")


def run_reference_model_self_test(require: Require) -> None:
    zero = {"kind": "finite", "value": 0.0, "negative_zero": False}
    negative_zero = {"kind": "finite", "value": 0.0, "negative_zero": True}
    port = {"name": "A", "direction": "input", "is_clock": False,
            "is_output": False, "default_capacitance": zero,
            "transition_capacitance": [[None, negative_zero], [zero, None]]}
    cell = {"cell_name": "BUF", "present": True, "library_name": "lib",
            "is_sequential": False, "is_clock_gating": False, "is_macro": False,
            "clock_pin_name": "", "data_pin_name": "", "output_pin_name": "Y",
            "ports": [port], "cell_arcs": [], "timing_checks": []}
    errors: list[str] = []
    validate_reference_value("referenced_library_cells", [cell], "valid-cell",
                             lambda ok, message: errors.append(message)
                             if not ok else None)
    require(not errors, f"valid exact library model was rejected: {errors}")
    tampered = {"capacitance_unit": "pf", "resistance_unit": "ohm",
                "nets": [{"map_key": "n", "embedded_name": "n",
                          "lumped_capacitance": zero,
                          "nodes": [{"same_size": "object"}],
                          "resistors": []}]}
    errors = []
    try:
        validate_reference_value("parasitics", tampered, "parasitic-tamper",
                                 lambda ok, message: errors.append(message)
                                 if not ok else None)
    except (KeyError, TypeError):
        errors.append("nested parasitic tamper rejected")
    require(errors, "nested parasitic node tamper was accepted")
