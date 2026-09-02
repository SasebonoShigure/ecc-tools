#!/usr/bin/env python3
"""Strict schemas, independent probe validation, and atomic publication contracts."""

import argparse
import json
import os
import re
import stat
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import reference_input_bootstrap as bootstrap
import reference_input_canary_wire as canary_wire
import reference_input_candidate_wire as candidate_wire
import reference_input_raw_kat as raw_kat
import reference_input_raw_wire as raw_wire
import reference_input_recompute_fingerprint as fingerprint_oracle
import reference_input_recompute_graph as graph_oracle
import reference_input_recompute_model as model_oracle

PROBE_SCHEMA = "ista-csr-reference-input-probe/1"
CONTROLLER_SCHEMA = "ista-csr-reference-input-controller/1"
CASE_ORDER = ("chain", "diamond")
SESSION_TOKEN = 1
CANONICAL_CONTEXT = {
    "liberty_corner": "fresh-liberty-corner",
    "temperature": "25",
    "rc_corner": "__no_rc_corner__",
}
COMPLEX_RAW_HEX = (
    "0000000000000022697374612d6373722d7265666572656e63652d696e7075742d7261772f312e302e300000"
    "0000000000000000000000000001000000000000000175000000000000000443454c4c010000000000000000"
    "000000000000000000000000000000000003753a5100000000000000010000000000000003753a5800000000"
    "0000000000000000000000000000000000000000010000000000000001630000000000000001650000000000"
    "000002000000000000000567686f7374000000000000000567686f737400c000000000000000000000000000"
    "00000000bff00000000000000000000000000000000000000000000000000000000000000000000000000000"
    "010000000000000004736c6f77000000000000000443454c4c01010000000000000000000000000000000000"
    "000000000000000001590000000000000002504600000000000000024e53003fe0000000000000003fc00000"
    "00000000003fd0000000000000003fe8000000000000003fec000000000000003fd0000000000000003fe000"
    "0000000000003fe0000000000000003fe8000000000000000000000000000000000000000000010000000000"
    "00000141000000000000000159003fd0000000000000003fc0000000000000003fe000000000000000000000"
    "000000000000010000000000000004736c6f77000000070201000000000000000002454e0040000000000000"
    "00004010000000000000003fe0000000000000003fc0000000000000003fd0000000000000003fe800000000"
    "0000003fec000000000000003fd0000000000000003fe0000000000000003fe0000000000000003fe8000000"
    "0000000100000000000000000000000000000000000000000000000000000000000000000000000000000001"
    "000000000000000c666978747572652e737065660000000000000002504600000000000000034f484d000000"
    "000000000200000000000000057a5f6e657400000000000000057a5f656d62003ff800000000000000000000"
    "0000000200000000000000027a3200000000000000057a5f74776f003fd00000000000000040000000000000"
    "0000c00000000000000000000000000000027a3100000000000000057a5f6f6e65003fc0000000000000003f"
    "f000000000000000bff0000000000000000000000000000200000000000000027a3200000000000000027a31"
    "00402000000000000000000000000000027a3100000000000000027a32004010000000000000000000000000"
    "0005615f6e65740000000000000005615f656d62000000000000000000000000000000000200000000000000"
    "0261320000000000000005615f74776f00000000000000000000401000000000000000c01000000000000000"
    "0000000000000261310000000000000005615f6f6e6500000000000000000000400800000000000000c00800"
    "0000000000000000000000000200000000000000026132000000000000000261310040000000000000000000"
    "000000000002613100000000000000026132003ff0000000000000"
)
PROBE_KEYS = frozenset(
    {
        "schema",
        "name",
        "top",
        "expected_clock",
        "context",
        "repeat_index",
        "run_nonce",
        "session_token",
        "pre_candidate_wire_hex",
        "post_candidate_wire_hex",
        "pre_raw_projection_hex",
        "post_raw_projection_hex",
        "canaries",
        "complete",
    }
)
CANARY_KEYS = frozenset(
    {
        "family",
        "pristine_typed_bytes_hex",
        "before_typed_bytes_hex",
        "after_typed_bytes_hex",
    }
)
CONTROLLER_KEYS = frozenset({"schema", "requested_repeats", "case_results", "complete"})
CASE_RESULT_KEYS = frozenset({"case", "repeats"})
REPEAT_KEYS = frozenset(
    {
        "repeat_index",
        "session_token",
        "touched_families",
    }
)
FAMILY_TOKENS = tuple(canary_wire.model.CANARY_FAMILY_TOKENS)
_NONCE = re.compile(r"[0-9a-f]{32}")
_LOWER_HEX = re.compile(r"(?:[0-9a-f]{2})+")

