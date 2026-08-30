#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
# http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
#
# See the Mulan PSL v2 for more details.
# ***************************************************************************************
"""Negative validation suite and fake subprocess for the projection controller."""
# ruff: noqa: FBT001, FBT003

import argparse
import copy
import hashlib
import json
import secrets
import sys
import tempfile
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Any

from projection_canonical import (
    CanonicalDecodeError,
    CanonicalReader,
    _read_library_arc,
    _read_table,
    decode_oracle,
    decode_projection,
    validate_acyclic_oracle_shape,
    validate_oracle_shape,
)
from projection_contract import (
    CONTROLLER_SCHEMA,
    PROBE_SCHEMA,
    BaselineError,
    CaseDescriptor,
    ProjectionContractError,
    compute_case_fingerprint,
    load_case_descriptors,
    publish_atomic,
    validate_controller_result,
    validate_probe_record,
    write_json_exclusive,
)
from projection_controller import RunSlot, run_controller, run_probe_slot
from projection_wire_fixture import (
    fixture_bytes as _fixture_bytes,
)
from projection_wire_fixture import (
    invalid_second_variable_table_bytes,
    valid_library_arc_bytes,
)
from projection_wire_fixture import (
    oracle_bytes as _oracle_bytes,
)

_DEFAULT_MANIFEST = Path(__file__).resolve().parents[1] / "baseline" / "manifest.json"
_PROJECTION_FIELDS = (
    "node_count",
    "arc_count",
    "direct_projection",
    "decoded_projection",
    "native_graph_oracle",
    "independent_fifo_oracle",
    "projection_sha256",
    "oracle_sha256",
    "database_before_sha256",
    "database_after_sha256",
)
_EXPECTED_CASE_FINGERPRINTS = {
    "chain": "aaf890bdd02aa7a5179fdb9e1f9488068978115ce4db65bf0899e10afedd5bb1",
    "diamond": "40083946ec3c41879f80e4d2854705da834485bd2913c7b4ac34963882229b1b",
}


def _valid_record(
    descriptor: CaseDescriptor,
    repeat_index: int,
    nonce: str,
    *,
    payload_case: str | None = None,
) -> dict[str, Any]:
    projection, oracle, node_count, arc_count = _fixture_bytes(payload_case or descriptor.name)
    projection_sha = hashlib.sha256(projection).hexdigest()
    oracle_sha = hashlib.sha256(oracle).hexdigest()
    return {
        "schema": PROBE_SCHEMA,
        "run_nonce": nonce,
        "case": descriptor.name,
        "top": descriptor.top,
        "expected_clock": descriptor.expected_clock,
        "repeat_index": repeat_index,
        "input_case_sha256": descriptor.input_case_sha256,
        "node_count": node_count,
        "arc_count": arc_count,
        "direct_projection": projection.hex(),
        "decoded_projection": projection.hex(),
        "native_graph_oracle": oracle.hex(),
        "independent_fifo_oracle": oracle.hex(),
        "projection_sha256": projection_sha,
        "oracle_sha256": oracle_sha,
        "database_before_sha256": projection_sha,
        "database_after_sha256": projection_sha,
        "complete": True,
    }


def _expect_error(name: str, action: Callable[[], object]) -> None:
    try:
        action()
    except (BaselineError, CanonicalDecodeError, FileExistsError):
        return
    raise ProjectionContractError(f"negative self-test {name!r} was accepted")


def _summary(descriptors: tuple[CaseDescriptor, CaseDescriptor]) -> dict[str, Any]:
    cases: list[dict[str, Any]] = []
    for case_index, descriptor in enumerate(descriptors):
        records = []
        for repeat_index in range(3):
            nonce = f"{case_index + 1:02x}{repeat_index + 1:030x}"
            records.append(_valid_record(descriptor, repeat_index, nonce))
        cases.append({"case": descriptor.name, "repeats": records})
    return {
        "schema": CONTROLLER_SCHEMA,
        "requested_repeats": 3,
        "case_results": cases,
        "complete": True,
    }


def _mutated(value: Any, action: Callable[[Any], None]) -> Any:
    result = copy.deepcopy(value)
    action(result)
    return result


