#!/usr/bin/env python3
"""Validate the closed-world Stage 3 timing-edit trace wire contract."""

import argparse
import copy
import math
import struct
import sys
from collections.abc import Callable
from pathlib import Path

from json_schema_contract import ValidationError, fail, load_json, loads_json, validate_schema

SCHEMA_PATH = Path(__file__).with_name("timing_edit_trace_v1.schema.json")
_U64_MAX = (1 << 64) - 1


class _Encoder:
    """The byte primitives used by the production CSR canonical encoder."""

    def __init__(self) -> None:
        self._value = bytearray()

    def result(self) -> bytes:
        return bytes(self._value)

    def u8(self, value: int) -> None:
        self._value.append(value)

    def u32(self, value: int) -> None:
        self._value.extend(value.to_bytes(4, "big", signed=False))

    def i32(self, value: int) -> None:
        self.u32(value & 0xFFFFFFFF)

    def u64(self, value: int) -> None:
        self._value.extend(value.to_bytes(8, "big", signed=False))

    def boolean(self, *, value: bool) -> None:
        self.u8(1 if value else 0)

    def text(self, value: str) -> None:
        try:
            encoded = value.encode("utf-8")
        except UnicodeEncodeError as error:
            raise ValidationError("text is not valid UTF-8", category="format") from error
        self.u64(len(encoded))
        self._value.extend(encoded)

    def blob(self, value: bytes) -> None:
        self.u64(len(value))
        self._value.extend(value)

    def number(self, value: int | float) -> None:
        result = _finite_double(value, "$")
        if result == 0.0:
            result = 0.0
        self.u8(0)
        self._value.extend(struct.pack(">d", result))

    def domain(self, name: str) -> None:
        self.text("ista-csr")
        self.text(name)
        self.text("1.0.0")


def _finite_double(value: int | float, field: str) -> float:
    try:
        result = float(value)
    except (OverflowError, ValueError) as error:
        fail(field, f"must convert to a finite double: {error}", "non_finite")
    if not math.isfinite(result):
        fail(field, "must be finite", "non_finite")
    return result


def _append_token(encoder: _Encoder, value: dict[str, object]) -> None:
    encoder.u32(value["schema_version"])
    encoder.text(value["case_id"])
    encoder.text(value["profile_id"])
    encoder.u64(value["seed"])
    encoder.u64(value["batch_ordinal"])
    encoder.u64(value["accepted_ordinal"])


def _append_node(encoder: _Encoder, value: dict[str, object]) -> None:
    encoder.text(value["kind"])
    encoder.text(value["full_name"])
    encoder.text(value["instance_name"])
    encoder.text(value["pin_name"])


def _node_bytes(value: dict[str, object]) -> bytes:
    encoder = _Encoder()
    encoder.domain("node-key")
    _append_node(encoder, value)
    return encoder.result()


def _append_table(encoder: _Encoder, value: dict[str, object]) -> None:
    encoder.boolean(value=value["present"])
    encoder.text(value["variable1"])
    encoder.text(value["variable2"])


def _append_signature(encoder: _Encoder, value: dict[str, object]) -> None:
    encoder.text(value["library_name"])
    encoder.i32(value["arc_index"])
    encoder.text(value["sense"])
    encoder.text(value["trigger_transition"])
    encoder.text(value["check_transition"])
    encoder.text(value["sdf_condition"])
    for table in value["delay_tables"]:
        _append_table(encoder, table)
    for table in value["slew_tables"]:
        _append_table(encoder, table)


def _signature_bytes(value: dict[str, object]) -> bytes:
    encoder = _Encoder()
    encoder.domain("library-arc-signature")
    _append_signature(encoder, value)
    return encoder.result()


def _append_arc(encoder: _Encoder, value: dict[str, object]) -> None:
    _append_node(encoder, value["source"])
    _append_node(encoder, value["sink"])
    encoder.text(value["kind"])
    encoder.text(value["owner_name"])
    encoder.text(value["library_source_port"])
    encoder.text(value["library_sink_port"])
    encoder.u64(len(value["library_arcs"]))
    for signature in value["library_arcs"]:
        _append_signature(encoder, signature)
    encoder.u64(value["parallel_ordinal"])


