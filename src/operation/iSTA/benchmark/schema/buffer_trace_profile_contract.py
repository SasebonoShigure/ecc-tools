"""Frozen Stage 4 profile literals shared by the Python validator."""

from collections.abc import Callable
from typing import Any


_OPERATING_POINT = {"input_slew_ns": 0.05, "output_load_pf": 0.01}
_RC_PROFILE = {
    "id": "engineering_rc_v1",
    "upstream_resistance_kohm": 64.0,
    "buffer_resistance_kohm": 1.0,
}
FROZEN_PROFILES: list[dict[str, object]] = [
    {
        "id": "paper_protocol_top1000_x1",
        "top_k": 1000,
        "rounds": 1,
        "max_total_insertions": 1000,
        "analysis": "cpu_full",
        "operating_point": dict(_OPERATING_POINT),
        "rc_profile": dict(_RC_PROFILE),
        "comparability_status": "protocol_only",
    },
    {
        "id": "paper_protocol_top100_x10",
        "top_k": 100,
        "rounds": 10,
        "max_total_insertions": 1000,
        "analysis": "cpu_full",
        "operating_point": dict(_OPERATING_POINT),
        "rc_profile": dict(_RC_PROFILE),
        "comparability_status": "protocol_only",
    },
    {
        "id": "engineering_smoke",
        "top_k": 4,
        "rounds": 2,
        "max_total_insertions": 4,
        "analysis": "cpu_full",
        "operating_point": dict(_OPERATING_POINT),
        "rc_profile": dict(_RC_PROFILE),
        "comparability_status": "non_comparable",
        "comparability_reason": "engineering_rc_no_physical",
    },
]


def _first_difference(left: Any, right: Any, path: str) -> str | None:
    if type(left) is not type(right):
        return (
            f"{path} type {type(left).__name__} differs from "
            f"{type(right).__name__}"
        )
    if isinstance(left, dict):
        if set(left) != set(right):
            return f"{path} key set differs"
        for key in sorted(left):
            difference = _first_difference(
                left[key], right[key], f"{path}.{key}")
            if difference is not None:
                return difference
        return None
    if isinstance(left, list):
        if len(left) != len(right):
            return f"{path} length differs"
        for index, (left_item, right_item) in enumerate(
            zip(left, right, strict=True)
        ):
            difference = _first_difference(
                left_item, right_item, f"{path}[{index}]")
            if difference is not None:
                return difference
        return None
    if left != right:
        return f"{path} value differs"
    return None


def validate_frozen_profiles(
    profiles: object,
    fail: Callable[[str, str], None],
) -> dict[str, dict[str, object]]:
    """Require the complete version-1 literal, not merely its profile ids."""
    difference = _first_difference(
        profiles, FROZEN_PROFILES, "manifest.profiles")
    if difference is not None:
        fail(
            "manifest.profiles",
            "must exactly equal the three frozen version-1 profiles: "
            + difference,
        )
    assert isinstance(profiles, list)
    return {
        profile["id"]: profile
        for profile in profiles
        if isinstance(profile, dict) and isinstance(profile.get("id"), str)
    }