def _replace_payload(destination: dict[str, Any], source: dict[str, Any]) -> None:
    for field in _PROJECTION_FIELDS:
        destination[field] = source[field]


def _test_contract_validation(descriptors: tuple[CaseDescriptor, CaseDescriptor]) -> None:
    summary = _summary(descriptors)
    validate_controller_result(summary, descriptors)
    chain_record = summary["case_results"][0]["repeats"][0]
    descriptor = descriptors[0]
    validate_probe_record(chain_record, descriptor, 0, chain_record["run_nonce"])

    mutations: tuple[tuple[str, Callable[[Any], None]], ...] = (
        ("controller-missing", lambda item: item.pop("complete")),
        ("controller-extra", lambda item: item.__setitem__("extra", 1)),
        ("controller-version", lambda item: item.__setitem__("schema", "bad/1.0.0")),
        ("controller-incomplete", lambda item: item.__setitem__("complete", False)),
        ("bool-repeats", lambda item: item.__setitem__("requested_repeats", True)),
        ("small-repeats", lambda item: item.__setitem__("requested_repeats", 2)),
        ("missing-case", lambda item: item["case_results"].pop()),
        (
            "extra-case",
            lambda item: item["case_results"].append(copy.deepcopy(item["case_results"][0])),
        ),
        (
            "duplicate-case",
            lambda item: item["case_results"].__setitem__(
                1, copy.deepcopy(item["case_results"][0])
            ),
        ),
        ("missing-repeat", lambda item: item["case_results"][0]["repeats"].pop()),
        (
            "duplicate-repeat",
            lambda item: item["case_results"][0]["repeats"].__setitem__(
                1, copy.deepcopy(item["case_results"][0]["repeats"][0])
            ),
        ),
        (
            "wrong-top",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__("top", "wrong"),
        ),
        (
            "wrong-clock",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__(
                "expected_clock", "wrong"
            ),
        ),
        (
            "wrong-fingerprint",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__(
                "input_case_sha256", "0" * 64
            ),
        ),
        (
            "wrong-nonce",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__("run_nonce", "bad"),
        ),
        ("probe-missing", lambda item: item["case_results"][0]["repeats"][0].pop("arc_count")),
        ("probe-extra", lambda item: item["case_results"][0]["repeats"][0].__setitem__("extra", 1)),
        (
            "probe-version",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__("schema", "bad"),
        ),
        (
            "null-payload",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__(
                "direct_projection", None
            ),
        ),
        (
            "empty-payload",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__("direct_projection", ""),
        ),
        (
            "bad-hex",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__(
                "direct_projection", "0G"
            ),
        ),
        (
            "bool-node-count",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__("node_count", True),
        ),
        (
            "zero-arc-count",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__("arc_count", 0),
        ),
        (
            "projection-mismatch",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__(
                "decoded_projection", item["case_results"][1]["repeats"][0]["direct_projection"]
            ),
        ),
        (
            "oracle-mismatch",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__(
                "independent_fifo_oracle",
                item["case_results"][1]["repeats"][0]["native_graph_oracle"],
            ),
        ),
        (
            "projection-hash",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__(
                "projection_sha256", "0" * 64
            ),
        ),
        (
            "oracle-hash",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__(
                "oracle_sha256", "0" * 64
            ),
        ),
        (
            "before-hash",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__(
                "database_before_sha256", "0" * 64
            ),
        ),
        (
            "after-hash",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__(
                "database_after_sha256", "0" * 64
            ),
        ),
        (
            "probe-incomplete",
            lambda item: item["case_results"][0]["repeats"][0].__setitem__("complete", False),
        ),
        (
            "reused-nonce",
            lambda item: item["case_results"][1]["repeats"][0].__setitem__(
                "run_nonce", item["case_results"][0]["repeats"][0]["run_nonce"]
            ),
        ),
        (
            "same-payload-cross-case",
            lambda item: [
                _replace_payload(record, item["case_results"][0]["repeats"][0])
                for record in item["case_results"][1]["repeats"]
            ],
        ),
    )
    for name, mutation in mutations:
        _expect_error(
            name,
            lambda mutation=mutation: validate_controller_result(
                _mutated(summary, mutation), descriptors
            ),
        )

    projection, oracle, _nodes, _arcs = _fixture_bytes("chain")
    decoded_projection = decode_projection(projection)
    decode_oracle(oracle)

    library_reader = CanonicalReader(valid_library_arc_bytes(), "selftest.library_arc")
    library_arc = _read_library_arc(library_reader)
    library_reader.finish()
    if library_arc.library_name or library_arc.arc_index != -7:
        raise ProjectionContractError("signed library arc index was not preserved")
    _expect_error(
        "table-second-variable-without-first",
        lambda: _read_table(
            CanonicalReader(invalid_second_variable_table_bytes(), "selftest.invalid_table")
        ),
    )

    _expect_error("projection-trailing-bytes", lambda: decode_projection(projection + b"\x00"))
    _expect_error(
        "projection-unknown-enum",
        lambda: decode_projection(projection.replace(b"port", b"xxxx", 1)),
    )
    _expect_error(
        "parallel-ordinal-native-rank",
        lambda: decode_projection(_fixture_bytes("chain", mutation="swap_parallel_ordinals")[0]),
    )
    _expect_error(
        "active-indegree-roots",
        lambda: decode_projection(_fixture_bytes("chain", mutation="drop_roots")[0]),
    )
    _expect_error("oracle-trailing-bytes", lambda: decode_oracle(oracle + b"\x00"))

    canonical_names = tuple(node.key.full_name for node in decoded_projection.nodes)
    partial_cycle = decode_oracle(
        _oracle_bytes((canonical_names[0],), tuple((name, 0) for name in canonical_names))
    )
    validate_oracle_shape(decoded_projection, partial_cycle)
    signed_levels = decode_oracle(
        _oracle_bytes(
            (canonical_names[0],),
            tuple((name, -7 if index == 0 else 0) for index, name in enumerate(canonical_names)),
        )
    )
    validate_oracle_shape(decoded_projection, signed_levels)
    if signed_levels.native_node_levels[0][1] != -7:
        raise ProjectionContractError("signed native oracle level was not preserved")
    _expect_error(
        "partial-cycle-rejected-by-acyclic-smoke",
        lambda: validate_acyclic_oracle_shape(decoded_projection, partial_cycle),
    )
    _expect_error(
        "signed-level-rejected-by-acyclic-smoke",
        lambda: validate_acyclic_oracle_shape(decoded_projection, signed_levels),
    )
    _expect_error(
        "permuted-level-rows",
        lambda: decode_oracle(
            _oracle_bytes(
                (canonical_names[0],), tuple((name, 0) for name in reversed(canonical_names))
            )
        ),
    )


