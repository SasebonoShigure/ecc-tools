#!/usr/bin/env python3
"""Adversarial self-test for the Step 4 contract and fresh-process controller."""
# ruff: noqa: FBT003

import argparse
import copy
import sys
import tempfile
from collections.abc import Callable, Sequence
from dataclasses import replace
from pathlib import Path
from typing import Any

import reference_input_contract as contract
import reference_input_controller as controller
import reference_input_controller_negative_cases as negative_cases
import reference_input_recompute_graph as graph_oracle
import reference_input_recompute_transfer as transfer_oracle
import reference_input_wire_fixture as fixture

_DEEP_CANDIDATE_BYTE_COUNT = 3409
_DEEP_RAW_BYTE_COUNT = 1226


def _known_answer_bytes(
    payload_hex: str,
    expected_byte_count: int,
    label: str,
) -> bytes:
    if (
        len(payload_hex) != expected_byte_count * 2
        or payload_hex != payload_hex.lower()
        or any(character not in "0123456789abcdef" for character in payload_hex)
    ):
        raise AssertionError(f"{label} literal is not strict lower hex")
    payload = bytes.fromhex(payload_hex)
    if len(payload) != expected_byte_count:
        raise AssertionError(f"{label} known-answer byte count differs")
    return payload


def _deep_wire_bytes() -> tuple[bytes, bytes]:
    candidate = _known_answer_bytes(
        fixture.DEEP_CANDIDATE_HEX,
        _DEEP_CANDIDATE_BYTE_COUNT,
        "deep candidate",
    )
    raw = _known_answer_bytes(
        fixture.DEEP_RAW_HEX,
        _DEEP_RAW_BYTE_COUNT,
        "deep raw",
    )
    return candidate, raw


def _valid_record(
    descriptor: contract.CaseDescriptor,
    repeat_index: int,
    nonce: str,
) -> dict[str, Any]:
    candidate, raw = fixture.fixture_case(descriptor.name)
    identity = contract.fingerprint_oracle.recompute_identity(candidate, contract.SESSION_TOKEN, 0)
    candidate = fixture.with_identity(candidate, identity)
    candidate_bytes = fixture.encode_candidate_wire(candidate)
    raw_bytes = fixture.encode_raw_wire(raw)
    canaries = [
        {
            "family": family,
            "pristine_typed_bytes_hex": fixture.encode_canary_wire(
                family,
                descriptor.name,
                repeat_index,
                nonce,
                after=False,
                pristine=True,
            ).hex(),
            "before_typed_bytes_hex": fixture.encode_canary_wire(
                family,
                descriptor.name,
                repeat_index,
                nonce,
                after=False,
            ).hex(),
            "after_typed_bytes_hex": fixture.encode_canary_wire(
                family,
                descriptor.name,
                repeat_index,
                nonce,
                after=True,
            ).hex(),
        }
        for family in contract.FAMILY_TOKENS
    ]
    return {
        "schema": contract.PROBE_SCHEMA,
        "name": descriptor.name,
        "top": descriptor.top,
        "expected_clock": descriptor.expected_clock,
        "context": dict(contract.CANONICAL_CONTEXT),
        "repeat_index": repeat_index,
        "run_nonce": nonce,
        "session_token": contract.SESSION_TOKEN,
        "pre_candidate_wire_hex": candidate_bytes.hex(),
        "post_candidate_wire_hex": candidate_bytes.hex(),
        "pre_raw_projection_hex": raw_bytes.hex(),
        "post_raw_projection_hex": raw_bytes.hex(),
        "canaries": canaries,
        "complete": True,
    }


