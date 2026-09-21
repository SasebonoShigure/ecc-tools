#!/usr/bin/env python3
"""Executable equal-cost canaries for the Stage 6 winner comparator."""

import copy
from collections.abc import Callable, Sequence
from typing import Any


def _key(name: str) -> dict[str, str]:
    return {"kind": "port", "full_name": name,
            "instance_name": "", "pin_name": ""}


def _number(value: float) -> dict[str, Any]:
    return {"kind": "finite", "negative_zero": False, "value": value}


def _soa(presence: list[int], values: Sequence[object]) -> dict[str, object]:
    return {"presence": presence, "values": values}


def _seed(node: object) -> dict[str, object]:
    zeros = [[_number(0.0), _number(0.0)],
             [_number(0.0), _number(0.0)]]
    return {"availability": "materialized", "clock_tag": "clk",
            "path_source": "input", "seed_node": node,
            "path_start_node": node, "arrival": copy.deepcopy(zeros),
            "slew": copy.deepcopy(zeros), "crpr_node": None,
            "crpr_transition": None}


def _row_offsets(source: int = 0) -> list[int]:
    result = [0]
    entries = 0
    for row in range(24):
        if row in (source * 2, 8 + source * 2, 16 + source * 2):
            entries += 1
        result.append(entries)
    return result


def _clock_section(size: int, default_node: object) -> dict[str, object]:
    return {
        "predecessor_nodes": _soa([0] * size, [default_node] * size),
        "predecessor_arc_delays": _soa(
            [0] * size, [_number(0.0) for _ in range(size)]),
        "predecessor_transitions": _soa([0] * size, ["rise"] * size),
    }


def _equal_cost_fixture(winner: int) -> tuple[dict[str, Any],
                                               dict[str, Any]]:
    source0, source1, sink = map(_key, ("s0", "s1", "sink"))
    nodes = [source0, source1, sink]
    arcs = [{"canary_arc": "s0-sink"}, {"canary_arc": "s1-sink"}]
    graph_arcs = [
        {"key": arcs[0], "source": source0, "sink": sink,
         "eligibility": {"data_arrival": True}},
        {"key": arcs[1], "source": source1, "sink": sink,
         "eligibility": {"data_arrival": True}},
    ]
    graph = {
        "nodes": [{"key": node} for node in nodes],
        "active_arcs": graph_arcs,
        "reference_payload": {
            "analysis_order": ["max", "min"],
            "path_source_order": ["input", "register"],
            "transition_order": ["rise", "fall"],
            "ordered_starts": [source0, source1],
            "start_seeds": [_seed(source0), _seed(source1)],
        },
    }
    chosen_source = nodes[winner]
    chosen_arc = arcs[winner]
    node_presence = [0, 0, 1]
    node_defaults = [source0, source0, chosen_source]
    arc_defaults = [arcs[0], arcs[0], chosen_arc]
    nodes_timing = {
        "arrivals": _soa([1, 1, 1],
                         [_number(0.0), _number(0.0), _number(1.0)]),
        "launches": _soa([1, 1, 1], [_number(0.0)] * 3),
        "clock_names": _soa([1, 1, 1], ["clk"] * 3),
        "predecessor_nodes": _soa(node_presence, node_defaults),
        "predecessor_arcs": _soa(node_presence, arc_defaults),
    }
    path_presence = [0, 0, 1]
    paths = {
        "row_offsets": _row_offsets(),
        "clock_tags": _soa([1, 1, 1], ["clk"] * 3),
        "arrivals": _soa([1, 1, 1],
                         [_number(0.0), _number(0.0), _number(1.0)]),
        "slews": _soa([1, 1, 1], [_number(0.0)] * 3),
        "launches": _soa([1, 1, 1], [_number(0.0)] * 3),
        "start_nodes": _soa([1, 1, 1],
                            [source0, source1, chosen_source]),
        "predecessor_nodes": _soa(
            path_presence, [source0, source0, chosen_source]),
        "predecessor_arcs": _soa(
            path_presence, [arcs[0], arcs[0], chosen_arc]),
        "predecessor_arc_delays": _soa(
            path_presence, [_number(0.0), _number(0.0), _number(1.0)]),
        "predecessor_transitions": _soa(
            path_presence, ["rise", "rise", "rise"]),
        "crpr_nodes": _soa([0, 0, 0], [source0] * 3),
        "crpr_transitions": _soa([0, 0, 0], ["rise"] * 3),
    }
    io_presence = [0] * 16
    io_values = [_number(0.0) for _ in range(16)]
    for lane in (0, 8):
        io_presence[lane] = 1
        io_values[lane] = _number(1.0)
    timing = {
        "nodes": nodes_timing,
        "effective_clock": _clock_section(12, source0),
        "physical_clock": _clock_section(12, source0),
        "paths": paths,
        "arcs": {
            "delays": _soa([1, 1], [_number(1.0), _number(1.0)]),
            "input_output_delays": _soa(
                copy.deepcopy(io_presence), copy.deepcopy(io_values)),
            "graph_delays": _soa(io_presence, io_values),
        },
    }
    return graph, timing