def _arc_bytes(value: dict[str, object]) -> bytes:
    encoder = _Encoder()
    encoder.domain("arc-key")
    _append_arc(encoder, value)
    return encoder.result()


def _append_node_ref(encoder: _Encoder, value: dict[str, object]) -> None:
    encoder.text(value["kind"])
    if value["kind"] == "existing":
        encoder.blob(_node_bytes(value["baseline_key"]))
    else:
        _append_token(encoder, value["creation_token"])
        encoder.text(value["role"])


def _node_ref_bytes(value: dict[str, object]) -> bytes:
    encoder = _Encoder()
    encoder.domain("timing-edit-node-ref")
    _append_node_ref(encoder, value)
    return encoder.result()


def _append_net_ref(encoder: _Encoder, value: dict[str, object]) -> None:
    encoder.text(value["kind"])
    if value["kind"] == "existing":
        encoder.text(value["baseline_name"])
    else:
        _append_token(encoder, value["creation_token"])
        encoder.text(value["role"])


def _net_ref_bytes(value: dict[str, object]) -> bytes:
    encoder = _Encoder()
    encoder.domain("timing-edit-net-ref")
    _append_net_ref(encoder, value)
    return encoder.result()


def _append_sorted(encoder: _Encoder, values: list[bytes]) -> None:
    encoder.u64(len(values))
    for value in sorted(values):
        encoder.blob(value)


def _append_library_cell(encoder: _Encoder, value: dict[str, object]) -> None:
    encoder.text(value["library_name"])
    encoder.text(value["cell_name"])
    encoder.text(value["input_port"])
    encoder.text(value["output_port"])
    signatures = value["ordered_arc_signatures"]
    encoder.u64(len(signatures))
    for signature in signatures:
        encoder.blob(_signature_bytes(signature))


def _edit_bytes(value: dict[str, object]) -> bytes:
    encoder = _Encoder()
    encoder.domain("timing-edit-buffer-insertion")
    _append_net_ref(encoder, value["original_net"])
    _append_sorted(encoder, [_node_ref_bytes(item) for item in value["moved_sinks"]])
    _append_library_cell(encoder, value["library_cell"])
    placement = value["placement_inputs"]
    encoder.text(placement["kind"])
    if placement["kind"] == "placed":
        encoder.number(placement["x_microns"])
        encoder.number(placement["y_microns"])
    rc_inputs = value["rc_inputs"]
    encoder.text(rc_inputs["kind"])
    if rc_inputs["kind"] == "parasitic":
        encoder.text(rc_inputs["policy"])
    _append_token(encoder, value["creation_token"])
    return encoder.result()


def _valid_text(value: str, field: str) -> None:
    if not value or "\0" in value:
        fail(field, "must be nonempty and contain no NUL")


def _require_sorted_unique(
    values: list[object],
    encode: Callable[[object], bytes],
    field: str,
) -> None:
    encoded = [encode(value) for value in values]
    if encoded != sorted(encoded) or len(encoded) != len(set(encoded)):
        fail(field, "must be in production canonical byte order and duplicate-free")


def _validate_node(value: dict[str, object], field: str) -> None:
    if value["kind"] == "port":
        if value["instance_name"] or value["pin_name"]:
            fail(field, "port key must not contain instance or pin names")
    elif value["full_name"] != f"{value['instance_name']}:{value['pin_name']}":
        fail(field, "instance-pin full_name must equal instance_name + ':' + pin_name")


def _validate_table(value: dict[str, object], field: str) -> None:
    if not value["present"] and (value["variable1"] != "none" or value["variable2"] != "none"):
        fail(field, "absent table must not retain variables")
    if value["present"] and value["variable1"] == "none" and value["variable2"] != "none":
        fail(field, "variable2 requires variable1")


def _validate_signature(value: dict[str, object], field: str, *, validate_tables: bool) -> None:
    for family in ("delay_tables", "slew_tables"):
        tables = value[family]
        if len(tables) != 2:
            fail(f"{field}.{family}", "must contain exactly two entries")
        if validate_tables:
            for index, table in enumerate(tables):
                _validate_table(table, f"{field}.{family}[{index}]")