def _deep_record(
    descriptor: contract.CaseDescriptor,
    repeat_index: int,
    nonce: str,
) -> dict[str, Any]:
    record = _valid_record(descriptor, repeat_index, nonce)
    candidate_bytes, raw_bytes = _deep_wire_bytes()
    raw = contract.raw_wire.decode_raw_wire(raw_bytes)
    if len(raw.instances) != 1 or len(raw.referenced_library_cells) != 1:
        raise AssertionError("deep raw role fixture inventory differs")
    raw_instance = raw.instances[0]
    raw_cell = raw.referenced_library_cells[0]
    if (
        (
            raw_instance.clock_pin_name,
            raw_instance.data_pin_name,
            raw_instance.output_pin_name,
        )
        != ("", "", "u:Y")
        or raw_instance.output_pin_name not in raw_instance.ordered_pin_names
        or raw_cell.payload is None
        or (
            raw_cell.payload.clock_pin_name,
            raw_cell.payload.data_pin_name,
            raw_cell.payload.output_pin_name,
        )
        != ("", "", "Y")
    ):
        raise AssertionError("deep raw native-full/cell-local role split differs")
    record["pre_candidate_wire_hex"] = candidate_bytes.hex()
    record["post_candidate_wire_hex"] = candidate_bytes.hex()
    record["pre_raw_projection_hex"] = raw_bytes.hex()
    record["post_raw_projection_hex"] = raw_bytes.hex()
    record["canaries"] = [
        {
            "family": family,
            "pristine_typed_bytes_hex": fixture.encode_canary_wire(
                family,
                descriptor.name,
                repeat_index,
                nonce,
                after=False,
                pristine=True,
                raw_override=raw,
            ).hex(),
            "before_typed_bytes_hex": fixture.encode_canary_wire(
                family,
                descriptor.name,
                repeat_index,
                nonce,
                after=False,
                raw_override=raw,
            ).hex(),
            "after_typed_bytes_hex": fixture.encode_canary_wire(
                family,
                descriptor.name,
                repeat_index,
                nonce,
                after=True,
                raw_override=raw,
            ).hex(),
        }
        for family in contract.FAMILY_TOKENS
    ]
    return record


def _expect_error(name: str, action: Callable[[], object]) -> None:
    try:
        action()
    except (contract.ReferenceInputContractError, transfer_oracle.CanonicalError):
        return
    raise AssertionError(f"negative self-test {name!r} unexpectedly succeeded")


def _mutated(value: Any, action: Callable[[Any], None]) -> Any:
    result = copy.deepcopy(value)
    action(result)
    return result


def _validate(
    record: dict[str, Any],
    descriptor: contract.CaseDescriptor,
    repeat_index: int,
    nonce: str,
) -> contract.ValidatedProbe:
    return contract.validate_probe_record(record, descriptor, repeat_index, nonce)


def _test_probe_contract(descriptor: contract.CaseDescriptor) -> None:
    nonce = "1" * 32
    record = _valid_record(descriptor, 0, nonce)
    validated = _validate(record, descriptor, 0, nonce)
    assert validated.touched_families == contract.FAMILY_TOKENS
    mutations: tuple[tuple[str, Callable[[dict[str, Any]], None]], ...] = (
        ("wrong-schema", lambda item: item.__setitem__("schema", "wrong")),
        ("wrong-case", lambda item: item.__setitem__("name", "diamond")),
        ("wrong-top", lambda item: item.__setitem__("top", "wrong")),
        ("wrong-clock", lambda item: item.__setitem__("expected_clock", "wrong")),
        ("wrong-repeat", lambda item: item.__setitem__("repeat_index", 1)),
        ("bool-repeat", lambda item: item.__setitem__("repeat_index", False)),
        ("wrong-nonce", lambda item: item.__setitem__("run_nonce", "2" * 32)),
        ("wrong-token", lambda item: item.__setitem__("session_token", 2)),
        ("bool-token", lambda item: item.__setitem__("session_token", True)),
        ("incomplete", lambda item: item.__setitem__("complete", False)),
        ("extra-canned-hash", lambda item: item.__setitem__("candidate_sha256", "0" * 64)),
        ("wrong-context", lambda item: item["context"].__setitem__("temperature", "26")),
        ("extra-context", lambda item: item["context"].__setitem__("extra", "x")),
        ("candidate-post-drift", lambda item: item.__setitem__("post_candidate_wire_hex", "00")),
        ("raw-post-drift", lambda item: item.__setitem__("post_raw_projection_hex", "00")),
        (
            "candidate-trailing",
            lambda item: item.__setitem__(
                "pre_candidate_wire_hex", item["pre_candidate_wire_hex"] + "00"
            ),
        ),
        (
            "raw-truncated",
            lambda item: item.__setitem__(
                "pre_raw_projection_hex", item["pre_raw_projection_hex"][:-2]
            ),
        ),
        ("missing-canary", lambda item: item["canaries"].pop()),
        ("duplicate-canary", lambda item: item["canaries"].__setitem__(1, item["canaries"][0])),
    )
    for name, mutation in mutations:
        _expect_error(
            name,
            lambda mutation=mutation: _validate(_mutated(record, mutation), descriptor, 0, nonce),
        )
    for family_index, family in enumerate(contract.FAMILY_TOKENS):
        _expect_error(
            f"incomplete-{family}",
            lambda family_index=family_index: _validate(
                _mutated(
                    record,
                    lambda item: item["canaries"][family_index].__setitem__(
                        "after_typed_bytes_hex",
                        item["canaries"][family_index]["before_typed_bytes_hex"],
                    ),
                ),
                descriptor,
                0,
                nonce,
            ),
        )
    fallback = contract.FAMILY_TOKENS.index("arc_timing_arc_fallback_provenance")
    fallback_bytes = bytearray.fromhex(record["canaries"][fallback]["after_typed_bytes_hex"])
    fallback_bytes[-3] = 1
    _expect_error(
        "ordinary-fallback-behavior-drift",
        lambda: _validate(
            _mutated(
                record,
                lambda item: item["canaries"][fallback].__setitem__(
                    "after_typed_bytes_hex", fallback_bytes.hex()
                ),
            ),
            descriptor,
            0,
            nonce,
        ),
    )