def _expect_rejected(graph: dict[str, Any], left: dict[str, Any],
                     right: dict[str, Any], compare: Callable,
                     first_difference: Callable, require: Callable,
                     error_type: type[Exception], label: str) -> None:
    try:
        compare(graph, left, right, label, first_difference, require)
    except error_type:
        return
    raise error_type(f"{label} invalid winner was accepted")


def _run_sequential_canary(compare: Callable, first_difference: Callable,
                           require: Callable,
                           error_type: type[Exception]) -> None:
    graph, timing = _equal_cost_fixture(0)
    paths = timing["paths"]
    paths["row_offsets"] = _row_offsets(1)
    paths["crpr_nodes"] = copy.deepcopy(paths["start_nodes"])
    paths["crpr_transitions"] = _soa([1, 1, 1], ["rise"] * 3)
    for seed in graph["reference_payload"]["start_seeds"]:
        seed.update(availability="deferred_sequential", path_source="register",
                    arrival=None, slew=None, crpr_node=seed["seed_node"],
                    crpr_transition="rise")
    physical = timing["physical_clock"]
    for field in ("predecessor_nodes", "predecessor_arc_delays",
                  "predecessor_transitions"):
        physical[field]["presence"][8] = 1
    physical["predecessor_nodes"]["values"][8] = _key("s0")
    physical["predecessor_arc_delays"]["values"][8] = _number(1.0)
    compare(graph, timing, copy.deepcopy(timing), "sequential-canary",
            first_difference, require)

    bad = copy.deepcopy(timing)
    bad["physical_clock"]["predecessor_nodes"]["values"][8] = _key("s1")
    _expect_rejected(graph, timing, bad, compare, first_difference,
                     require, error_type, "physical-clock-winner-tamper")
    bad = copy.deepcopy(timing)
    bad["effective_clock"] = copy.deepcopy(physical)
    _expect_rejected(graph, bad, bad, compare, first_difference,
                     require, error_type, "effective-clock-predecessor")
    for field, value in (("start_nodes", _key("s1")),
                         ("crpr_nodes", _key("s1")),
                         ("launches", _number(1.0))):
        bad = copy.deepcopy(timing)
        bad["paths"][field]["values"][0] = value
        _expect_rejected(graph, bad, bad, compare, first_difference,
                         require, error_type, f"deferred-{field}-tamper")
    bad_graph = copy.deepcopy(graph)
    bad_graph["reference_payload"]["start_seeds"][0]["path_source"] = "input"
    _expect_rejected(bad_graph, timing, timing, compare, first_difference,
                     require, error_type, "deferred-source-tamper")
    for field in ("arrivals", "slews"):
        bad = copy.deepcopy(timing)
        bad["paths"][field]["values"][0] = _number(0.1)
        _expect_rejected(graph, timing, bad, compare, first_difference,
                         require, error_type, f"deferred-{field}-tamper")



def _run_interleaved_arc_canary(compare: Callable, first_difference: Callable,
                                require: Callable,
                                error_type: type[Exception]) -> None:
    graph, timing = _equal_cost_fixture(1)
    graph["active_arcs"].insert(1, {
        "key": {"canary_arc": "s0-s1"}, "source": _key("s0"),
        "sink": _key("s1"), "eligibility": {"data_arrival": True},
    })
    arcs = timing["arcs"]
    arcs["delays"] = _soa([1, 1, 1],
                          [_number(1.0), _number(-2.0), _number(1.0)])
    for field in ("input_output_delays", "graph_delays"):
        arcs[field]["presence"][8:8] = [1] + [0] * 7
        arcs[field]["values"][8:8] = [_number(-2.0)] * 8
    # First incoming arc uses graph delay; the final global arc uses IO fallback.
    arcs["input_output_delays"]["values"][0] = _number(9.0)
    arcs["graph_delays"]["presence"][16] = 0
    compare(graph, timing, copy.deepcopy(timing), "interleaved-global-arcs",
            first_difference, require)
    bad = copy.deepcopy(timing)
    bad["arcs"]["graph_delays"]["presence"][23] = 2
    _expect_rejected(graph, bad, bad, compare, first_difference,
                     require, error_type, "unvisited-delay-lane-presence")


def run_equal_cost_canary(compare: Callable, first_difference: Callable,
                          require: Callable,
                          error_type: type[Exception]) -> dict[str, int]:
    graph, left = _equal_cost_fixture(0)
    _, right = _equal_cost_fixture(1)
    evidence = compare(graph, left, right, "equal-cost-canary",
                       first_difference, require)
    require(evidence["equal_cost_node_sites"] >= 1
            and evidence["equal_cost_path_sites"] >= 1,
            "equal-cost canary did not derive both candidate sets")
    bad_node = copy.deepcopy(right)
    bad_node["nodes"]["predecessor_nodes"]["values"][2] = _key("s0")
    _expect_rejected(graph, left, bad_node, compare, first_difference,
                     require, error_type, "node-endpoint-tamper")
    bad_path = copy.deepcopy(right)
    bad_path["paths"]["predecessor_nodes"]["values"][2] = _key("s0")
    _expect_rejected(graph, left, bad_path, compare, first_difference,
                     require, error_type, "path-endpoint-tamper")
    _run_sequential_canary(compare, first_difference, require, error_type)
    _run_interleaved_arc_canary(compare, first_difference, require, error_type)
    return evidence
