#!/usr/bin/env python3
"""Graph-derived equal-cost winner equivalence for Stage 6 receipts."""

import copy
import json
import math
from collections.abc import Callable
from typing import Any

SCALAR_WINNER_EPSILON = 1.0e-9
PATH_WINNER_EPSILON = 1.0e-6
VALUE_TOLERANCE = 1.0e-9
PATH_ROUND_SCALE = 1.0e15

SEMANTIC_FIELDS = {
    "nodes": ("predecessor_nodes", "predecessor_arcs"),
    "effective_clock": ("predecessor_nodes",),
    "paths": ("start_nodes", "predecessor_nodes",
              "predecessor_arcs", "crpr_nodes"),
}


def canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def regular_timing_view(timing: dict[str, Any], require: Callable,
                        label: str) -> dict[str, Any]:
    """Keep semantic-field presence, but erase traversal-order choices."""
    result = copy.deepcopy(timing)
    for section, fields in SEMANTIC_FIELDS.items():
        require(isinstance(result.get(section), dict),
                f"{label}.{section} must be an object")
        for field in fields:
            soa = result[section].get(field)
            require(isinstance(soa, dict)
                    and set(soa) == {"presence", "values"}
                    and isinstance(soa["presence"], list)
                    and isinstance(soa["values"], list)
                    and len(soa["presence"]) == len(soa["values"]),
                    f"{label}.{section}.{field} is not an OptionalSoA")
            soa["values"] = [None] * len(soa["values"])
    return result


def _soa(section: dict[str, Any], field: str, size: int,
         require: Callable, label: str) -> dict[str, list[Any]]:
    value = section.get(field)
    require(isinstance(value, dict)
            and set(value) == {"presence", "values"}
            and isinstance(value["presence"], list)
            and isinstance(value["values"], list)
            and len(value["presence"]) == size
            and len(value["values"]) == size
            and all(item in (0, 1) and type(item) is int
                    for item in value["presence"]),
            f"{label}.{field} shape/presence is malformed")
    return value


def _finite(value: object) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        result = float(value)
        return result if math.isfinite(result) else None
    if not isinstance(value, dict) or value.get("kind") != "finite":
        return None
    raw = value.get("value")
    if isinstance(raw, bool) or not isinstance(raw, (int, float)):
        return None
    result = float(raw)
    return result if math.isfinite(result) else None


def _number(soa: dict[str, list[Any]], index: int,
            require: Callable, label: str) -> float:
    require(soa["presence"][index] == 1,
            f"{label}[{index}] is absent")
    result = _finite(soa["values"][index])
    require(result is not None, f"{label}[{index}] is not finite")
    return result


def _close(left: float, right: float,
           tolerance: float = VALUE_TOLERANCE) -> bool:
    return math.isclose(left, right, rel_tol=VALUE_TOLERANCE,
                        abs_tol=tolerance)


def _graph_index(graph: dict[str, Any], require: Callable, label: str
                 ) -> tuple[list[object], list[dict[str, Any]],
                            dict[str, int], dict[str, int]]:
    raw_nodes = graph.get("nodes")
    raw_arcs = graph.get("active_arcs")
    require(isinstance(raw_nodes, list) and isinstance(raw_arcs, list),
            f"{label} nodes/arcs must be arrays")
    nodes: list[object] = []
    node_index: dict[str, int] = {}
    for index, raw in enumerate(raw_nodes):
        require(isinstance(raw, dict) and isinstance(raw.get("key"), dict),
                f"{label}.nodes[{index}].key is malformed")
        encoded = canonical(raw["key"])
        require(encoded not in node_index,
                f"{label}.nodes repeats a NodeKey")
        node_index[encoded] = index
        nodes.append(raw["key"])
    arcs: list[dict[str, Any]] = []
    arc_index: dict[str, int] = {}
    for index, raw in enumerate(raw_arcs):
        require(isinstance(raw, dict) and isinstance(raw.get("key"), dict)
                and isinstance(raw.get("source"), dict)
                and isinstance(raw.get("sink"), dict)
                and isinstance(raw.get("eligibility"), dict),
                f"{label}.active_arcs[{index}] is malformed")
        encoded = canonical(raw["key"])
        require(encoded not in arc_index
                and canonical(raw["source"]) in node_index
                and canonical(raw["sink"]) in node_index,
                f"{label}.active_arcs[{index}] key/endpoints are invalid")
        arc_index[encoded] = index
        arcs.append(raw)
    return nodes, arcs, node_index, arc_index