def _test_fingerprint(descriptors: tuple[CaseDescriptor, CaseDescriptor]) -> None:
    if descriptors[0].input_case_sha256 == descriptors[1].input_case_sha256:
        raise ProjectionContractError("valid chain/diamond fingerprints unexpectedly match")
    actual_known = {item.name: item.input_case_sha256 for item in descriptors}
    if actual_known != _EXPECTED_CASE_FINGERPRINTS:
        raise ProjectionContractError(
            f"input-case fingerprint known answers changed: {actual_known!r}"
        )
    descriptor = descriptors[0]
    fields = {
        "name": descriptor.name,
        "top": descriptor.top,
        "verilog": descriptor.verilog,
        "sdc": descriptor.sdc,
        "expected_clock": descriptor.expected_clock,
        "input_hashes": descriptor.input_hashes,
    }
    actual = compute_case_fingerprint(**fields)
    if actual != descriptor.input_case_sha256:
        raise ProjectionContractError("input-case fingerprint is not repeatable")
    for field in ("name", "top", "verilog", "sdc", "expected_clock"):
        changed = dict(fields)
        changed[field] = f"{changed[field]}_changed"
        if compute_case_fingerprint(**changed) == actual:
            raise ProjectionContractError(f"input-case fingerprint ignores {field}")
    changed_hashes = list(descriptor.input_hashes)
    changed_hashes[0] = (changed_hashes[0][0], "0" * 64)
    changed = dict(fields)
    changed["input_hashes"] = tuple(changed_hashes)
    if compute_case_fingerprint(**changed) == actual:
        raise ProjectionContractError("input-case fingerprint ignores input content hashes")
    _expect_error(
        "fingerprint-four-inputs",
        lambda: compute_case_fingerprint(**{**fields, "input_hashes": descriptor.input_hashes[:4]}),
    )