def _validate_arc(value: dict[str, object], field: str) -> None:
    _validate_node(value["source"], f"{field}.source")
    _validate_node(value["sink"], f"{field}.sink")
    if value["kind"] == "net":
        if value["library_source_port"] or value["library_sink_port"] or value["library_arcs"]:
            fail(field, "net arc must not contain library identity")
    elif not value["library_source_port"] or not value["library_sink_port"]:
        fail(field, "cell arc requires library ports")
    for index, signature in enumerate(value["library_arcs"]):
        _validate_signature(signature, f"{field}.library_arcs[{index}]", validate_tables=True)


def _validate_pre_state(value: dict[str, object], field: str) -> None:
    for family in ("instances", "nets"):
        names = value[family]
        for index, name in enumerate(names):
            _valid_text(name, f"{field}.{family}[{index}]")
        encoded = [name.encode("utf-8") for name in names]
        if encoded != sorted(encoded) or len(encoded) != len(set(encoded)):
            fail(f"{field}.{family}", "must be sorted and duplicate-free")
    for index, node in enumerate(value["nodes"]):
        _validate_node(node, f"{field}.nodes[{index}]")
    _require_sorted_unique(value["nodes"], _node_bytes, f"{field}.nodes")
    for index, arc in enumerate(value["arcs"]):
        _validate_arc(arc, f"{field}.arcs[{index}]")
    _require_sorted_unique(value["arcs"], _arc_bytes, f"{field}.arcs")


def _validate_identity(value: dict[str, object], field: str) -> None:
    _validate_pre_state(value["canonical_pre_state"], f"{field}.canonical_pre_state")


def _validate_token(value: dict[str, object], field: str) -> None:
    _valid_text(value["case_id"], f"{field}.case_id")
    _valid_text(value["profile_id"], f"{field}.profile_id")


def _validate_node_ref(value: dict[str, object], field: str) -> None:
    if value["kind"] == "existing":
        _validate_node(value["baseline_key"], f"{field}.baseline_key")
    else:
        _validate_token(value["creation_token"], f"{field}.creation_token")


def _validate_net_ref(value: dict[str, object], field: str) -> None:
    if value["kind"] == "existing":
        _valid_text(value["baseline_name"], f"{field}.baseline_name")
    else:
        _validate_token(value["creation_token"], f"{field}.creation_token")


def _validate_library_cell(value: dict[str, object], field: str) -> None:
    for name in ("library_name", "cell_name", "input_port", "output_port"):
        _valid_text(value[name], f"{field}.{name}")
    if value["input_port"] == value["output_port"]:
        fail(f"{field}.output_port", "input and output ports must differ")
    signatures = value["ordered_arc_signatures"]
    for index, signature in enumerate(signatures):
        _validate_signature(
            signature,
            f"{field}.ordered_arc_signatures[{index}]",
            validate_tables=False,
        )
    _require_sorted_unique(signatures, _signature_bytes, f"{field}.ordered_arc_signatures")


def _validate_edit(value: dict[str, object], field: str) -> None:
    token = value["creation_token"]
    _validate_token(token, f"{field}.creation_token")
    original_net = value["original_net"]
    _validate_net_ref(original_net, f"{field}.original_net")
    if (
        original_net["kind"] == "created"
        and original_net["creation_token"]["batch_ordinal"] >= token["batch_ordinal"]
    ):
        fail(f"{field}.original_net", "created original net must come from an earlier batch")
    sinks = value["moved_sinks"]
    for index, sink in enumerate(sinks):
        _validate_node_ref(sink, f"{field}.moved_sinks[{index}]")
        if (
            sink["kind"] == "created"
            and sink["creation_token"]["batch_ordinal"] >= token["batch_ordinal"]
        ):
            fail(f"{field}.moved_sinks[{index}]", "created sink must come from an earlier batch")
    _require_sorted_unique(sinks, _node_ref_bytes, f"{field}.moved_sinks")
    _validate_library_cell(value["library_cell"], f"{field}.library_cell")
    placement = value["placement_inputs"]
    if placement["kind"] == "placed":
        _finite_double(placement["x_microns"], f"{field}.placement_inputs.x_microns")
        _finite_double(placement["y_microns"], f"{field}.placement_inputs.y_microns")
    rc_inputs = value["rc_inputs"]
    if rc_inputs["kind"] == "parasitic" and rc_inputs["policy"] == "error":
        fail(f"{field}.rc_inputs", "parasitic mutation is rejected by policy")