def _admissible(costs: list[tuple[float, str]], analysis: int,
                epsilon: float) -> list[str]:
    if not costs:
        return []
    optimum = (max if analysis == 0 else min)(cost for cost, _ in costs)
    if analysis == 0:
        return sorted(key for cost, key in costs
                      if cost >= optimum - epsilon)
    return sorted(key for cost, key in costs
                  if cost <= optimum + epsilon)


def _validate_node_winners(graph: dict[str, Any], timing: dict[str, Any],
                           require: Callable, label: str
                           ) -> dict[str, list[str]]:
    nodes, arcs, node_index, arc_index = _graph_index(
        graph, require, f"{label}.graph")
    count = len(nodes)
    section = timing["nodes"]
    arrivals = _soa(section, "arrivals", count, require, label)
    launches = _soa(section, "launches", count, require, label)
    clock_names = _soa(section, "clock_names", count, require, label)
    predecessor_nodes = _soa(
        section, "predecessor_nodes", count, require, label)
    predecessor_arcs = _soa(
        section, "predecessor_arcs", count, require, label)
    require(predecessor_nodes["presence"]
            == predecessor_arcs["presence"],
            f"{label}.nodes predecessor presence differs")
    delays = _soa(timing["arcs"], "delays", len(arcs), require,
                  f"{label}.arcs")
    starts = {canonical(value) for value in
              graph["reference_payload"].get("ordered_starts", [])}
    result: dict[str, list[str]] = {}
    for sink, sink_key in enumerate(nodes):
        costs: list[tuple[float, str]] = []
        selected_costs: dict[str, float] = {}
        for arc_id, arc in enumerate(arcs):
            if arc["eligibility"].get("data_arrival") is not True \
                    or canonical(arc["sink"]) != canonical(sink_key):
                continue
            source = node_index[canonical(arc["source"])]
            source_arrival = _finite(arrivals["values"][source]) \
                if arrivals["presence"][source] == 1 else None
            delay = _finite(delays["values"][arc_id]) \
                if delays["presence"][arc_id] == 1 else None
            if source_arrival is None or delay is None:
                continue
            candidate = source_arrival + delay
            if not math.isfinite(candidate):
                continue
            key = canonical({"node": arc["source"], "arc": arc["key"]})
            costs.append((candidate, key))
            selected_costs[key] = candidate
        admissible = _admissible(costs, 0, SCALAR_WINNER_EPSILON)
        result[f"node:{canonical(sink_key)}"] = admissible
        if predecessor_nodes["presence"][sink] == 0:
            arrival = _finite(arrivals["values"][sink]) \
                if arrivals["presence"][sink] == 1 else None
            require(arrival is None or canonical(sink_key) in starts,
                    f"{label}.nodes[{sink}] finite non-start lacks winner")
            continue
        node_key = predecessor_nodes["values"][sink]
        arc_key = predecessor_arcs["values"][sink]
        chosen = canonical({"node": node_key, "arc": arc_key})
        require(chosen in admissible and canonical(arc_key) in arc_index,
                f"{label}.nodes[{sink}] winner is not admissible")
        arc = arcs[arc_index[canonical(arc_key)]]
        source = node_index.get(canonical(node_key))
        require(source is not None
                and canonical(arc["source"]) == canonical(node_key)
                and canonical(arc["sink"]) == canonical(sink_key),
                f"{label}.nodes[{sink}] winner endpoints differ")
        sink_arrival = _number(
            arrivals, sink, require, f"{label}.nodes.arrivals")
        require(_close(selected_costs[chosen], sink_arrival),
                f"{label}.nodes[{sink}] winner recurrence differs")
        require(_close(_number(launches, source, require,
                               f"{label}.nodes.launches"),
                       _number(launches, sink, require,
                               f"{label}.nodes.launches")),
                f"{label}.nodes[{sink}] winner launch differs")
        require(clock_names["presence"][source]
                == clock_names["presence"][sink]
                and (clock_names["presence"][sink] == 0
                     or clock_names["values"][source]
                        == clock_names["values"][sink]),
                f"{label}.nodes[{sink}] winner clock name differs")
    return result