def _record_with_candidate(
    descriptor: contract.CaseDescriptor,
    nonce: str,
    mutation: Callable[[object], object],
) -> dict[str, Any]:
    record = _valid_record(descriptor, 0, nonce)
    candidate, _raw = fixture.fixture_case(descriptor.name)
    candidate = mutation(candidate)
    identity = contract.fingerprint_oracle.recompute_identity(candidate, 1, 0)
    candidate = fixture.with_identity(candidate, identity)
    payload = fixture.encode_candidate_wire(candidate).hex()
    record["pre_candidate_wire_hex"] = payload
    record["post_candidate_wire_hex"] = payload
    return record


def _test_model_negatives(descriptor: contract.CaseDescriptor) -> None:
    nonce = "3" * 32
    _expect_error(
        "derived-start-mismatch",
        lambda: _validate(
            _record_with_candidate(
                descriptor,
                nonce,
                lambda candidate: replace(
                    candidate,
                    nodes=(
                        replace(candidate.nodes[0], is_startpoint=False),
                        *candidate.nodes[1:],
                    ),
                ),
            ),
            descriptor,
            0,
            nonce,
        ),
    )
    record = _valid_record(descriptor, 0, nonce)
    candidate, _raw = fixture.fixture_case(descriptor.name)
    identity = contract.fingerprint_oracle.recompute_identity(candidate, 1, 0)
    bad_identity = replace(identity, graph_generation=1)
    payload = fixture.encode_candidate_wire(fixture.with_identity(candidate, bad_identity)).hex()
    record["pre_candidate_wire_hex"] = payload
    record["post_candidate_wire_hex"] = payload
    _expect_error("wrong-generation", lambda: _validate(record, descriptor, 0, nonce))
    record = _valid_record(descriptor, 0, nonce)
    raw_payload = bytearray.fromhex(record["pre_raw_projection_hex"])
    raw_payload[-1] = 1
    record["pre_raw_projection_hex"] = raw_payload.hex()
    record["post_raw_projection_hex"] = raw_payload.hex()
    _expect_error("raw-candidate-mismatch", lambda: _validate(record, descriptor, 0, nonce))


