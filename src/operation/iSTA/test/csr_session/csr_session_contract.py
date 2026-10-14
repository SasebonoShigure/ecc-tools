#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# ***************************************************************************************
"""Strict fresh-process session probe, transition, and compatibility contracts."""

import copy
import math
import re
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

_TEST_ROOT = Path(__file__).resolve().parent.parent
_BASELINE_ROOT = _TEST_ROOT / "baseline"
_PROJECTION_ROOT = _TEST_ROOT / "projection"
for _import_root in (_BASELINE_ROOT, _PROJECTION_ROOT):
    if str(_import_root) not in sys.path:
        sys.path.insert(0, str(_import_root))

from baseline_contract import (  # noqa: E402
    BaselineError,
    load_json,
    require_exact_keys,
    require_list,
    require_mapping,
    validate_repeat_count,
)
from projection_canonical import decode_oracle, decode_projection  # noqa: E402
from projection_contract import CaseDescriptor, decode_lower_hex  # noqa: E402

PROBE_SCHEMA = "ista-csr-session-probe/1.0.0"
CONTROLLER_SCHEMA = "ista-csr-session-controller/2.0.0"
DEFAULT_COMPAT_SCHEMA = "ista-csr-projection-default-compat/2.0.0"
UNSUPPORTED_REASON_TOKENS = (
    "parasitic_net",
    "sequential_instance",
    "sequential_launch",
    "timing_check",
    "crpr",
    "propagated_clock",
    "clock_tree_fanout",
    "clock_gating_or_divider",
    "conditional_timing_arc",
    "case_analysis",
)

PROBE_KEYS = {
    "schema",
    "run_nonce",
    "case",
    "repeat_index",
    "full_observation",
    "sessions",
    "complete",
}
SESSION_KEYS = {"session_token", "active", "released", "oracle"}
ORACLE_KEYS = {
    "source_identity",
    "node_count",
    "edge_count",
    "level_count",
    "projection_wire_hex",
    "fifo_wire_hex",
}
CONTROLLER_KEYS = {
    "schema",
    "requested_repeats",
    "default_compat_results",
    "case_results",
    "complete",
}
DEFAULT_RESULT_KEYS = {"case", "projection_wire_hex", "fifo_wire_hex"}
CASE_RESULT_KEYS = {"case", "repeats"}
DEFAULT_SEMANTIC_KEYS = (
    "case",
    "top",
    "expected_clock",
    "functional_projection",
    "complete",
)
OBSERVATION_KEYS = {
    "schema_version",
    "state",
    "identity",
    "counts",
    "buffers",
    "capacity",
    "support",
    "timing_ms",
    "host_bytes",
    "last_rejection",
    "last_invalidation",
}
IDENTITY_KEYS = {
    "fingerprint_schema_version",
    "session_token",
    "graph_generation",
    "topology_fingerprint",
    "clock_constraint_fingerprint",
    "library_fingerprint",
    "corner_fingerprint",
    "parasitic_fingerprint",
    "timing_input_fingerprint",
}
CONTENT_FINGERPRINT_KEYS = (
    "topology_fingerprint",
    "clock_constraint_fingerprint",
    "library_fingerprint",
    "corner_fingerprint",
    "parasitic_fingerprint",
    "timing_input_fingerprint",
)
BUFFER_KEYS = {
    "node_keys",
    "arc_keys",
    "out_row_offsets",
    "out_arc_ids",
    "in_row_offsets",
    "in_arc_ids",
    "level_offsets",
    "level_nodes",
    "node_to_level",
}
_SHA256 = re.compile(r"[0-9a-f]{64}")
_NONCE = re.compile(r"[0-9a-f]{32}")
_U64_MAX = (1 << 64) - 1


class SessionContractError(BaselineError):
    """Fresh-process CSR session contract violation."""


def _u64(value: Any, where: str, *, positive: bool = False) -> int:
    if type(value) is not int or value < 0 or value > _U64_MAX:
        raise SessionContractError(f"{where} must be a non-bool U64")
    if positive and value == 0:
        raise SessionContractError(f"{where} must be nonzero")
    return value