def _round_path(value: float) -> float:
    scaled = value * PATH_ROUND_SCALE
    rounded = math.floor(scaled + 0.5) if scaled >= 0.0 \
        else math.ceil(scaled - 0.5)
    return rounded / PATH_ROUND_SCALE


def _selected_path_delay(arcs: dict[str, Any], arc: int, analysis: int,
                         input_transition: int, output_transition: int,
                         require: Callable, label: str) -> float | None:
    count = len(arcs["delays"]["presence"])
    io_lane = ((arc * 2 + analysis) * 2 + input_transition) * 2 \
        + output_transition
    io = _soa(arcs, "input_output_delays", count * 8,
              require, label)
    if io["presence"][io_lane] == 0:
        return None
    graph = _soa(arcs, "graph_delays", count * 8, require, label)
    if graph["presence"][io_lane] == 1:
        return _number(graph, io_lane, require, f"{label}.graph_delays")
    return _number(io, io_lane, require,
                   f"{label}.input_output_delays")


def _root_seed_matches(graph: dict[str, Any], nodes: list[object],
                       coordinates: tuple[int, int, int, int], entry: int,
                       paths: dict[str, Any], require: Callable,
                       label: str) -> bool:
    node, analysis, source, transition = coordinates
    payload = graph["reference_payload"]
    sources = payload.get("path_source_order")
    if not isinstance(sources, list) or source >= len(sources):
        return False
    tag = paths["clock_tags"]["values"][entry]
    start = canonical(paths["start_nodes"]["values"][entry])
    arrival = _number(paths["arrivals"], entry, require,
                      f"{label}.paths.arrivals")
    slew = _number(paths["slews"], entry, require,
                   f"{label}.paths.slews")
    matches = 0
    for seed in payload.get("start_seeds", []):
        if not isinstance(seed, dict) \
                or canonical(seed.get("seed_node")) != canonical(nodes[node]) \
                or seed.get("path_source") != sources[source] \
                or seed.get("clock_tag") != tag \
                or canonical(seed.get("path_start_node")) != start:
            continue
        if seed.get("availability") == "deferred_sequential":
            if sources[source] != "register" or _number(
                    paths["launches"], entry, require,
                    f"{label}.paths.launches") != 0.0 \
                    or seed.get("crpr_node") is None \
                    or seed.get("crpr_transition") not in ("rise", "fall"):
                continue
        elif seed.get("availability") == "materialized":
            raw_arrival = seed.get("arrival")
            raw_slew = seed.get("slew")
            try:
                seed_arrival = _finite(raw_arrival[analysis][transition])
                seed_slew = _finite(raw_slew[analysis][transition])
            except (IndexError, TypeError):
                continue
            if seed_arrival is None or seed_slew is None \
                    or not _close(seed_arrival, arrival) \
                    or not _close(seed_slew, slew):
                continue
        else:
            continue
        crpr = seed.get("crpr_node")
        present = paths["crpr_nodes"]["presence"][entry]
        if (crpr is None) != (present == 0):
            continue
        if crpr is not None and (
                canonical(crpr) != canonical(
                    paths["crpr_nodes"]["values"][entry])
                or seed.get("crpr_transition") != paths[
                    "crpr_transitions"]["values"][entry]):
            continue
        matches += 1
    return matches == 1