def _slot(root: Path, descriptor: CaseDescriptor, index: int) -> RunSlot:
    nonce = secrets.token_hex(16)
    work = root / f"slot-{index}-{nonce}"
    work.mkdir()
    return RunSlot(
        descriptor,
        index,
        nonce,
        work,
        work / f".projection-result-{nonce}.tmp",
        work / "projection-result.json",
    )


def _fake_command(mode: str) -> tuple[str, ...]:
    return sys.executable, str(Path(__file__).resolve()), "--fake-probe-mode", mode


def _test_subprocess_protocol(descriptor: CaseDescriptor, root: Path) -> None:
    valid_slot = _slot(root, descriptor, 0)
    validated = run_probe_slot(_fake_command("ok"), valid_slot, 10.0)
    if not valid_slot.final_result.is_file() or valid_slot.temporary_result.exists():
        raise ProjectionContractError("valid probe result was not atomically published")
    if validated.record["run_nonce"] != valid_slot.run_nonce:
        raise ProjectionContractError("valid subprocess nonce was not preserved")

    modes = (
        "exit0-no-result",
        "missing-temp",
        "nonzero",
        "stderr",
        "missing-key",
        "extra-key",
        "bad-version",
        "null-payload",
        "empty-payload",
        "bad-hex",
        "wrong-top",
        "wrong-clock",
        "wrong-fingerprint",
        "wrong-nonce",
        "projection-mismatch",
        "oracle-mismatch",
        "bad-hash",
        "incomplete",
        "direct-final",
    )
    for index, mode in enumerate(modes, start=1):
        slot = _slot(root, descriptor, index)
        _expect_error(
            f"subprocess-{mode}",
            lambda slot=slot, mode=mode: run_probe_slot(_fake_command(mode), slot, 10.0),
        )
    preexisting = _slot(root, descriptor, len(modes) + 1)
    write_json_exclusive(preexisting.final_result, {"complete": False})
    _expect_error(
        "preexisting-final",
        lambda: run_probe_slot(_fake_command("ok"), preexisting, 10.0),
    )
    stale = _slot(root, descriptor, len(modes) + 2)
    write_json_exclusive(stale.final_result, {"complete": True})
    _expect_error(
        "failed-probe-stale-complete-final",
        lambda: run_probe_slot(_fake_command("nonzero"), stale, 10.0),
    )
    _expect_error(
        "non-finite-timeout",
        lambda: run_probe_slot(
            _fake_command("ok"), _slot(root, descriptor, len(modes) + 3), float("nan")
        ),
    )


def _test_atomic_publication(root: Path) -> None:
    directory = root / "atomic"
    directory.mkdir()
    temporary = directory / ".summary.tmp"
    final = directory / "summary.json"
    write_json_exclusive(temporary, {"complete": True})
    publish_atomic(temporary, final, "self-test summary")
    if temporary.exists() or json.loads(final.read_text(encoding="utf-8")) != {"complete": True}:
        raise ProjectionContractError("atomic summary publication did not preserve content")
    second_temp = directory / ".second.tmp"
    write_json_exclusive(second_temp, {"complete": False})
    _expect_error("atomic-overwrite-final", lambda: publish_atomic(second_temp, final, "self-test"))
    _expect_error("exclusive-temp", lambda: write_json_exclusive(second_temp, {}))


