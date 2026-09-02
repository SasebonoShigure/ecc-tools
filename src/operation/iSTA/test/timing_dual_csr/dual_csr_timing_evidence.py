#!/usr/bin/env python3
"""Strict validation for Stage 5 phase and resource observations."""

import copy
import math
from typing import Any, Callable

Require = Callable[[bool, str], None]

PHASE_TIMING_KEYS = {
    "generation_before", "generation_after", "pre_candidate_prepare_ms",
    "candidate_apply_execute_ms", "post_candidate_capture_ms",
    "dual_csr_update_ms", "full_levelization_ms", "full_propagation_ms",
    "invariant_ms", "verify_ms", "publish_ms",
}
DURATION_KEYS = PHASE_TIMING_KEYS - {"generation_before", "generation_after"}
BUFFER_ORDER = [
    "nodes", "arc_slots", "base_out_offsets", "base_out_slots",
    "base_in_offsets", "base_in_slots", "aux_offsets", "aux_sources",
    "aux_arcs", "aux_source_map",
]
REQUESTED_STRATEGY = {
    "compute": "cpu", "update": "incremental", "fallback": "error"}
EXECUTED_STRATEGY = {
    "compute": "cpu", "update": "targeted_dual_csr", "fallback": "none",
    "propagation": "full_graph"}


def _object(value: object, keys: set[str], label: str,
            require: Require) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    if not isinstance(value, dict):
        return {}
    require(set(value) == keys, f"{label} fields differ: {sorted(value)}")
    return value


def _count(value: object, label: str, require: Require) -> int:
    require(type(value) is int and value >= 0,
            f"{label} must be a nonnegative integer")
    return value if type(value) is int else 0


def duration_ms(value: object, label: str, require: Require,
                positive: bool = False) -> float:
    valid = (not isinstance(value, bool) and isinstance(value, (int, float))
             and math.isfinite(float(value)) and float(value) >= 0.0)
    require(valid, f"{label} must be a finite nonnegative duration")
    result = float(value) if valid else 0.0
    if positive:
        require(result > 0.0, f"{label} must be positive")
    return result


def validate_batch_phase_timings(value: object, generation: int, label: str,
                                 require: Require) -> list[dict[str, Any]]:
    require(isinstance(value, list) and len(value) == generation,
            f"{label} must contain one timing record per batch")
    if not isinstance(value, list):
        return []
    for index, raw in enumerate(value):
        item = _object(raw, PHASE_TIMING_KEYS, f"{label}[{index}]", require)
        require(item.get("generation_before") == index
                and item.get("generation_after") == index + 1,
                f"{label}[{index}] generation chain differs")
        for key in DURATION_KEYS:
            duration_ms(item.get(key), f"{label}[{index}].{key}", require)
    return value


def _extent(structural: dict[str, Any], buffer: str) -> tuple[int, int]:
    mapping = {
        "base_out_offsets": ("base_out", "offsets_extent"),
        "base_out_slots": ("base_out", "edge_extent"),
        "base_in_offsets": ("base_in", "offsets_extent"),
        "base_in_slots": ("base_in", "edge_extent"),
        "aux_offsets": ("aux_out", "offsets_extent"),
        "aux_sources": ("aux_out", "source_extent"),
        "aux_arcs": ("aux_out", "edge_extent"),
        "aux_source_map": ("aux_out", "mapping_extent"),
    }
    owner, field = mapping[buffer]
    extent = structural[owner][field]
    return extent["logical_size"], extent["capacity"]


