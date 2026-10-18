#!/usr/bin/env python3
"""Validation of historical phase timings."""

import math
from collections.abc import Callable
from typing import Any

Require = Callable[[bool, str], None]

PHASE_TIMING_KEYS = {
    "generation_before", "generation_after", "prepare_batch_ms",
    "direct_prepare_ms",
    "dual_csr_update_ms", "full_levelization_ms", "full_propagation_ms",
    "mixed_build_ms", "mixed_kahn_ms", "reconstruct_ms",
    "invariant_ms", "verify_ms", "publish_ms",
    "session_step_ms", "result_consumption", "query_materialization_ms",
    "report_ms", "consumer_step_ms",
}
DURATION_KEYS = PHASE_TIMING_KEYS - {"generation_before", "generation_after", "result_consumption"}


def _object(value: object, keys: set[str], label: str,
            require: Require) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    if not isinstance(value, dict):
        return {}
    require(set(value) == keys, f"{label} fields differ: {sorted(value)}")
    return value




def duration_ms(value: object, label: str, require: Require, *,
                positive: bool = False) -> float:
    valid = (not isinstance(value, bool) and isinstance(value, (int, float))
             and math.isfinite(float(value)) and float(value) >= 0.0)
    require(valid, f"{label} must be a finite nonnegative duration")
    result = float(value) if valid else 0.0
    if positive:
        require(result > 0.0, f"{label} must be positive")
    return result


def validate_batch_phase_timings(
    value: object, generation: int, label: str, require: Require, *, debug_checks: bool = True,
) -> list[dict[str, Any]]:
    require(isinstance(value, list) and len(value) == generation,
            f"{label} must contain one timing record per batch")
    if not isinstance(value, list):
        return []
    for index, raw in enumerate(value):
        item = _object(raw, PHASE_TIMING_KEYS, f"{label}[{index}]", require)
        require(item.get("generation_before") == index
                and item.get("generation_after") == index + 1,
                f"{label}[{index}] generation chain differs")
        consumption = item["result_consumption"]
        require(consumption in {"completed", "not_run"},
                f"{label}[{index}] has an unknown consumption status")
        durations = {}
        for key in DURATION_KEYS:
            if consumption == "not_run" and key in {"query_materialization_ms", "report_ms"}:
                require(item[key] is None, f"{label}[{index}].{key} must be null when not_run")
                durations[key] = 0.0
            elif not debug_checks and key in {"invariant_ms", "verify_ms"}:
                require(item.get(key) == "not_run",
                        f"{label}[{index}].{key} must be not_run in performance output")
            else:
                durations[key] = duration_ms(item.get(key), f"{label}[{index}].{key}", require)
        require(durations["consumer_step_ms"] >= durations["session_step_ms"]
                + durations["query_materialization_ms"] + durations["report_ms"],
                f"{label}[{index}] consumer interval omits a completed phase")
    return value