def _validate_path_winners(graph: dict[str, Any], timing: dict[str, Any],
                           require: Callable, label: str
                           ) -> dict[str, list[str]]:
    nodes, active_arcs, node_index, arc_index = _graph_index(
        graph, require, f"{label}.graph")
    rows = len(nodes) * 8
    paths = timing["paths"]
    offsets = paths.get("row_offsets")
    require(isinstance(offsets, list) and len(offsets) == rows + 1
            and offsets[0] == 0
            and all(type(item) is int and item >= 0 for item in offsets)
            and all(left <= right for left, right
                    in zip(offsets, offsets[1:], strict=False)),
            f"{label}.paths.row_offsets is malformed")
    entries = offsets[-1]
    for field in ("clock_tags", "arrivals", "slews", "launches",
                  "start_nodes", "predecessor_nodes", "predecessor_arcs",
                  "predecessor_arc_delays", "predecessor_transitions",
                  "crpr_nodes", "crpr_transitions"):
        _soa(paths, field, entries, require, f"{label}.paths")
    for field in ("clock_tags", "arrivals", "slews", "launches",
                  "start_nodes"):
        require(set(paths[field]["presence"]) <= {1},
                f"{label}.paths.{field} contains an absent entry")
    predecessor_presence = paths["predecessor_nodes"]["presence"]
    require(predecessor_presence == paths["predecessor_arcs"]["presence"]
            == paths["predecessor_arc_delays"]["presence"]
            == paths["predecessor_transitions"]["presence"],
            f"{label}.paths predecessor presence differs")
    require(paths["crpr_nodes"]["presence"]
            == paths["crpr_transitions"]["presence"],
            f"{label}.paths CRPR presence differs")
    entry_rows = [0] * entries
    by_row_tag: dict[tuple[int, str], int] = {}
    for row in range(rows):
        for entry in range(offsets[row], offsets[row + 1]):
            tag = paths["clock_tags"]["values"][entry]
            require(isinstance(tag, str)
                    and (row, tag) not in by_row_tag,
                    f"{label}.paths row repeats a clock tag")
            by_row_tag[(row, tag)] = entry
            entry_rows[entry] = row
    arc_section = timing["arcs"]
    _soa(arc_section, "delays", len(active_arcs), require,
         f"{label}.arcs")
    result: dict[str, list[str]] = {}
    for entry, row in enumerate(entry_rows):
        transition = row % 2
        rest = row // 2
        source_kind = rest % 2
        rest //= 2
        analysis = rest % 2
        sink = rest // 2
        tag = paths["clock_tags"]["values"][entry]
        costs: list[tuple[float, str]] = []
        evidence: dict[str, tuple[float, float, int]] = {}
        for arc_id, arc in enumerate(active_arcs):
            if arc["eligibility"].get("data_arrival") is not True \
                    or canonical(arc["sink"]) != canonical(nodes[sink]):
                continue
            predecessor = node_index[canonical(arc["source"])]
            for predecessor_transition in range(2):
                delay = _selected_path_delay(
                    arc_section, arc_id, analysis, predecessor_transition,
                    transition, require, f"{label}.arcs")
                if delay is None:
                    continue
                upstream_row = (((predecessor * 2 + analysis) * 2
                                 + source_kind) * 2
                                + predecessor_transition)
                upstream = by_row_tag.get((upstream_row, tag))
                if upstream is None:
                    continue
                source_arrival = _number(
                    paths["arrivals"], upstream, require,
                    f"{label}.paths.arrivals")
                candidate = _round_path(source_arrival + delay)
                key = canonical({"node": arc["source"], "arc": arc["key"],
                                 "transition": ("rise", "fall")[
                                     predecessor_transition]})
                costs.append((candidate, key))
                evidence[key] = (candidate, delay, upstream)
        admissible = _admissible(costs, analysis, PATH_WINNER_EPSILON)
        site = f"path:{row}:{canonical(tag)}"
        result[site] = admissible
        if predecessor_presence[entry] == 0:
            require(_root_seed_matches(
                        graph, nodes,
                        (sink, analysis, source_kind, transition), entry,
                        paths, require, label),
                    f"{label}.paths[{entry}] is not an exact root seed")
            continue
        predecessor_key = paths["predecessor_nodes"]["values"][entry]
        predecessor_arc = paths["predecessor_arcs"]["values"][entry]
        predecessor_transition = paths["predecessor_transitions"][
            "values"][entry]
        chosen = canonical({"node": predecessor_key,
                            "arc": predecessor_arc,
                            "transition": predecessor_transition})
        require(chosen in admissible
                and canonical(predecessor_arc) in arc_index,
                f"{label}.paths[{entry}] winner is not admissible")
        candidate, delay, upstream = evidence[chosen]
        require(_close(candidate, _number(
                    paths["arrivals"], entry, require,
                    f"{label}.paths.arrivals"))
                and _close(delay, _number(
                    paths["predecessor_arc_delays"], entry, require,
                    f"{label}.paths.predecessor_arc_delays")),
                f"{label}.paths[{entry}] winner recurrence differs")
        require(canonical(paths["start_nodes"]["values"][entry])
                == canonical(paths["start_nodes"]["values"][upstream])
                and _close(_number(paths["launches"], entry, require,
                                   f"{label}.paths.launches"),
                           _number(paths["launches"], upstream, require,
                                   f"{label}.paths.launches")),
                f"{label}.paths[{entry}] winner lineage differs")
        require(paths["crpr_nodes"]["presence"][entry]
                == paths["crpr_nodes"]["presence"][upstream]
                and (paths["crpr_nodes"]["presence"][entry] == 0
                     or (canonical(paths["crpr_nodes"]["values"][entry])
                         == canonical(paths["crpr_nodes"]["values"][upstream])
                         and paths["crpr_transitions"]["values"][entry]
                         == paths["crpr_transitions"]["values"][upstream])),
                f"{label}.paths[{entry}] winner CRPR lineage differs")
    return result