def _test_parallel_closure_reuse() -> None:
    candidate_bytes, raw_bytes = _deep_wire_bytes()
    candidate = contract.candidate_wire.decode_candidate_wire(candidate_bytes)
    raw = contract.raw_wire.decode_raw_wire(raw_bytes)
    facts = contract.graph_oracle.recompute_graph(candidate, raw)
    raw_cells = transfer_oracle._validate_library(candidate, raw)
    cell_arc, raw_cell_arc = candidate.arcs[1], facts.raw_arc_by_rank[1]
    original = replace(cell_arc, native_traversal_rank=11)
    clone = replace(
        cell_arc,
        arc_id=3,
        key=replace(cell_arc.key, parallel_ordinal=1),
        native_traversal_rank=28,
    )
    candidate = replace(candidate, arcs=(candidate.arcs[0], original, candidate.arcs[2], clone))
    facts = replace(
        facts,
        raw_arc_by_rank=(
            facts.raw_arc_by_rank[0],
            replace(raw_cell_arc, native_rank=11),
            facts.raw_arc_by_rank[2],
            replace(raw_cell_arc, native_rank=28),
        ),
    )
    transfer_oracle._graph_transfers(candidate, raw_cells, facts)

    def reject(label: str, changed_candidate=candidate, changed_cells=raw_cells) -> None:
        _expect_error(
            label, lambda: transfer_oracle._graph_transfers(changed_candidate, changed_cells, facts)
        )

    bad_signature = replace(clone.key.library_arcs[0], arc_index=99)
    reject(
        "parallel-closure-signature",
        replace(
            candidate,
            arcs=(
                *candidate.arcs[:3],
                replace(clone, key=replace(clone.key, library_arcs=(bad_signature,))),
            ),
        ),
    )
    reject(
        "parallel-closure-transfer",
        replace(
            candidate,
            arcs=(*candidate.arcs[:3], replace(clone, transfer=replace(clone.transfer, delay=2.0))),
        ),
    )
    cell_name = candidate.instances[0].cell_name
    raw_cell, payload = raw_cells[cell_name], raw_cells[cell_name].payload
    assert payload is not None
    missing = dict(raw_cells)
    missing[cell_name] = replace(
        raw_cell,
        payload=replace(
            payload, cell_arcs=(replace(payload.cell_arcs[0], source_port="Y", sink_port="A"),)
        ),
    )
    reject("parallel-closure-missing", changed_cells=missing)
    ambiguous = dict(raw_cells)
    ambiguous[cell_name] = replace(
        raw_cell, payload=replace(payload, cell_arcs=(payload.cell_arcs[0], payload.cell_arcs[0]))
    )
    reject("parallel-closure-ambiguous", changed_cells=ambiguous)


def _test_deep_child(descriptor: contract.CaseDescriptor, root: Path) -> None:
    nonce = "4" * 32
    record = _deep_record(descriptor, 0, nonce)
    validated = _validate(record, descriptor, 0, nonce)
    candidate_bytes, raw_bytes = _deep_wire_bytes()
    candidate = contract.candidate_wire.decode_candidate_wire(candidate_bytes)
    raw = contract.raw_wire.decode_raw_wire(raw_bytes)
    if validated.candidate != candidate or validated.raw != raw:
        raise AssertionError("deep known-answer record did not validate as its decoded DTOs")
    if len(candidate.arcs) != 3 or len(raw.arcs) != 3:
        raise AssertionError("deep known-answer graph inventory differs")
    semantic_order = tuple(
        (arc.key.source.full_name, arc.key.sink.full_name) for arc in candidate.arcs
    )
    if semantic_order != (("in", "u:A"), ("u:A", "u:Y"), ("u:Y", "out")) or tuple(
        arc.native_traversal_rank for arc in candidate.arcs
    ) != (0, 1, 2):
        raise AssertionError("deep unequal-base ArcKey semantic/native order differs")
    base_lengths = tuple(
        len(contract.graph_oracle._arc_base_bytes(arc.key)) for arc in candidate.arcs
    )
    if (
        len(set(base_lengths)) < 2
        or tuple(
            sorted(candidate.arcs, key=lambda arc: contract.graph_oracle._arc_key_bytes(arc.key))
        )
        != candidate.arcs
    ):
        raise AssertionError("deep unequal-base ArcKeys do not follow direct canonical bytes")
    if (
        tuple(arc.arc_id for arc in candidate.arcs) != (0, 1, 2)
        or tuple(lookup.arc_id for lookup in candidate.arc_ids_by_key) != (0, 1, 2)
        or candidate.out_arc_ids != (0, 1, 2)
        or candidate.in_arc_ids != (2, 0, 1)
    ):
        raise AssertionError("deep canonical ArcIds/lookup/CSR remap differs")
    cell_arc = candidate.arcs[1]
    if cell_arc.native_traversal_rank != 1:
        raise AssertionError("deep known-answer cell arc rank differs")
    raw_cell_arc = raw.arcs[cell_arc.native_traversal_rank]
    if (
        cell_arc.kind != fixture.model.ArcKind.CELL
        or raw_cell_arc.kind != fixture.model.ArcKind.CELL
        or (
            cell_arc.key.source.full_name,
            cell_arc.key.sink.full_name,
            cell_arc.key.library_source_port,
            cell_arc.key.library_sink_port,
        )
        != (
            raw_cell_arc.source,
            raw_cell_arc.sink,
            raw_cell_arc.library_source_port,
            raw_cell_arc.library_sink_port,
        )
    ):
        raise AssertionError("deep raw/candidate cell-arc pairing differs")
    if len(cell_arc.transfer.children) != 1:
        raise AssertionError("deep known-answer child timing-arc inventory differs")
    child = cell_arc.transfer.children[0]
    if (
        child.signature.sdf_condition != "A == 1"
        or child.capacitance_unit_scale != 2.0
        or cell_arc.output_load != ((0.75, 0.75), (0.75, 0.75))
    ):
        raise AssertionError("deep conditional/load conversion fields differ")
    slot = _slot(root, descriptor, 0)
    fresh = controller.run_probe_slot(_fake_command("deep-child"), slot, 30.0)
    if fresh.candidate != candidate or fresh.raw != raw:
        raise AssertionError("fresh deep-child probe differs from the known-answer DTOs")