def _token_key(value: dict[str, object]) -> tuple[object, ...]:
    return tuple(
        value[name]
        for name in (
            "schema_version",
            "case_id",
            "profile_id",
            "seed",
            "batch_ordinal",
            "accepted_ordinal",
        )
    )


def _validate_batch(value: dict[str, object], field: str, *, seed: int, profile: str) -> None:
    pre = value["expected_pre_identity"]
    post = value["expected_post_identity"]
    _validate_identity(pre, f"{field}.expected_pre_identity")
    _validate_identity(post, f"{field}.expected_post_identity")
    if pre["generation"] == _U64_MAX or post["generation"] != pre["generation"] + 1:
        fail(f"{field}.expected_post_identity.generation", "must equal pre generation plus one")

    edits = value["edits"]
    encoded_edits: list[bytes] = []
    tokens: set[tuple[object, ...]] = set()
    original_nets: set[bytes] = set()
    moved_sinks: set[bytes] = set()
    requires_full = False
    for index, edit in enumerate(edits):
        edit_field = f"{field}.edits[{index}]"
        _validate_edit(edit, edit_field)
        encoded_edits.append(_edit_bytes(edit))
        token = edit["creation_token"]
        key = _token_key(token)
        if key in tokens:
            fail(f"{edit_field}.creation_token", "duplicate creation token")
        tokens.add(key)
        if token["batch_ordinal"] != pre["generation"] + 1:
            fail(f"{edit_field}.creation_token.batch_ordinal", "must equal pre generation plus one")
        if (
            token["seed"] != seed
            or token["profile_id"] != profile
            or token["batch_ordinal"] != value["ordinal"]
        ):
            fail(f"{edit_field}.creation_token", "seed/profile/batch must match trace envelope")
        net_key = _net_ref_bytes(edit["original_net"])
        if net_key in original_nets:
            fail(f"{edit_field}.original_net", "only one insertion per original net is allowed")
        original_nets.add(net_key)
        for sink in edit["moved_sinks"]:
            sink_key = _node_ref_bytes(sink)
            if sink_key in moved_sinks:
                fail(f"{edit_field}.moved_sinks", "sink is moved by more than one edit")
            moved_sinks.add(sink_key)
        rc_inputs = edit["rc_inputs"]
        requires_full |= (
            rc_inputs["kind"] == "parasitic" and rc_inputs["policy"] == "full_with_record"
        )
    if encoded_edits != sorted(encoded_edits):
        fail(f"{field}.edits", "must be in production canonical byte order")

    receipt = value["policy_receipt"]
    expected_disposition = "full_with_record" if requires_full else "supported"
    if receipt["disposition"] != expected_disposition:
        fail(f"{field}.policy_receipt.disposition", "does not match batch RC fallback semantics")
    reasons = receipt["reasons"]
    if bool(reasons) != requires_full:
        fail(f"{field}.policy_receipt.reasons", "must be nonempty exactly for full_with_record")
    if reasons != sorted(reasons) or len(reasons) != len(set(reasons)):
        fail(f"{field}.policy_receipt.reasons", "must be sorted and duplicate-free")


def _validate_semantics(document: dict[str, object]) -> None:
    _valid_text(document["profile"], "$.profile")
    baseline = document["baseline"]
    _valid_text(baseline["design_name"], "$.baseline.design_name")
    _validate_pre_state(baseline["canonical_pre_state"], "$.baseline.canonical_pre_state")

    artifact_keys: set[tuple[str, str]] = set()
    for index, artifact in enumerate(document["source_artifacts"]):
        field = f"$.source_artifacts[{index}]"
        _valid_text(artifact["artifact_role"], f"{field}.artifact_role")
        _valid_text(artifact["logical_name"], f"{field}.logical_name")
        key = (artifact["artifact_role"], artifact["logical_name"])
        if key in artifact_keys:
            fail(field, "duplicate source artifact role/name")
        artifact_keys.add(key)

    expected_pre = {
        "generation": 0,
        "current_fingerprints": baseline["fingerprints"],
        "canonical_pre_state": baseline["canonical_pre_state"],
    }
    for index, batch in enumerate(document["batches"]):
        field = f"$.batches[{index}]"
        if batch["ordinal"] != index + 1:
            fail(f"{field}.ordinal", "batch ordinals must be dense, one-based and ordered")
        if batch["expected_pre_identity"] != expected_pre:
            fail(f"{field}.expected_pre_identity", "batch identity chain is discontinuous")
        _validate_batch(
            batch,
            field,
            seed=document["seed"],
            profile=document["profile"],
        )
        expected_pre = batch["expected_post_identity"]

    replay = document["replay_command"]
    _valid_text(replay["executable"], "$.replay_command.executable")
    for index, argument in enumerate(replay["arguments"]):
        _valid_text(argument, f"$.replay_command.arguments[{index}]")
    if "--trace" not in replay["arguments"] or "--prefix" not in replay["arguments"]:
        fail("$.replay_command.arguments", "must contain --trace and --prefix")


