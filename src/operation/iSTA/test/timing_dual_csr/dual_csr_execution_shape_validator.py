#!/usr/bin/env python3
"""Structural validators for per-generation Dual-CSR execution evidence."""

from typing import Any, Callable

Require = Callable[[bool, str], None]


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


def validate_execution_levels(value: object, label: str,
                              require: Require) -> int:
    item = _object(value, {"complete", "level_offsets", "level_nodes",
                          "node_to_level", "view_error"}, label, require)
    require(item["complete"] is True and item["view_error"] is None,
            f"{label} must be complete and view-error-free")
    nodes = _array(item["level_nodes"], f"{label}.level_nodes", require)
    offsets = _array(item["level_offsets"], f"{label}.level_offsets", require)
    inverse = _array(item["node_to_level"], f"{label}.node_to_level", require)
    node_count = len(nodes)
    require(all(type(node) is int and node >= 0 for node in nodes)
            and set(nodes) == set(range(node_count)),
            f"{label}.level_nodes is not a dense complete permutation")
    require(len(inverse) == node_count
            and all(type(level) is int and level >= 1 for level in inverse),
            f"{label}.node_to_level is malformed")
    offsets_valid = (bool(offsets) and offsets[0] == 0
                     and offsets[-1] == node_count
                     and all(type(offset) is int and 0 <= offset <= node_count
                             for offset in offsets))
    if node_count == 0:
        offsets_valid = offsets == [0]
    else:
        offsets_valid = (offsets_valid and len(offsets) >= 2
                         and all(left < right for left, right
                                 in zip(offsets, offsets[1:])))
    require(offsets_valid, f"{label}.level_offsets is malformed")
    if offsets_valid and len(inverse) == node_count:
        for level, (begin, end) in enumerate(
                zip(offsets, offsets[1:]), start=1):
            for position in range(begin, end):
                require(inverse[nodes[position]] == level,
                        f"{label}.node_to_level is not the exact inverse")
    return node_count


def validate_mixed_execution(value: object, expected_last_nodes: int,
                             node_count: int, expected_new_nodes: int,
                             level_count: int, label: str,
                             require: Require) -> None:
    keys = {"n_last_nodes", "n_current_nodes", "n_new_nodes", "n_old_levels",
            "mixed_vertex_count", "actual_eligible_arc_visits", "old_old",
            "old_new", "new_old", "new_new",
            "mapped_actual_arc_candidates", "unique_actual_endpoint_pairs",
            "actual_parallel_duplicates_collapsed",
            "proxy_alias_duplicates_collapsed", "unique_precedence_edges",
            "virtual_chain_edges", "base_out", "aux_out",
            "kahn_processed_count", "max_frontier",
            "reconstructed_level_count", "actual_cycle_classifier_runs",
            "actual_cycle_classifier_arc_visits", "fallback_count"}
    item = _object(value, keys, label, require)
    for key in keys:
        _count(item[key], f"{label}.{key}", require)
    require(item["n_current_nodes"] == node_count
            and item["n_last_nodes"] == expected_last_nodes
            and item["n_new_nodes"] == expected_new_nodes
            and expected_last_nodes + expected_new_nodes == node_count,
            f"{label} descriptor-bound node boundaries differ")
    require(item["n_old_levels"] + item["n_new_nodes"]
            == item["mixed_vertex_count"],
            f"{label}.mixed_vertex_count differs")
    require(item["old_old"] + item["old_new"] + item["new_old"]
            + item["new_new"] == item["actual_eligible_arc_visits"],
            f"{label} edge-class counts differ")
    require(item["old_new"] + item["new_old"] + item["new_new"]
            == item["mapped_actual_arc_candidates"],
            f"{label}.mapped_actual_arc_candidates differs")
    require(item["actual_parallel_duplicates_collapsed"]
            <= item["mapped_actual_arc_candidates"],
            f"{label} parallel duplicate count exceeds candidates")
    require(item["unique_actual_endpoint_pairs"]
            == item["mapped_actual_arc_candidates"]
               - item["actual_parallel_duplicates_collapsed"],
            f"{label}.unique_actual_endpoint_pairs differs")
    require(item["proxy_alias_duplicates_collapsed"]
            <= item["unique_actual_endpoint_pairs"],
            f"{label} proxy alias count exceeds endpoint pairs")
    require(item["unique_precedence_edges"]
            == item["virtual_chain_edges"]
               + item["unique_actual_endpoint_pairs"]
               - item["proxy_alias_duplicates_collapsed"],
            f"{label}.unique_precedence_edges differs")
    require(item["virtual_chain_edges"] == max(item["n_old_levels"] - 1, 0),
            f"{label}.virtual_chain_edges differs")
    require(item["base_out"] + item["aux_out"]
            == item["actual_eligible_arc_visits"],
            f"{label} storage visit counts differ")
    frontier_valid = ((item["mixed_vertex_count"] == 0
                       and item["max_frontier"] == 0)
                      or 0 < item["max_frontier"]
                         <= item["mixed_vertex_count"])
    require(frontier_valid
            and item["kahn_processed_count"] == item["mixed_vertex_count"]
            and item["reconstructed_level_count"] == level_count
            and item["actual_cycle_classifier_runs"] == 0
            and item["actual_cycle_classifier_arc_visits"] == 0
            and item["fallback_count"] == 0,
            f"{label} successful Kahn/reconstruction evidence differs")