def _summary(
    descriptors: tuple[contract.CaseDescriptor, contract.CaseDescriptor],
) -> dict[str, Any]:
    cases = []
    for descriptor in descriptors:
        rows = []
        for repeat_index in range(3):
            rows.append(
                {
                    "repeat_index": repeat_index,
                    "session_token": 1,
                    "touched_families": list(contract.FAMILY_TOKENS),
                }
            )
        cases.append({"case": descriptor.name, "repeats": rows})
    return {
        "schema": contract.CONTROLLER_SCHEMA,
        "requested_repeats": 3,
        "case_results": cases,
        "complete": True,
    }


def _test_summary(
    descriptors: tuple[contract.CaseDescriptor, contract.CaseDescriptor],
) -> None:
    summary = _summary(descriptors)
    contract.validate_controller_result(summary, descriptors)
    mutations: tuple[tuple[str, Callable[[dict[str, Any]], None]], ...] = (
        ("summary-extra", lambda item: item.__setitem__("extra", True)),
        ("summary-incomplete", lambda item: item.__setitem__("complete", False)),
        ("summary-bool-repeat", lambda item: item.__setitem__("requested_repeats", True)),
        ("summary-too-few", lambda item: item.__setitem__("requested_repeats", 2)),
        ("summary-case-missing", lambda item: item["case_results"].pop()),
        ("summary-case-order", lambda item: item["case_results"].reverse()),
        ("summary-repeat-missing", lambda item: item["case_results"][0]["repeats"].pop()),
        (
            "summary-repeat-index",
            lambda item: item["case_results"][0]["repeats"][1].__setitem__("repeat_index", 0),
        ),
        (
            "summary-touch-order",
            lambda item: item["case_results"][0]["repeats"][0]["touched_families"].reverse(),
        ),
    )
    for name, mutation in mutations:
        _expect_error(
            name,
            lambda mutation=mutation: contract.validate_controller_result(
                _mutated(summary, mutation), descriptors
            ),
        )


def _slot(root: Path, descriptor: contract.CaseDescriptor, index: int) -> controller.RunSlot:
    nonce = f"{index + 1:032x}"
    work = root / f"slot-{index}"
    work.mkdir(parents=True)
    return controller.RunSlot(
        descriptor,
        index,
        nonce,
        work,
        work / ".result.tmp",
        work / "reference-input-result.json",
    )