def validate_document(document: object, schema: object) -> None:
    """Validate declarative shape first, then production cross-field semantics."""

    if not isinstance(schema, dict):
        raise ValidationError("root schema must be an object")
    validate_schema(document, schema)
    if not isinstance(document, dict):
        raise ValidationError("root trace must be an object")
    _validate_semantics(document)


def _table() -> dict[str, object]:
    return {"present": False, "variable1": "none", "variable2": "none"}


def _signature() -> dict[str, object]:
    return {
        "library_name": "fixture.lib",
        "arc_index": 0,
        "sense": "positive_unate",
        "trigger_transition": "none",
        "check_transition": "none",
        "sdf_condition": "",
        "delay_tables": [_table(), _table()],
        "slew_tables": [_table(), _table()],
    }


def _fingerprints(digit: str) -> dict[str, object]:
    value = digit * 64
    return {
        "schema_version": "2.0.0",
        "topology": value,
        "clock_constraint": value,
        "library": value,
        "corner": value,
        "parasitic": value,
        "timing_input": value,
    }


def _valid_trace() -> dict[str, object]:
    driver = {"kind": "port", "full_name": "in", "instance_name": "", "pin_name": ""}
    sink = {"kind": "instance_pin", "full_name": "u1:A", "instance_name": "u1", "pin_name": "A"}
    arc = {
        "source": copy.deepcopy(driver),
        "sink": copy.deepcopy(sink),
        "kind": "net",
        "owner_name": "n0",
        "library_source_port": "",
        "library_sink_port": "",
        "library_arcs": [],
        "parallel_ordinal": 0,
    }
    pre_state = {"instances": ["u1"], "nets": ["n0"], "nodes": [driver, sink], "arcs": [arc]}
    baseline = {
        "design_name": "contract_fixture",
        "fingerprints": _fingerprints("a"),
        "canonical_pre_state": copy.deepcopy(pre_state),
    }
    token = {
        "schema_version": 1,
        "case_id": "case",
        "profile_id": "profile",
        "seed": 42,
        "batch_ordinal": 1,
        "accepted_ordinal": 0,
    }
    edit = {
        "original_net": {"kind": "existing", "baseline_name": "n0"},
        "moved_sinks": [{"kind": "existing", "baseline_key": copy.deepcopy(sink)}],
        "library_cell": {
            "library_name": "fixture.lib",
            "cell_name": "BUF_X1",
            "input_port": "A",
            "output_port": "Y",
            "ordered_arc_signatures": [_signature()],
        },
        "placement_inputs": {"kind": "unplaced"},
        "rc_inputs": {"kind": "no_parasitic"},
        "creation_token": token,
    }
    pre_identity = {
        "generation": 0,
        "current_fingerprints": copy.deepcopy(baseline["fingerprints"]),
        "canonical_pre_state": copy.deepcopy(pre_state),
    }
    post_identity = {
        "generation": 1,
        "current_fingerprints": _fingerprints("b"),
        "canonical_pre_state": copy.deepcopy(pre_state),
    }
    return {
        "schema_version": "timing-edit-trace/1.0.0",
        "workload_kind": "synthetic",
        "seed": 42,
        "profile": "profile",
        "baseline": baseline,
        "source_artifacts": [],
        "batches": [
            {
                "ordinal": 1,
                "expected_pre_identity": pre_identity,
                "edits": [edit],
                "policy_receipt": {"disposition": "supported", "reasons": []},
                "expected_post_identity": post_identity,
            }
        ],
        "replay_command": {
            "executable": "ista_timing_edit_replay",
            "arguments": ["--trace", "{trace}", "--prefix", "{prefix}"],
        },
    }