def _sha256(value: Any, where: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise SessionContractError(f"{where} must be a lowercase SHA-256")
    return value


def _finite_ms(value: Any, where: str, *, nullable: bool) -> float | None:
    if value is None and nullable:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise SessionContractError(f"{where} must be a JSON number")
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise SessionContractError(f"{where} must be finite and nonnegative")
    return result


def _identity(value: Any, where: str, *, expected_token: int) -> dict[str, Any]:
    identity = require_mapping(value, where)
    require_exact_keys(identity, IDENTITY_KEYS, where)
    if identity["fingerprint_schema_version"] != "2.0.0":
        raise SessionContractError(f"{where}.fingerprint_schema_version changed")
    token = _u64(identity["session_token"], f"{where}.session_token", positive=True)
    if token != expected_token:
        raise SessionContractError(f"{where}.session_token does not match its session")
    if _u64(identity["graph_generation"], f"{where}.graph_generation") != 0:
        raise SessionContractError(f"{where}.graph_generation must be zero")
    for field in CONTENT_FINGERPRINT_KEYS:
        _sha256(identity[field], f"{where}.{field}")
    return identity


def _extent(value: Any, where: str, expected_logical: int) -> None:
    extent = require_mapping(value, where)
    require_exact_keys(extent, {"logical_size", "capacity"}, where)
    logical = _u64(extent["logical_size"], f"{where}.logical_size")
    capacity = _u64(extent["capacity"], f"{where}.capacity")
    if logical != expected_logical or capacity < logical:
        raise SessionContractError(f"{where} has inconsistent logical size/capacity")


def _observation(
    value: Any,
    where: str,
    *,
    expected_state: str,
    expected_token: int,
) -> dict[str, Any]:
    observation = require_mapping(value, where)
    require_exact_keys(observation, OBSERVATION_KEYS, where)
    if observation["schema_version"] != "1.0.0":
        raise SessionContractError(f"{where}.schema_version changed")
    if observation["state"] != expected_state:
        raise SessionContractError(f"{where}.state must be {expected_state!r}")
    _identity(observation["identity"], f"{where}.identity", expected_token=expected_token)

    counts = require_mapping(observation["counts"], f"{where}.counts")
    require_exact_keys(counts, {"nodes", "arcs", "levels"}, f"{where}.counts")
    nodes = _u64(counts["nodes"], f"{where}.counts.nodes", positive=True)
    arcs = _u64(counts["arcs"], f"{where}.counts.arcs", positive=True)
    levels = _u64(counts["levels"], f"{where}.counts.levels", positive=True)
    if levels > nodes:
        raise SessionContractError(f"{where}.counts.levels exceeds nodes")

    buffers = require_mapping(observation["buffers"], f"{where}.buffers")
    require_exact_keys(buffers, BUFFER_KEYS, f"{where}.buffers")
    expected_extents = {
        "node_keys": nodes,
        "arc_keys": arcs,
        "out_row_offsets": nodes + 1,
        "out_arc_ids": arcs,
        "in_row_offsets": nodes + 1,
        "in_arc_ids": arcs,
        "level_offsets": levels + 1,
        "level_nodes": nodes,
        "node_to_level": nodes,
    }
    for field, expected in expected_extents.items():
        _extent(buffers[field], f"{where}.buffers.{field}", expected)

    capacity = require_mapping(observation["capacity"], f"{where}.capacity")
    require_exact_keys(capacity, {"last_action", "event_count"}, f"{where}.capacity")
    if capacity["last_action"] != "initial_allocate":
        raise SessionContractError(f"{where}.capacity.last_action changed")
    if _u64(capacity["event_count"], f"{where}.capacity.event_count") != 1:
        raise SessionContractError(f"{where}.capacity.event_count must be one")

    support = require_mapping(observation["support"], f"{where}.support")
    require_exact_keys(
        support,
        {"reference_supported", "unsupported_reasons"},
        f"{where}.support",
    )
    reasons = require_list(support["unsupported_reasons"], f"{where}.support.unsupported_reasons")
    reason_ordinals: list[int] = []
    for item in reasons:
        if type(item) is not str or item not in UNSUPPORTED_REASON_TOKENS:
            raise SessionContractError(
                f"{where}.support contains an unknown unsupported reason token"
            )
        reason_ordinals.append(UNSUPPORTED_REASON_TOKENS.index(item))
    if reason_ordinals != sorted(set(reason_ordinals)):
        raise SessionContractError(f"{where}.support reasons must follow stable unique enum order")
    if type(support["reference_supported"]) is not bool:
        raise SessionContractError(f"{where}.support.reference_supported must be bool")
    if support["reference_supported"] != (not reasons):
        raise SessionContractError(f"{where}.support fields disagree")

    timing = require_mapping(observation["timing_ms"], f"{where}.timing_ms")
    require_exact_keys(timing, {"export", "reference", "scatter"}, f"{where}.timing_ms")
    _finite_ms(timing["export"], f"{where}.timing_ms.export", nullable=False)
    if _finite_ms(timing["reference"], f"{where}.timing_ms.reference", nullable=True) is not None:
        raise SessionContractError(f"{where}.timing_ms.reference must be null")
    if _finite_ms(timing["scatter"], f"{where}.timing_ms.scatter", nullable=True) is not None:
        raise SessionContractError(f"{where}.timing_ms.scatter must be null")

    host = require_mapping(observation["host_bytes"], f"{where}.host_bytes")
    require_exact_keys(host, {"logical", "capacity"}, f"{where}.host_bytes")
    logical_bytes = _u64(host["logical"], f"{where}.host_bytes.logical", positive=True)
    capacity_bytes = _u64(host["capacity"], f"{where}.host_bytes.capacity", positive=True)
    if logical_bytes > capacity_bytes:
        raise SessionContractError(f"{where}.host_bytes logical exceeds capacity")
    if observation["last_rejection"] is not None or observation["last_invalidation"] is not None:
        raise SessionContractError(f"{where} initial capture must not contain an event")
    return observation


def _validate_release_transition(
    active: dict[str, Any], released: dict[str, Any], where: str
) -> None:
    expected = copy.deepcopy(active)
    expected["state"] = "released"
    if released != expected:
        raise SessionContractError(f"{where} released observation changed fields other than state")


def _oracle(value: Any, where: str, *, token: int) -> dict[str, Any]:
    oracle = require_mapping(value, where)
    require_exact_keys(oracle, ORACLE_KEYS, where)
    _identity(oracle["source_identity"], f"{where}.source_identity", expected_token=token)
    for field in ("node_count", "edge_count", "level_count"):
        _u64(oracle[field], f"{where}.{field}", positive=True)
    if oracle["level_count"] > oracle["node_count"]:
        raise SessionContractError(f"{where}.level_count exceeds node_count")
    decode_lower_hex(oracle["projection_wire_hex"], f"{where}.projection_wire_hex")
    decode_lower_hex(oracle["fifo_wire_hex"], f"{where}.fifo_wire_hex")
    return oracle


def _content_identity(identity: Mapping[str, Any]) -> tuple[str, ...]:
    return tuple(str(identity[field]) for field in CONTENT_FINGERPRINT_KEYS)


def _session(value: Any, where: str) -> dict[str, Any]:
    session = require_mapping(value, where)
    require_exact_keys(session, SESSION_KEYS, where)
    token = _u64(session["session_token"], f"{where}.session_token", positive=True)
    active = _observation(
        session["active"],
        f"{where}.active",
        expected_state="ready",
        expected_token=token,
    )
    released = _observation(
        session["released"],
        f"{where}.released",
        expected_state="released",
        expected_token=token,
    )
    _validate_release_transition(active, released, where)
    oracle = _oracle(session["oracle"], f"{where}.oracle", token=token)
    if active["identity"] != oracle["source_identity"]:
        raise SessionContractError(f"{where} observation and oracle identities differ")
    counts = active["counts"]
    expected_counts = (oracle["node_count"], oracle["edge_count"], oracle["level_count"])
    if (counts["nodes"], counts["arcs"], counts["levels"]) != expected_counts:
        raise SessionContractError(f"{where} observation and oracle counts differ")
    return session


def validate_session_probe_record(
    value: Any,
    descriptor: CaseDescriptor,
    repeat_index: int,
    run_nonce: str,
) -> dict[str, Any]:
    record = require_mapping(value, "session probe result")
    require_exact_keys(record, PROBE_KEYS, "session probe result")
    if record["schema"] != PROBE_SCHEMA:
        raise SessionContractError("session probe schema changed")
    if not isinstance(record["run_nonce"], str) or _NONCE.fullmatch(record["run_nonce"]) is None:
        raise SessionContractError("session probe run_nonce is malformed")
    if record["run_nonce"] != run_nonce:
        raise SessionContractError("session probe run_nonce echo mismatch")
    if record["case"] != descriptor.name:
        raise SessionContractError("session probe case echo mismatch")
    if _u64(record["repeat_index"], "session probe repeat_index") != repeat_index:
        raise SessionContractError("session probe repeat_index echo mismatch")
    if record["full_observation"] is not None:
        raise SessionContractError("cpu/full observation must be null")
    sessions = require_list(record["sessions"], "session probe sessions")
    if len(sessions) != 2:
        raise SessionContractError("session probe must contain exactly two sessions")
    validated = tuple(
        _session(item, f"session probe sessions[{index}]") for index, item in enumerate(sessions)
    )
    if validated[0]["session_token"] == validated[1]["session_token"]:
        raise SessionContractError("sequential session tokens must differ")
    first_identity = validated[0]["active"]["identity"]
    second_identity = validated[1]["active"]["identity"]
    if _content_identity(first_identity) != _content_identity(second_identity):
        raise SessionContractError("same-input sequential sessions changed content fingerprints")
    if validated[0]["active"]["counts"] != validated[1]["active"]["counts"]:
        raise SessionContractError("same-input sequential sessions changed counts")
    if (
        validated[0]["oracle"]["projection_wire_hex"]
        != validated[1]["oracle"]["projection_wire_hex"]
    ):
        raise SessionContractError("same-input sequential sessions changed projection bytes")
    if validated[0]["oracle"]["fifo_wire_hex"] != validated[1]["oracle"]["fifo_wire_hex"]:
        raise SessionContractError("same-input sequential sessions changed FIFO bytes")
    if record["complete"] is not True:
        raise SessionContractError("session probe result is incomplete")
    return record


def load_default_compat(path: Path) -> dict[str, Any]:
    compat = require_mapping(load_json(path.resolve()), "default compatibility")
    require_exact_keys(
        compat,
        {"schema", "expected_by_case"},
        "default compatibility",
    )
    if compat["schema"] != DEFAULT_COMPAT_SCHEMA:
        raise SessionContractError("default compatibility schema changed")
    expected = require_mapping(compat["expected_by_case"], "default compatibility cases")
    require_exact_keys(expected, {"chain", "diamond"}, "default compatibility cases")
    for case, value in expected.items():
        semantic = require_mapping(value, f"default compatibility {case}")
        require_exact_keys(
            semantic,
            set(DEFAULT_SEMANTIC_KEYS),
            f"default compatibility {case}",
        )
        _expected_default_semantics(semantic, f"default compatibility {case}")
    return compat


def _table_semantics(value: Any) -> list[Any]:
    return [value.present, value.variable1, value.variable2]


def _library_arc_semantics(value: Any) -> dict[str, Any]:
    return {
        "library_name": value.library_name,
        "arc_index": value.arc_index,
        "sense": value.sense,
        "trigger_transition": value.trigger_transition,
        "check_transition": value.check_transition,
        "sdf_condition": value.sdf_condition,
        "delay_tables": [_table_semantics(item) for item in value.delay_tables],
        "slew_tables": [_table_semantics(item) for item in value.slew_tables],
    }


def _functional_projection(record: Mapping[str, Any], where: str) -> dict[str, Any]:
    projection = decode_projection(
        decode_lower_hex(record["direct_projection"], f"{where}.direct_projection")
    )
    oracle = decode_oracle(
        decode_lower_hex(record["native_graph_oracle"], f"{where}.native_graph_oracle")
    )
    arc_ids = {arc.key: index for index, arc in enumerate(projection.arcs)}
    return {
        "projection_schema": projection.schema,
        "oracle_schema": oracle.schema,
        "nodes": [
            {
                "key": [
                    node.key.kind,
                    node.key.full_name,
                    node.key.instance_name,
                    node.key.pin_name,
                ],
                "direction": node.direction,
                "instance_cell": node.instance_cell,
                "net_name": node.net_name,
                "roles": [
                    node.is_port,
                    node.is_startpoint,
                    node.is_endpoint,
                    node.is_sequential,
                    node.is_clock_pin,
                    node.is_data_pin,
                    node.is_output_pin,
                ],
            }
            for node in projection.nodes
        ],
        "instances": [
            {
                "instance_name": instance.instance_name,
                "cell_name": instance.cell_name,
                "ordered_pins": [pin.full_name for pin in instance.ordered_pins],
                "roles": [
                    instance.is_sequential,
                    instance.has_timing_checks,
                    instance.is_clock_gating,
                    instance.is_macro,
                ],
            }
            for instance in projection.instances
        ],
        "arcs": [
            {
                "source": arc.key.source.full_name,
                "sink": arc.key.sink.full_name,
                "kind": arc.key.kind,
                "owner_name": arc.key.owner_name,
                "library_source_port": arc.key.library_source_port,
                "library_sink_port": arc.key.library_sink_port,
                "library_arcs": [
                    _library_arc_semantics(item) for item in arc.key.library_arcs
                ],
                "parallel_ordinal": arc.key.parallel_ordinal,
                "native_traversal_rank": arc.native_traversal_rank,
                "roles": [
                    arc.is_clock_arc,
                    arc.is_disabled,
                    arc.is_loop_disabled,
                ],
            }
            for arc in projection.arcs
        ],
        "base_out": [
            [row.node.full_name, [arc_ids[arc] for arc in row.ordered_arcs]]
            for row in projection.base_out
        ],
        "base_in": [
            [row.node.full_name, [arc_ids[arc] for arc in row.ordered_arcs]]
            for row in projection.base_in
        ],
        "ordered_roots": [node.full_name for node in projection.ordered_roots],
        "ordered_starts": [node.full_name for node in projection.ordered_starts],
        "ordered_ends": [node.full_name for node in projection.ordered_ends],
        "native_timing_order": [node.full_name for node in oracle.native_timing_order],
        "native_node_levels": [
            [node.full_name, level] for node, level in oracle.native_node_levels
        ],
    }


def _expected_default_semantics(record: Mapping[str, Any], where: str) -> dict[str, Any]:
    require_exact_keys(record, set(DEFAULT_SEMANTIC_KEYS), where)
    result = copy.deepcopy(dict(record))
    if not isinstance(result["functional_projection"], dict):
        raise SessionContractError(f"{where}.functional_projection must be an object")
    return _validate_default_metadata(result, where)


def _actual_default_semantics(record: Mapping[str, Any], where: str) -> dict[str, Any]:
    missing = [
        field
        for field in (
            "case", "top", "expected_clock", "direct_projection", "native_graph_oracle", "complete"
        )
        if field not in record
    ]
    if missing:
        raise SessionContractError(f"{where} lacks semantic fields: {missing!r}")
    result = {
        "case": copy.deepcopy(record["case"]),
        "top": copy.deepcopy(record["top"]),
        "expected_clock": copy.deepcopy(record["expected_clock"]),
        "functional_projection": _functional_projection(record, where),
        "complete": copy.deepcopy(record["complete"]),
    }
    return _validate_default_metadata(result, where)


def _validate_default_metadata(result: dict[str, Any], where: str) -> dict[str, Any]:
    if not isinstance(result["case"], str) or not isinstance(result["top"], str):
        raise SessionContractError(f"{where} case/top must be strings")
    if not isinstance(result["expected_clock"], str):
        raise SessionContractError(f"{where} expected_clock must be a string")
    if result["complete"] is not True:
        raise SessionContractError(f"{where}.complete must be true")
    return result


def validate_default_records(
    records_by_case: Mapping[str, Sequence[Mapping[str, Any]]],
    descriptors: Sequence[CaseDescriptor],
    compat: Mapping[str, Any],
) -> list[dict[str, Any]]:
    expected_by_case = require_mapping(
        compat["expected_by_case"], "default compatibility cases"
    )
    results: list[dict[str, Any]] = []
    for descriptor in descriptors:
        records = records_by_case.get(descriptor.name)
        if records is None or len(records) != 3:
            raise SessionContractError(
                f"default compatibility case {descriptor.name} must have three records"
            )
        expected = _expected_default_semantics(
            require_mapping(expected_by_case[descriptor.name], descriptor.name),
            f"default compatibility {descriptor.name}",
        )
        actual = tuple(
            _actual_default_semantics(record, f"default {descriptor.name} record")
            for record in records
        )
        if any(payload != expected for payload in actual):
            raise SessionContractError(
                f"default compatibility semantics changed for {descriptor.name}"
            )
        projection_payloads = tuple(
            decode_lower_hex(
                record["direct_projection"],
                f"default {descriptor.name} record[{index}].direct_projection",
            )
            for index, record in enumerate(records)
        )
        fifo_payloads = tuple(
            decode_lower_hex(
                record["native_graph_oracle"],
                f"default {descriptor.name} record[{index}].native_graph_oracle",
            )
            for index, record in enumerate(records)
        )
        if any(payload != projection_payloads[0] for payload in projection_payloads[1:]):
            raise SessionContractError(
                f"default projection bytes changed across repeats for {descriptor.name}"
            )
        if any(payload != fifo_payloads[0] for payload in fifo_payloads[1:]):
            raise SessionContractError(
                f"default FIFO bytes changed across repeats for {descriptor.name}"
            )
        results.append(
            {
                "case": descriptor.name,
                "projection_wire_hex": projection_payloads[0].hex(),
                "fifo_wire_hex": fifo_payloads[0].hex(),
            }
        )
    return results


def validate_controller_result(
    value: Any,
    descriptors: Sequence[CaseDescriptor],
) -> dict[str, Any]:
    summary = require_mapping(value, "session controller result")
    require_exact_keys(summary, CONTROLLER_KEYS, "session controller result")
    if summary["schema"] != CONTROLLER_SCHEMA:
        raise SessionContractError("session controller schema changed")
    repeats = validate_repeat_count(summary["requested_repeats"])
    if summary["complete"] is not True:
        raise SessionContractError("session controller result is incomplete")
    default_results = require_list(
        summary["default_compat_results"],
        "session controller default_compat_results",
    )
    case_results = require_list(summary["case_results"], "session controller case_results")
    if len(default_results) != len(descriptors) or len(case_results) != len(descriptors):
        raise SessionContractError("session controller case count changed")

    all_nonces: set[str] = set()
    semantic_by_case: list[tuple[tuple[str, ...], tuple[int, int, int], str, str]] = []
    for index, descriptor in enumerate(descriptors):
        default = require_mapping(default_results[index], f"default result[{index}]")
        require_exact_keys(default, DEFAULT_RESULT_KEYS, f"default result[{index}]")
        if default["case"] != descriptor.name:
            raise SessionContractError("default compatibility result order changed")
        default_projection = decode_lower_hex(
            default["projection_wire_hex"],
            f"default result[{index}].projection_wire_hex",
        )
        default_fifo = decode_lower_hex(
            default["fifo_wire_hex"],
            f"default result[{index}].fifo_wire_hex",
        )

        case = require_mapping(case_results[index], f"case result[{index}]")
        require_exact_keys(case, CASE_RESULT_KEYS, f"case result[{index}]")
        if case["case"] != descriptor.name:
            raise SessionContractError("session case identity/order changed")
        records = require_list(case["repeats"], f"case result[{index}].repeats")
        if len(records) != repeats:
            raise SessionContractError("session case repeat count changed")
        signatures: list[tuple[tuple[str, ...], tuple[int, int, int], str, str]] = []
        for repeat_index, raw_record in enumerate(records):
            raw_mapping = require_mapping(
                raw_record, f"case result[{index}].repeats[{repeat_index}]"
            )
            nonce = raw_mapping.get("run_nonce")
            if not isinstance(nonce, str):
                raise SessionContractError("session repeat nonce must be a string")
            record = validate_session_probe_record(
                raw_mapping,
                descriptor,
                repeat_index,
                nonce,
            )
            if nonce in all_nonces:
                raise SessionContractError("session controller reused a run nonce")
            all_nonces.add(nonce)
            session = record["sessions"][0]
            identity = session["active"]["identity"]
            counts = session["active"]["counts"]
            signatures.append(
                (
                    _content_identity(identity),
                    (counts["nodes"], counts["arcs"], counts["levels"]),
                    session["oracle"]["projection_wire_hex"],
                    session["oracle"]["fifo_wire_hex"],
                )
            )
        if any(signature != signatures[0] for signature in signatures[1:]):
            raise SessionContractError("fresh repeats changed semantic session output")
        if bytes.fromhex(signatures[0][2]) != default_projection:
            raise SessionContractError(
                "session projection bytes differ from validated default projection"
            )
        if bytes.fromhex(signatures[0][3]) != default_fifo:
            raise SessionContractError(
                "session FIFO bytes differ from validated default FIFO oracle"
            )
        semantic_by_case.append(signatures[0])

    chain, diamond = semantic_by_case
    if chain[0][0] == diamond[0][0]:
        raise SessionContractError("chain and diamond topology fingerprints unexpectedly match")
    if chain[2] == diamond[2]:
        raise SessionContractError("chain and diamond projection bytes unexpectedly match")
    if chain[1] == diamond[1]:
        raise SessionContractError("chain and diamond counts must differ")
    return summary