def _test_direct_fresh_equality(
    descriptors: tuple[contract.CaseDescriptor, contract.CaseDescriptor],
    root: Path,
) -> None:
    slots: list[controller.RunSlot] = []
    validated: list[contract.ValidatedProbe] = []
    for descriptor in descriptors:
        for repeat_index in range(3):
            slot = _slot(root / descriptor.name, descriptor, repeat_index)
            slots.append(slot)
            validated.append(
                _validate(
                    _valid_record(descriptor, repeat_index, slot.run_nonce),
                    descriptor,
                    repeat_index,
                    slot.run_nonce,
                )
            )
    controller._build_summary(descriptors, 3, tuple(slots), tuple(validated))

    byte_drift = list(validated)
    byte_drift[1] = replace(
        byte_drift[1],
        candidate_bytes=byte_drift[1].candidate_bytes + b"\x00",
    )
    _expect_error(
        "fresh-canonical-byte-drift",
        lambda: controller._build_summary(descriptors, 3, tuple(slots), tuple(byte_drift)),
    )

    decoded_drift = list(validated)
    decoded_drift[1] = replace(decoded_drift[1], raw=validated[3].raw)
    _expect_error(
        "fresh-decoded-value-drift",
        lambda: controller._build_summary(descriptors, 3, tuple(slots), tuple(decoded_drift)),
    )

    same_candidate = list(validated)
    for index in range(3, 6):
        same_candidate[index] = replace(
            same_candidate[index],
            candidate=validated[0].candidate,
            candidate_bytes=validated[0].candidate_bytes,
        )
    _expect_error(
        "chain-diamond-candidate-equality",
        lambda: controller._build_summary(descriptors, 3, tuple(slots), tuple(same_candidate)),
    )


def _fake_command(mode: str) -> tuple[str, ...]:
    return (sys.executable, str(Path(__file__).resolve()), "--fake-probe", "--mode", mode)


def _test_subprocess_protocol(descriptor: contract.CaseDescriptor, root: Path) -> None:
    valid_slot = _slot(root, descriptor, 0)
    result = controller.run_probe_slot(_fake_command("valid"), valid_slot, 30.0)
    assert valid_slot.final_result.is_file()
    assert result.record["complete"] is True
    for index, mode in enumerate(
        ("exit", "stderr", "stdout", "no-temp", "wrong-echo", "canned", "direct-final"),
        start=1,
    ):
        slot = _slot(root, descriptor, index)
        _expect_error(
            f"subprocess-{mode}",
            lambda mode=mode, slot=slot: controller.run_probe_slot(_fake_command(mode), slot, 30.0),
        )
        assert slot.final_result.exists() is (mode == "direct-final")


def _test_atomic(root: Path) -> None:
    temporary = root / "atomic.tmp"
    final = root / "atomic.json"
    contract.write_json_exclusive(temporary, {"complete": True})
    contract.publish_atomic(temporary, final, "selftest")
    assert final.is_file() and not temporary.exists()
    _expect_error("exclusive-existing", lambda: contract.write_json_exclusive(final, {}))
    replacement = root / "replacement.tmp"
    contract.write_json_exclusive(replacement, {})
    _expect_error(
        "atomic-existing-final",
        lambda: contract.publish_atomic(replacement, final, "selftest"),
    )


def _test_end_to_end(manifest: Path, root: Path) -> None:
    result = controller.run_controller(
        manifest,
        _fake_command("valid"),
        root / "runs",
        3,
        30.0,
    )
    assert result.summary_path.is_file()
    assert not (result.directory / ".reference-input-summary.tmp").exists()
    descriptors = contract.load_case_descriptors(manifest)
    assert contract.validate_controller_result(result.summary, descriptors) == result.summary