def _validate_memory(value: object, structural: dict[str, Any], label: str,
                     require: Require) -> dict[str, Any]:
    memory = _object(value, {"estimate_kind", "buffers"}, label, require)
    require(memory.get("estimate_kind") == "shallow_vector_storage",
            f"{label}.estimate_kind differs")
    buffers = memory.get("buffers")
    require(isinstance(buffers, list)
            and len(buffers) == len(BUFFER_ORDER),
            f"{label} must cover all ten state vectors")
    if not isinstance(buffers, list):
        return memory
    names: list[object] = []
    keys = {"buffer", "logical_elements", "capacity_elements",
            "element_size_bytes", "estimated_reserved_bytes"}
    for index, raw in enumerate(buffers):
        item = _object(raw, keys, f"{label}.buffers[{index}]", require)
        name = item.get("buffer")
        names.append(name)
        require(name == BUFFER_ORDER[index],
                f"{label}[{index}].buffer order differs")
        logical = _count(item.get("logical_elements"),
                         f"{label}[{index}].logical_elements", require)
        capacity = _count(item.get("capacity_elements"),
                          f"{label}[{index}].capacity_elements", require)
        element = _count(item.get("element_size_bytes"),
                         f"{label}[{index}].element_size_bytes", require)
        reserved = _count(item.get("estimated_reserved_bytes"),
                          f"{label}[{index}].estimated_reserved_bytes", require)
        require(capacity >= logical and element > 0
                and reserved == capacity * element,
                f"{label}[{index}] shallow storage estimate differs")
        if name == "nodes":
            require(logical == structural["high_water"]["node_slots"],
                    f"{label}[{index}] node logical extent differs")
        elif name == "arc_slots":
            require(logical == structural["high_water"]["arc_slots"],
                    f"{label}[{index}] arc logical extent differs")
        elif name in BUFFER_ORDER[2:]:
            expected_logical, expected_capacity = _extent(structural, name)
            require((logical, capacity)
                    == (expected_logical, expected_capacity),
                    f"{label}[{index}] serialized extent differs")
    require(names == BUFFER_ORDER, f"{label} buffer coverage/order differs")
    return memory


def validate_resource_observations(
        value: object, observations: list[dict[str, Any]],
        transaction: dict[str, Any], generation: int, label: str,
        require: Require) -> list[dict[str, Any]]:
    require(isinstance(value, list) and len(value) == generation,
            f"{label} must contain one resource record per batch")
    if not isinstance(value, list):
        return []
    evidence = transaction["evidence"]
    counters = evidence["operation_counters"]
    require(evidence["incremental_batch_executed"] is True
            and evidence["propagation_scope"] == "full_graph"
            and counters["native_full_graph_exports"] == 0
            and counters["native_full_reference_exports"] == 0
            and counters["host_snapshot_rebuilds"] == 0
            and counters["fallback_calls"] == 0,
            f"{label} execution evidence does not prove the declared strategy")
    keys = {"generation", "tombstone_ratio", "requested_strategy",
            "executed_strategy", "vector_memory"}
    for index, raw in enumerate(value):
        item = _object(raw, keys, f"{label}[{index}]", require)
        structural = observations[index]["structural"]
        telemetry = structural["update_telemetry"]
        require(item.get("generation") == index + 1
                == structural["generation"],
                f"{label}[{index}] generation differs")
        physical = telemetry["base_physical_edges"]
        expected_ratio = (0.0 if physical == 0
                          else telemetry["base_tombstones"] / physical)
        ratio = item.get("tombstone_ratio")
        valid_ratio = (not isinstance(ratio, bool)
                       and isinstance(ratio, (int, float))
                       and math.isfinite(float(ratio)))
        require(valid_ratio and math.isclose(float(ratio), expected_ratio,
                                             rel_tol=0.0, abs_tol=1.0e-15),
                f"{label}[{index}].tombstone_ratio differs")
        require(item.get("requested_strategy") == REQUESTED_STRATEGY,
                f"{label}[{index}].requested_strategy differs")
        require(item.get("executed_strategy") == EXECUTED_STRATEGY
                and telemetry["full_rebuild_count"] == 0
                and telemetry["fallback_count"] == 0,
                f"{label}[{index}].executed_strategy differs")
        _validate_memory(item.get("vector_memory"), structural,
                         f"{label}[{index}].vector_memory", require)
    return value


def run_timing_evidence_self_test(require: Require) -> None:
    valid = [{"generation_before": 0, "generation_after": 1,
              **{key: 0.0 for key in DURATION_KEYS}}]
    errors: list[str] = []
    validate_batch_phase_timings(
        valid, 1, "phase-timing-canary",
        lambda ok, message: errors.append(message) if not ok else None)
    require(not errors, f"valid phase timings were rejected: {errors}")
    missing = copy.deepcopy(valid)
    del missing[0]["verify_ms"]
    errors = []
    validate_batch_phase_timings(
        missing, 1, "phase-key-tamper",
        lambda ok, message: errors.append(message) if not ok else None)
    require(errors, "missing independent verify_ms was accepted")
    negative = copy.deepcopy(valid)
    negative[0]["invariant_ms"] = -1.0
    errors = []
    validate_batch_phase_timings(
        negative, 1, "phase-negative-tamper",
        lambda ok, message: errors.append(message) if not ok else None)
    require(errors, "negative invariant_ms was accepted")