def _validate_ideal_clock_winners(timing: dict[str, Any], node_count: int,
                                  require: Callable, label: str) -> None:
    section = timing["effective_clock"]
    label = f"{label}.effective_clock"
    nodes = _soa(section, "predecessor_nodes", node_count * 4, require, label)
    delays = _soa(section, "predecessor_arc_delays", node_count * 4,
                  require, label)
    transitions = _soa(section, "predecessor_transitions", node_count * 4,
                       require, label)
    require(nodes["presence"] == delays["presence"]
            == transitions["presence"] and not any(nodes["presence"]),
            f"{label} must use isolated ideal-clock roots")



def admissible_winner_sets(graph: dict[str, Any], timing: dict[str, Any],
                           require: Callable, label: str
                           ) -> dict[str, Any]:
    nodes = graph.get("nodes")
    require(isinstance(nodes, list), f"{label}.graph.nodes is malformed")
    _validate_ideal_clock_winners(
        timing, len(nodes), require, f"{label}.timing")
    node_sets = _validate_node_winners(
        graph, timing, require, f"{label}.timing")
    path_sets = _validate_path_winners(
        graph, timing, require, f"{label}.timing")
    return {"nodes": node_sets, "paths": path_sets}


def compare_timing_with_admissible_winners(
        graph: dict[str, Any], targeted: dict[str, Any],
        fresh: dict[str, Any], label: str, first_difference: Callable,
        require: Callable) -> dict[str, int]:
    target_values = regular_timing_view(
        targeted, require, f"{label}.targeted")
    fresh_values = regular_timing_view(
        fresh, require, f"{label}.fresh")
    difference = first_difference(
        target_values, fresh_values, f"{label}.numeric_and_presence")
    require(difference is None,
            f"{label} numeric/presence mismatch: {difference}")
    target_sets = admissible_winner_sets(
        graph, targeted, require, f"{label}.targeted")
    fresh_sets = admissible_winner_sets(
        graph, fresh, require, f"{label}.fresh")
    difference = first_difference(
        target_sets, fresh_sets, f"{label}.admissible_winner_sets")
    require(difference is None,
            f"{label} admissible winner-set mismatch: {difference}")
    return {
        "node_winner_sites": len(target_sets["nodes"]),
        "path_winner_sites": len(target_sets["paths"]),
        "equal_cost_node_sites": sum(
            len(values) > 1 for values in target_sets["nodes"].values()),
        "equal_cost_path_sites": sum(
            len(values) > 1 for values in target_sets["paths"].values()),
    }