def _test_adjacency_slot_order(descriptors: Sequence[contract.CaseDescriptor]) -> None:
    for descriptor in descriptors:
        candidate, raw = fixture.fixture_case(descriptor.name)
        row = next((node for node in raw.nodes if len(node.outgoing_arc_ranks) > 1), None)
        if row is not None:
            break
    else:
        raise AssertionError("fanout fixture missing")
    names = {node.key.full_name: node.node_id for node in candidate.nodes}
    changed_raw = replace(
        raw,
        nodes=tuple(
            replace(node, outgoing_arc_ranks=tuple(reversed(node.outgoing_arc_ranks)))
            if node == row else node
            for node in raw.nodes
        ),
    )
    node_id = names[row.full_name]
    begin, end = candidate.out_row_offsets[node_id : node_id + 2]
    changed_candidate = replace(
        candidate,
        out_arc_ids=(
            candidate.out_arc_ids[:begin]
            + tuple(reversed(candidate.out_arc_ids[begin:end]))
            + candidate.out_arc_ids[end:]
        ),
    )
    graph_oracle._validate_arcs(changed_candidate, changed_raw, names)
    _expect_error(
        "adjacency-order-corruption",
        lambda: graph_oracle._validate_arcs(candidate, changed_raw, names),
    )


def run_self_test(controller_path: Path, manifest: Path) -> None:
    if controller_path.resolve() != Path(controller.__file__).resolve():
        raise AssertionError("--controller does not name the imported controller")
    descriptors = contract.load_case_descriptors(manifest)
    _test_adjacency_slot_order(descriptors)
    _test_probe_contract(descriptors[0])
    _test_parallel_closure_reuse()
    _test_model_negatives(descriptors[0])
    _test_summary(descriptors)
    negative_cases.run_negative_matrix(manifest)
    with tempfile.TemporaryDirectory(prefix="ista-reference-input-selftest-") as directory:
        root = Path(directory)
        _test_direct_fresh_equality(descriptors, root / "fresh-equality")
        _test_deep_child(descriptors[0], root / "deep-child")
        _test_subprocess_protocol(descriptors[0], root / "protocol")
        _test_atomic(root)
        _test_end_to_end(manifest, root)


def _fake_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--fake-probe", action="store_true")
    parser.add_argument("--mode", required=True)
    for name in ("tech-lef", "cell-lef", "liberty", "verilog", "sdc"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--top", required=True)
    parser.add_argument("--case-name", choices=contract.CASE_ORDER, required=True)
    parser.add_argument("--expected-clock", required=True)
    parser.add_argument("--liberty-corner", required=True)
    parser.add_argument("--temperature", required=True)
    parser.add_argument("--rc-corner", required=True)
    parser.add_argument("--repeat-index", type=int, required=True)
    parser.add_argument("--session-token", type=int, required=True)
    parser.add_argument("--run-nonce", required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--result-temp", type=Path, required=True)
    return parser


def _fake_probe(argv: Sequence[str]) -> int:
    arguments = _fake_parser().parse_args(argv)
    if arguments.mode == "exit":
        return 7
    if arguments.mode == "stderr":
        sys.stderr.write("forbidden stderr")
        return 0
    if arguments.mode == "stdout":
        print("forbidden stdout")
        return 0
    if arguments.mode == "no-temp":
        return 0
    descriptor = type(
        "Descriptor",
        (),
        {
            "name": arguments.case_name,
            "top": arguments.top,
            "expected_clock": arguments.expected_clock,
        },
    )()
    record_factory = _deep_record if arguments.mode == "deep-child" else _valid_record
    record = record_factory(descriptor, arguments.repeat_index, arguments.run_nonce)
    record["context"] = {
        "liberty_corner": arguments.liberty_corner,
        "temperature": arguments.temperature,
        "rc_corner": arguments.rc_corner,
    }
    record["session_token"] = arguments.session_token
    if arguments.mode == "wrong-echo":
        record["top"] = "wrong"
    elif arguments.mode == "canned":
        record["candidate_sha256"] = "0" * 64
    elif arguments.mode == "direct-final":
        contract.write_json_exclusive(arguments.run_dir / "reference-input-result.json", record)
    contract.write_json_exclusive(arguments.result_temp, record)
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--controller", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments_list = list(sys.argv[1:] if argv is None else argv)
    if "--fake-probe" in arguments_list:
        return _fake_probe(arguments_list)
    arguments = _parser().parse_args(arguments_list)
    run_self_test(arguments.controller, arguments.manifest.resolve())
    print("reference-input controller self-test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