def _expect_rejected(name: str, action: Callable[[], object]) -> None:
    try:
        action()
    except ValidationError:
        return
    raise AssertionError(f"negative self-test {name!r} was accepted")


def _run_self_test(schema: object) -> None:
    minimum = _valid_trace()
    validate_document(minimum, schema)

    real_design = copy.deepcopy(minimum)
    real_design["workload_kind"] = "real_design"
    real_design["source_artifacts"] = [
        {"artifact_role": "netlist", "logical_name": "fixture.v", "content_sha256": "c" * 64}
    ]
    validate_document(real_design, schema)

    negatives: list[tuple[str, dict[str, object]]] = []

    unknown = copy.deepcopy(minimum)
    unknown["future_field"] = True
    negatives.append(("unknown-key", unknown))

    for identifier in ("dense_node_id", "native_node_id"):
        native_id = copy.deepcopy(minimum)
        native_id["baseline"]["canonical_pre_state"]["nodes"][0][identifier] = 0
        negatives.append((identifier, native_id))

    version = copy.deepcopy(minimum)
    version["schema_version"] = "timing-edit-trace/2.0.0"
    negatives.append(("unknown-version", version))

    tag = copy.deepcopy(minimum)
    tag["batches"][0]["edits"][0]["placement_inputs"] = {"kind": "future"}
    negatives.append(("unknown-tag", tag))

    noncanonical = copy.deepcopy(minimum)
    noncanonical["baseline"]["canonical_pre_state"]["nodes"].reverse()
    negatives.append(("non-canonical", noncanonical))

    replay = copy.deepcopy(minimum)
    replay["replay_command"]["arguments"] = ["{trace}", "{prefix}"]
    negatives.append(("missing-replay-flags", replay))

    token_version = copy.deepcopy(minimum)
    token_version["batches"][0]["edits"][0]["creation_token"]["schema_version"] = 2
    negatives.append(("unknown-token-version", token_version))

    generation = copy.deepcopy(minimum)
    generation["batches"][0]["expected_post_identity"]["generation"] = 2
    negatives.append(("generation-gap", generation))

    bad_hash = copy.deepcopy(real_design)
    bad_hash["source_artifacts"][0]["content_sha256"] = "C" * 64
    negatives.append(("sha-format", bad_hash))

    receipt = copy.deepcopy(minimum)
    receipt["batches"][0]["edits"][0]["rc_inputs"] = {
        "kind": "parasitic",
        "policy": "full_with_record",
    }
    receipt["batches"][0]["policy_receipt"] = {
        "disposition": "full_with_record",
        "reasons": ["z_reason", "a_reason"],
    }
    negatives.append(("non-canonical-reasons", receipt))

    for name, document in negatives:
        _expect_rejected(name, lambda document=document: validate_document(document, schema))

    raw_negatives = (
        ("duplicate-key", lambda: loads_json('{"schema_version": 1, "schema_version": 1}')),
        ("non-finite", lambda: loads_json('{"value": NaN}')),
    )
    for name, action in raw_negatives:
        _expect_rejected(name, action)

    checks = 2 + len(negatives) + len(raw_negatives)
    print(f"timing-edit trace self-test: {checks} checks passed")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--schema",
        type=Path,
        default=SCHEMA_PATH,
        help=f"schema path (default: {SCHEMA_PATH.name})",
    )
    parser.add_argument(
        "--trace",
        action="append",
        type=Path,
        default=[],
        help="trace document to validate; may be repeated",
    )
    parser.add_argument(
        "--self-test", action="store_true", help="run positive and negative contract checks"
    )
    args = parser.parse_args(argv)

    if not args.self_test and not args.trace:
        parser.error("provide --trace or use --self-test")

    try:
        schema = load_json(args.schema)
        if args.self_test:
            _run_self_test(schema)
        for trace_path in args.trace:
            validate_document(load_json(trace_path), schema)
            print(f"valid: {trace_path}")
    except (ValidationError, AssertionError) as error:
        print(f"validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