CaseDescriptor = bootstrap.CaseDescriptor


class ReferenceInputContractError(RuntimeError):
    """A probe, summary, schema, or publication violated the Step 4 contract."""


@dataclass(frozen=True)
class ValidatedProbe:
    record: dict[str, Any]
    candidate: object
    raw: object
    candidate_bytes: bytes
    raw_bytes: bytes
    touched_families: tuple[str, ...]


def _mapping(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ReferenceInputContractError(f"{where} must be an object")
    return value


def _list(value: Any, where: str) -> list[Any]:
    if not isinstance(value, list):
        raise ReferenceInputContractError(f"{where} must be an array")
    return value


def _exact_keys(value: dict[str, Any], expected: frozenset[str], where: str) -> None:
    actual = frozenset(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ReferenceInputContractError(
            f"{where} keys differ; missing={missing!r}, extra={extra!r}"
        )


def _u64(value: Any, where: str, *, minimum: int = 0) -> int:
    if type(value) is not int or not minimum <= value < 1 << 64:
        raise ReferenceInputContractError(f"{where} must be a non-bool u64")
    return value


def decode_lower_hex(value: Any, where: str) -> bytes:
    if not isinstance(value, str) or _LOWER_HEX.fullmatch(value) is None:
        raise ReferenceInputContractError(f"{where} must be nonempty lowercase even-length hex")
    return bytes.fromhex(value)


def _json_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ReferenceInputContractError(f"JSON object duplicates key {key!r}")
        result[key] = value
    return result


def load_json(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as stream:
            return json.load(stream, object_pairs_hook=_json_pairs)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ReferenceInputContractError(f"cannot load strict JSON {path}: {exc}") from exc


def load_case_descriptors(path: Path) -> tuple[CaseDescriptor, CaseDescriptor]:
    try:
        result = bootstrap.load_case_descriptors(path)
    except Exception as exc:
        raise ReferenceInputContractError(f"case-descriptor validation failed: {exc}") from exc
    if tuple(item.name for item in result) != CASE_ORDER:
        raise ReferenceInputContractError("case descriptors are not exact chain/diamond order")
    return result


def _closed_objects(value: Any, where: str) -> None:
    if isinstance(value, dict):
        if value.get("type") == "object" and value.get("additionalProperties") is not False:
            raise ReferenceInputContractError(
                f"schema object {where} does not set additionalProperties=false"
            )
        for key, child in value.items():
            _closed_objects(child, f"{where}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _closed_objects(child, f"{where}[{index}]")


def _schema_object(schema: dict[str, Any], expected_keys: frozenset[str], where: str) -> None:
    if schema.get("type") != "object" or schema.get("additionalProperties") is not False:
        raise ReferenceInputContractError(f"{where} is not a closed object schema")
    properties = _mapping(schema.get("properties"), f"{where}.properties")
    required = schema.get("required")
    if set(properties) != expected_keys or not isinstance(required, list):
        raise ReferenceInputContractError(f"{where} property inventory differs")
    if len(required) != len(expected_keys) or set(required) != expected_keys:
        raise ReferenceInputContractError(f"{where} required inventory differs")


def validate_schema_documents(probe_path: Path, controller_path: Path) -> None:
    probe = _mapping(load_json(probe_path), "probe schema")
    controller = _mapping(load_json(controller_path), "controller schema")
    _closed_objects(probe, "probe")
    _closed_objects(controller, "controller")
    _schema_object(probe, PROBE_KEYS, "probe")
    _schema_object(controller, CONTROLLER_KEYS, "controller")
    probe_defs = _mapping(probe.get("$defs"), "probe.$defs")
    controller_defs = _mapping(controller.get("$defs"), "controller.$defs")
    if probe["properties"]["schema"].get("const") != PROBE_SCHEMA:
        raise ReferenceInputContractError("probe schema const differs")
    if controller["properties"]["schema"].get("const") != CONTROLLER_SCHEMA:
        raise ReferenceInputContractError("controller schema const differs")
    if tuple(probe_defs["family"].get("enum", ())) != FAMILY_TOKENS:
        raise ReferenceInputContractError("probe family enum differs")
    if tuple(controller_defs["family"].get("enum", ())) != FAMILY_TOKENS:
        raise ReferenceInputContractError("controller family enum differs")
    _schema_object(probe_defs["context"], frozenset(CANONICAL_CONTEXT), "context")
    _schema_object(probe_defs["canary"], CANARY_KEYS, "canary")
    _schema_object(controller_defs["repeat"], REPEAT_KEYS, "repeat")
    _schema_object(controller_defs["caseResult"], CASE_RESULT_KEYS, "caseResult")


def _validate_context(value: Any, expected: dict[str, str]) -> dict[str, str]:
    context = _mapping(value, "probe.context")
    _exact_keys(context, frozenset(CANONICAL_CONTEXT), "probe.context")
    if any(not isinstance(item, str) or not item for item in context.values()):
        raise ReferenceInputContractError("probe context values must be nonempty strings")
    if context != expected:
        raise ReferenceInputContractError("probe context does not echo controller context")
    return context


def _validate_subject_inventory(envelope: canary_wire.CanaryEnvelope, raw: Any) -> None:
    family = envelope.family
    keys = tuple(subject.key for subject in envelope.subjects)
    if family.value <= canary_wire.model.CanaryFamily.TIMING_POINT_PATH_STATE_MAP.value:
        expected = tuple(canary_wire.PointKey(node.full_name) for node in raw.nodes)
    elif family.value <= canary_wire.model.CanaryFamily.ARC_TIMING_ARC_FALLBACK_PROVENANCE.value:
        expected = tuple(
            canary_wire.ArcKey(
                arc.native_rank,
                arc.source,
                arc.sink,
                arc.kind,
                arc.owner,
                arc.library_source_port,
                arc.library_sink_port,
            )
            for arc in sorted(raw.arcs, key=lambda item: item.native_rank)
        )
    else:
        expected = ("database",)
    if keys != expected:
        raise ReferenceInputContractError("canary subjects differ from raw stable inventory")


def _decode_candidate_and_raw(
    record: dict[str, Any], context: dict[str, str]
) -> tuple[object, object, bytes, bytes]:
    pre_candidate = decode_lower_hex(
        record["pre_candidate_wire_hex"], "probe.pre_candidate_wire_hex"
    )
    post_candidate = decode_lower_hex(
        record["post_candidate_wire_hex"], "probe.post_candidate_wire_hex"
    )
    pre_raw = decode_lower_hex(record["pre_raw_projection_hex"], "probe.pre_raw_projection_hex")
    post_raw = decode_lower_hex(record["post_raw_projection_hex"], "probe.post_raw_projection_hex")
    if pre_candidate != post_candidate or pre_raw != post_raw:
        raise ReferenceInputContractError("poison changed candidate or raw projection bytes")
    try:
        candidate = candidate_wire.decode_candidate_wire(pre_candidate)
        post_decoded = candidate_wire.decode_candidate_wire(post_candidate)
        raw = raw_wire.decode_raw_wire(pre_raw)
        post_raw_decoded = raw_wire.decode_raw_wire(post_raw)
    except RuntimeError as exc:
        raise ReferenceInputContractError(f"strict candidate/raw decode failed: {exc}") from exc
    if candidate != post_decoded or raw != post_raw_decoded:
        raise ReferenceInputContractError("pre/post decoded candidate or raw differs")
    expected_context = (
        context["liberty_corner"],
        context["temperature"],
        context["rc_corner"],
    )
    actual_context = (
        candidate.analysis_context.liberty_corner,
        candidate.analysis_context.temperature,
        candidate.analysis_context.rc_corner,
    )
    if actual_context != expected_context:
        raise ReferenceInputContractError("candidate analysis context differs from probe echo")
    try:
        graph_facts = graph_oracle.recompute_graph(candidate, raw)
        model_oracle.recompute_model(candidate, raw, graph_facts)
        identity = fingerprint_oracle.recompute_identity(candidate, SESSION_TOKEN, 0)
    except RuntimeError as exc:
        raise ReferenceInputContractError(f"independent model replay failed: {exc}") from exc
    if candidate.source_identity != identity:
        raise ReferenceInputContractError(
            "candidate source identity differs from six recomputed hashes"
        )
    return candidate, raw, pre_candidate, pre_raw


def validate_probe_record(
    raw_record: Any,
    descriptor: CaseDescriptor,
    expected_repeat: int,
    expected_nonce: str,
    expected_context: dict[str, str] | None = None,
) -> ValidatedProbe:
    record = _mapping(raw_record, "probe result")
    _exact_keys(record, PROBE_KEYS, "probe result")
    if record["schema"] != PROBE_SCHEMA or record["complete"] is not True:
        raise ReferenceInputContractError("probe schema/complete marker differs")
    if not isinstance(expected_nonce, str) or _NONCE.fullmatch(expected_nonce) is None:
        raise ReferenceInputContractError("controller nonce is malformed")
    _u64(expected_repeat, "expected repeat")
    expected_echo = {
        "name": descriptor.name,
        "top": descriptor.top,
        "expected_clock": descriptor.expected_clock,
        "repeat_index": expected_repeat,
        "run_nonce": expected_nonce,
        "session_token": SESSION_TOKEN,
    }
    for field, expected in expected_echo.items():
        if type(record[field]) is not type(expected) or record[field] != expected:
            raise ReferenceInputContractError(f"probe {field} does not echo its run slot")
    context = _validate_context(
        record["context"], CANONICAL_CONTEXT if expected_context is None else expected_context
    )
    candidate, raw, candidate_bytes, raw_bytes = _decode_candidate_and_raw(record, context)
    canaries = _list(record["canaries"], "probe.canaries")
    if len(canaries) != len(FAMILY_TOKENS):
        raise ReferenceInputContractError("probe must contain exactly 17 canaries")
    touched: list[str] = []
    for index, token in enumerate(FAMILY_TOKENS):
        item = _mapping(canaries[index], f"probe.canaries[{index}]")
        _exact_keys(item, CANARY_KEYS, f"probe.canaries[{index}]")
        if item["family"] != token:
            raise ReferenceInputContractError("canary family order/token differs")
        pristine_bytes = decode_lower_hex(item["pristine_typed_bytes_hex"], "canary.pristine")
        before_bytes = decode_lower_hex(item["before_typed_bytes_hex"], "canary.before")
        after_bytes = decode_lower_hex(item["after_typed_bytes_hex"], "canary.after")
        try:
            pristine = canary_wire.decode_canary_wire(pristine_bytes, token)
            before = canary_wire.decode_canary_wire(before_bytes, token)
            after = canary_wire.decode_canary_wire(after_bytes, token)
            canary_wire.validate_canary_scaffold(pristine, before)
            changed = canary_wire.validate_canary_pair(before, after)
        except RuntimeError as exc:
            raise ReferenceInputContractError(f"canary {token} is invalid: {exc}") from exc
        binding = before.binding
        if (
            binding.repeat_index,
            binding.run_nonce,
            binding.session_token,
        ) != (
            expected_repeat,
            expected_nonce,
            SESSION_TOKEN,
        ):
            raise ReferenceInputContractError(f"canary {token} binding differs")
        _validate_subject_inventory(pristine, raw)
        _validate_subject_inventory(before, raw)
        touched.append(changed)
    if tuple(touched) != FAMILY_TOKENS:
        raise ReferenceInputContractError("aggregate canary mask is not exact 0x1ffff")
    return ValidatedProbe(
        record,
        candidate,
        raw,
        candidate_bytes,
        raw_bytes,
        tuple(touched),
    )


def validate_controller_result(
    raw_result: Any,
    descriptors: tuple[CaseDescriptor, CaseDescriptor],
) -> dict[str, Any]:
    result = _mapping(raw_result, "controller result")
    _exact_keys(result, CONTROLLER_KEYS, "controller result")
    if result["schema"] != CONTROLLER_SCHEMA or result["complete"] is not True:
        raise ReferenceInputContractError("controller schema/complete marker differs")
    repeats = _u64(result["requested_repeats"], "requested_repeats", minimum=3)
    cases = _list(result["case_results"], "controller.case_results")
    if len(cases) != 2:
        raise ReferenceInputContractError("controller requires exactly two case results")
    for case_index, descriptor in enumerate(descriptors):
        case = _mapping(cases[case_index], f"case_results[{case_index}]")
        _exact_keys(case, CASE_RESULT_KEYS, f"case_results[{case_index}]")
        if case["case"] != descriptor.name:
            raise ReferenceInputContractError("controller case order/name differs")
        rows = _list(case["repeats"], f"case {descriptor.name}.repeats")
        if len(rows) != repeats:
            raise ReferenceInputContractError("controller repeat count is incomplete")
        for expected_repeat, raw_row in enumerate(rows):
            row = _mapping(raw_row, f"case {descriptor.name} repeat")
            _exact_keys(row, REPEAT_KEYS, "controller repeat")
            if row["repeat_index"] != expected_repeat or type(row["repeat_index"]) is not int:
                raise ReferenceInputContractError("controller repeat indices are not exact")
            if row["session_token"] != SESSION_TOKEN or type(row["session_token"]) is not int:
                raise ReferenceInputContractError("controller repeat session token differs")
            if tuple(_list(row["touched_families"], "touched_families")) != FAMILY_TOKENS:
                raise ReferenceInputContractError("controller touched_families differs")
    return result


def require_regular_owned_file(path: Path, where: str) -> os.stat_result:
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise ReferenceInputContractError(f"{where} is missing: {path}") from exc
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        raise ReferenceInputContractError(f"{where} must be one regular, non-linked file")
    if hasattr(os, "geteuid") and metadata.st_uid != os.geteuid():
        raise ReferenceInputContractError(f"{where} is not owned by the current user")
    return metadata


def write_json_exclusive(path: Path, value: Any) -> None:
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except OSError as exc:
        raise ReferenceInputContractError(f"cannot create exclusive JSON {path}: {exc}") from exc
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            descriptor = -1
            json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def publish_atomic(temporary: Path, final: Path, where: str) -> None:
    if temporary.parent != final.parent:
        raise ReferenceInputContractError(f"{where} temp/final directories differ")
    require_regular_owned_file(temporary, f"{where} temporary")
    if final.exists() or final.is_symlink():
        raise ReferenceInputContractError(f"{where} final already exists: {final}")
    os.replace(temporary, final)
    require_regular_owned_file(final, f"{where} final")


def _validate_complex_raw_kat() -> None:
    expected = raw_kat.complex_raw_kat_input()
    payload = raw_wire.encode_raw_wire(expected)
    if len(payload) != 1127 or payload.hex() != COMPLEX_RAW_HEX:
        raise ReferenceInputContractError("complex raw-wire known-answer bytes differ")
    decoded = raw_wire.decode_raw_wire(bytes.fromhex(COMPLEX_RAW_HEX))
    if decoded != expected:
        raise ReferenceInputContractError("complex raw-wire literal decode differs")
    if (
        len(decoded.instances) != 1
        or decoded.instances[0].selected_clock_arc is not None
        or decoded.instances[0].output_pin_name != "u:Q"
        or decoded.instances[0].ordered_pin_names != ("u:X",)
    ):
        raise ReferenceInputContractError("complex raw-wire missing-C2Q fields differ")
    if len(decoded.clock_constraints) != 1:
        raise ReferenceInputContractError("complex raw-wire clock inventory differs")
    clock = decoded.clock_constraints[0]
    if (
        (clock.map_key, clock.embedded_name) != ("c", "e")
        or clock.ordered_sources != ("ghost", "ghost")
        or (clock.period, clock.rise_edge, clock.fall_edge) != (-2.0, 0.0, -1.0)
    ):
        raise ReferenceInputContractError("complex raw-wire unresolved clock fields differ")
    if len(decoded.referenced_library_cells) != 1:
        raise ReferenceInputContractError("complex raw-wire library inventory differs")
    library = decoded.referenced_library_cells[0]
    if library.payload is None or len(library.payload.cell_arcs) != 1:
        raise ReferenceInputContractError("complex raw-wire library payload differs")
    child = library.payload.cell_arcs[0].ordered_children[0]
    tables = (*child.delay_tables, *child.slew_tables, *child.check_tables)
    if (
        not tables[0].present
        or tables[0].variable1 != raw_wire.TableVariable.NONE
        or tables[0].variable2 != raw_wire.TableVariable.NONE
        or tables[0].axis1
        or tables[0].axis2
        or tables[0].values
        or any(table.present for table in tables[1:])
    ):
        raise ReferenceInputContractError("complex raw-wire present-empty table differs")
    parasitics = decoded.parasitics
    if parasitics is None or tuple(net.map_key for net in parasitics.nets) != ("z_net", "a_net"):
        raise ReferenceInputContractError("complex raw-wire parasitic net order differs")
    z_net, a_net = parasitics.nets
    if tuple(node.map_key for node in z_net.nodes) != ("z2", "z1") or tuple(
        (resistor.source, resistor.sink, resistor.resistance) for resistor in z_net.resistors
    ) != (("z2", "z1", 8.0), ("z1", "z2", 4.0)):
        raise ReferenceInputContractError("complex raw-wire z_net order differs")
    if tuple(node.map_key for node in a_net.nodes) != ("a2", "a1") or tuple(
        (resistor.source, resistor.sink, resistor.resistance) for resistor in a_net.resistors
    ) != (("a2", "a1", 2.0), ("a1", "a2", 1.0)):
        raise ReferenceInputContractError("complex raw-wire a_net order differs")


def _self_test(probe_schema: Path, controller_schema: Path) -> None:
    validate_schema_documents(probe_schema, controller_schema)
    payload = raw_wire.encode_raw_wire(raw_wire.empty_raw_input())
    if (
        len(payload) != 115
        or payload.hex() != raw_wire.EMPTY_RAW_HEX
        or raw_wire.decode_raw_wire(payload) != raw_wire.empty_raw_input()
    ):
        raise ReferenceInputContractError("empty raw-wire known-answer test differs")
    _validate_complex_raw_kat()
    try:
        raw_wire.decode_raw_wire(payload + b"\x00")
    except RuntimeError:
        pass
    else:
        raise ReferenceInputContractError("raw-wire decoder accepted trailing bytes")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-schema", type=Path, required=True)
    parser.add_argument("--controller-schema", type=Path, required=True)
    parser.add_argument("--self-test", action="store_true", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    _self_test(arguments.probe_schema.resolve(), arguments.controller_schema.resolve())
    print("reference-input contract self-test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