def _test_end_to_end(manifest_path: Path, root: Path) -> None:
    run_root = root / "complete-runs"
    first = run_controller(
        probe_command=_fake_command("ok"),
        manifest_path=manifest_path,
        run_root=run_root,
        repeats=3,
        timeout_seconds=10.0,
    )
    second = run_controller(
        probe_command=_fake_command("ok"),
        manifest_path=manifest_path,
        run_root=run_root,
        repeats=3,
        timeout_seconds=10.0,
    )
    if (
        first.directory == second.directory
        or not first.summary_path.is_file()
        or not second.summary_path.is_file()
    ):
        raise ProjectionContractError(
            "controller did not create and publish two unique complete runs"
        )
    if tuple(first.directory.rglob("*.tmp")) or tuple(second.directory.rglob("*.tmp")):
        raise ProjectionContractError("successful controller run retained a temporary result")


def run_self_test(manifest_path: Path) -> None:
    descriptors = load_case_descriptors(manifest_path)
    _test_contract_validation(descriptors)
    _test_fingerprint(descriptors)
    with tempfile.TemporaryDirectory(prefix="ista-projection-controller-") as raw_directory:
        root = Path(raw_directory).resolve()
        _test_subprocess_protocol(descriptors[0], root)
        _test_atomic_publication(root)
        _test_end_to_end(manifest_path, root)


def _fake_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--fake-probe-mode", required=True)
    parser.add_argument("--tech-lef", type=Path, required=True)
    parser.add_argument("--cell-lef", type=Path, required=True)
    parser.add_argument("--liberty", type=Path, required=True)
    parser.add_argument("--verilog", type=Path, required=True)
    parser.add_argument("--sdc", type=Path, required=True)
    parser.add_argument("--top", required=True)
    parser.add_argument("--case-name", required=True)
    parser.add_argument("--expected-clock", required=True)
    parser.add_argument("--repeat-index", type=int, required=True)
    parser.add_argument("--input-case-sha256", required=True)
    parser.add_argument("--run-nonce", required=True)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--result-temp", type=Path, required=True)
    return parser


def _fake_probe(argv: Sequence[str]) -> int:
    arguments = _fake_parser().parse_args(argv)
    mode = arguments.fake_probe_mode
    descriptor = CaseDescriptor(
        arguments.case_name,
        arguments.top,
        str(arguments.verilog),
        str(arguments.sdc),
        arguments.expected_clock,
        (),
        (),
        arguments.input_case_sha256,
    )
    record = _valid_record(descriptor, arguments.repeat_index, arguments.run_nonce)
    if mode in {"exit0-no-result", "missing-temp"}:
        return 0
    if mode == "nonzero":
        return 7
    if mode == "stderr":
        print("forbidden diagnostic", file=sys.stderr)
        return 0
    if mode == "missing-key":
        record.pop("arc_count")
    elif mode == "extra-key":
        record["extra"] = True
    elif mode == "bad-version":
        record["schema"] = "ista-csr-projection-probe/9.9.9"
    elif mode == "null-payload":
        record["direct_projection"] = None
    elif mode == "empty-payload":
        record["direct_projection"] = ""
    elif mode == "bad-hex":
        record["direct_projection"] = "GG"
    elif mode == "wrong-top":
        record["top"] += "_wrong"
    elif mode == "wrong-clock":
        record["expected_clock"] += "_wrong"
    elif mode == "wrong-fingerprint":
        record["input_case_sha256"] = "0" * 64
    elif mode == "wrong-nonce":
        record["run_nonce"] = "0" * 32
    elif mode == "projection-mismatch":
        record["decoded_projection"] = _fixture_bytes("diamond")[0].hex()
    elif mode == "oracle-mismatch":
        record["independent_fifo_oracle"] = _fixture_bytes("diamond")[1].hex()
    elif mode == "bad-hash":
        record["projection_sha256"] = "0" * 64
    elif mode == "incomplete":
        record["complete"] = False
    elif mode == "direct-final":
        write_json_exclusive(arguments.run_dir / "projection-result.json", record)
    elif mode != "ok":
        raise ProjectionContractError(f"unknown fake probe mode: {mode}")
    write_json_exclusive(arguments.result_temp, record)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if "--fake-probe-mode" in arguments:
        return _fake_probe(arguments)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=_DEFAULT_MANIFEST)
    parsed = parser.parse_args(arguments)
    try:
        run_self_test(parsed.manifest)
        return 0
    except (BaselineError, OSError) as exc:
        print(f"projection controller self-test failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
