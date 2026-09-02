#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# ***************************************************************************************
"""Raw-byte and semantic negative validation for the CSR session controller."""
# ruff: noqa: FBT001, FBT003

import copy
import hashlib
import json
import os
import sys
import tempfile
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Any

from csr_session_contract import (
    CONTROLLER_SCHEMA,
    PROBE_SCHEMA,
    SessionContractError,
    _actual_default_semantics,
    load_default_compat,
    validate_controller_result,
    validate_default_records,
    validate_session_probe_record,
)
from projection_contract import (
    BaselineError,
    CaseDescriptor,
    load_case_descriptors,
    load_json,
)
from projection_wire_fixture import fixture_bytes

_FINGERPRINT_FIELDS = (
    "topology_fingerprint",
    "clock_constraint_fingerprint",
    "library_fingerprint",
    "corner_fingerprint",
    "parasitic_fingerprint",
    "timing_input_fingerprint",
)


def _digest(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _identity(case: str, token: int) -> dict[str, Any]:
    result: dict[str, Any] = {
        "fingerprint_schema_version": "1.0.0",
        "session_token": token,
        "graph_generation": 0,
    }
    for field in _FINGERPRINT_FIELDS:
        result[field] = _digest(f"{case}:{field}")
    return result


def _extent(logical: int) -> dict[str, int]:
    return {"logical_size": logical, "capacity": logical}


def _observation(
    case: str,
    token: int,
    state: str,
    counts: tuple[int, int, int],
) -> dict[str, Any]:
    nodes, arcs, levels = counts
    return {
        "schema_version": "1.0.0",
        "state": state,
        "identity": _identity(case, token),
        "counts": {"nodes": nodes, "arcs": arcs, "levels": levels},
        "buffers": {
            "node_keys": _extent(nodes),
            "arc_keys": _extent(arcs),
            "out_row_offsets": _extent(nodes + 1),
            "out_arc_ids": _extent(arcs),
            "in_row_offsets": _extent(nodes + 1),
            "in_arc_ids": _extent(arcs),
            "level_offsets": _extent(levels + 1),
            "level_nodes": _extent(nodes),
            "node_to_level": _extent(nodes),
        },
        "capacity": {"last_action": "initial_allocate", "event_count": 1},
        "support": {"reference_supported": True, "unsupported_reasons": []},
        "timing_ms": {"export": 0.0, "reference": None, "scatter": None},
        "host_bytes": {"logical": 64, "capacity": 96},
        "last_rejection": None,
        "last_invalidation": None,
    }


def _session(
    case: str,
    token: int,
    counts: tuple[int, int, int],
) -> dict[str, Any]:
    active = _observation(case, token, "ready", counts)
    released = copy.deepcopy(active)
    released["state"] = "released"
    return {
        "session_token": token,
        "active": active,
        "released": released,
        "oracle": {
            "source_identity": copy.deepcopy(active["identity"]),
            "node_count": counts[0],
            "edge_count": counts[1],
            "level_count": counts[2],
            "projection_wire_hex": f"{case}:projection".encode().hex(),
            "fifo_wire_hex": f"{case}:fifo".encode().hex(),
        },
    }


def _record(
    descriptor: CaseDescriptor,
    repeat_index: int,
    nonce: str,
) -> dict[str, Any]:
    case_index = 0 if descriptor.name == "chain" else 1
    counts = (3, 2, 3) if case_index == 0 else (4, 4, 3)
    return {
        "schema": PROBE_SCHEMA,
        "run_nonce": nonce,
        "case": descriptor.name,
        "repeat_index": repeat_index,
        "full_observation": None,
        "sessions": [
            _session(descriptor.name, case_index * 10 + 1, counts),
            _session(descriptor.name, case_index * 10 + 2, counts),
        ],
        "complete": True,
    }


def _summary(descriptors: Sequence[CaseDescriptor]) -> dict[str, Any]:
    default_results: list[dict[str, Any]] = []
    case_results: list[dict[str, Any]] = []
    for case_index, descriptor in enumerate(descriptors):
        default_results.append(
            {
                "case": descriptor.name,
                "projection_wire_hex": f"{descriptor.name}:projection".encode().hex(),
                "fifo_wire_hex": f"{descriptor.name}:fifo".encode().hex(),
            }
        )
        records = []
        for repeat_index in range(3):
            nonce = f"{case_index + 1:02x}{repeat_index + 1:030x}"
            records.append(_record(descriptor, repeat_index, nonce))
        case_results.append(
            {
                "case": descriptor.name,
                "repeats": records,
            }
        )
    return {
        "schema": CONTROLLER_SCHEMA,
        "requested_repeats": 3,
        "default_compat_results": default_results,
        "case_results": case_results,
        "complete": True,
    }


def _mutated(value: Any, action: Callable[[Any], None]) -> Any:
    result = copy.deepcopy(value)
    action(result)
    return result


def _expect_error(name: str, action: Callable[[], object]) -> None:
    try:
        action()
    except (BaselineError, FileExistsError, OSError, ValueError):
        return
    raise SessionContractError(f"negative session self-test {name!r} was accepted")


def _set_active_support_reasons(record: dict[str, Any], reasons: tuple[str, ...]) -> None:
    support = record["sessions"][0]["active"]["support"]
    support["reference_supported"] = not reasons
    support["unsupported_reasons"] = list(reasons)


def _test_record_validation(
    descriptors: Sequence[CaseDescriptor],
    summary: dict[str, Any],
) -> None:
    descriptor = descriptors[0]
    valid = summary["case_results"][0]["repeats"][0]
    validate_session_probe_record(valid, descriptor, 0, valid["run_nonce"])
    mutations: tuple[tuple[str, Callable[[Any], None]], ...] = (
        ("missing-key", lambda item: item.pop("complete")),
        ("extra-key", lambda item: item.__setitem__("extra", 1)),
        ("wrong-schema", lambda item: item.__setitem__("schema", "bad")),
        ("false-complete", lambda item: item.__setitem__("complete", False)),
        ("wrong-case", lambda item: item.__setitem__("case", "diamond")),
        ("wrong-repeat", lambda item: item.__setitem__("repeat_index", 2)),
        ("wrong-nonce", lambda item: item.__setitem__("run_nonce", "bad")),
        (
            "full-observation",
            lambda item: item.__setitem__("full_observation", {}),
        ),
        ("missing-session", lambda item: item["sessions"].pop()),
        (
            "extra-session",
            lambda item: item["sessions"].append(copy.deepcopy(item["sessions"][0])),
        ),
        (
            "bool-token",
            lambda item: item["sessions"][0].__setitem__("session_token", True),
        ),
        (
            "zero-token",
            lambda item: item["sessions"][0].__setitem__("session_token", 0),
        ),
        (
            "identity-token",
            lambda item: item["sessions"][0]["active"]["identity"].__setitem__("session_token", 2),
        ),
        (
            "bad-generation",
            lambda item: item["sessions"][0]["active"]["identity"].__setitem__(
                "graph_generation", 1
            ),
        ),
        (
            "bad-hash",
            lambda item: item["sessions"][0]["active"]["identity"].__setitem__(
                "topology_fingerprint", "bad"
            ),
        ),
        (
            "bad-transition",
            lambda item: item["sessions"][0]["released"]["counts"].__setitem__("nodes", 99),
        ),
        (
            "same-session-token",
            lambda item: item["sessions"][1].__setitem__(
                "session_token", item["sessions"][0]["session_token"]
            ),
        ),
        (
            "different-content",
            lambda item: item["sessions"][1]["active"]["identity"].__setitem__(
                "library_fingerprint", "f" * 64
            ),
        ),
        (
            "observation-oracle-count",
            lambda item: item["sessions"][0]["oracle"].__setitem__("node_count", 99),
        ),
        (
            "malformed-projection-bytes",
            lambda item: item["sessions"][0]["oracle"].__setitem__(
                "projection_wire_hex", "not-hex"
            ),
        ),
        (
            "bool-count",
            lambda item: item["sessions"][0]["active"]["counts"].__setitem__("nodes", True),
        ),
        (
            "nan-timing",
            lambda item: item["sessions"][0]["active"]["timing_ms"].__setitem__(
                "export", float("nan")
            ),
        ),
        (
            "bad-buffer",
            lambda item: item["sessions"][0]["active"]["buffers"]["node_keys"].__setitem__(
                "logical_size", 0
            ),
        ),
        (
            "unknown-support-reason",
            lambda item: _set_active_support_reasons(item, ("unknown_reason",)),
        ),
        (
            "duplicate-support-reason",
            lambda item: _set_active_support_reasons(item, ("parasitic_net", "parasitic_net")),
        ),
        (
            "out-of-order-support-reason",
            lambda item: _set_active_support_reasons(
                item, ("sequential_instance", "parasitic_net")
            ),
        ),
    )
    for name, mutation in mutations:
        _expect_error(
            name,
            lambda mutation=mutation: validate_session_probe_record(
                _mutated(valid, mutation),
                descriptor,
                0,
                valid["run_nonce"],
            ),
        )


def _test_summary_validation(
    descriptors: Sequence[CaseDescriptor],
    summary: dict[str, Any],
) -> None:
    validate_controller_result(summary, descriptors)
    mutations: tuple[tuple[str, Callable[[Any], None]], ...] = (
        ("summary-missing", lambda item: item.pop("complete")),
        ("summary-extra", lambda item: item.__setitem__("extra", 1)),
        ("summary-schema", lambda item: item.__setitem__("schema", "bad")),
        ("summary-incomplete", lambda item: item.__setitem__("complete", False)),
        ("bool-repeats", lambda item: item.__setitem__("requested_repeats", True)),
        ("small-repeats", lambda item: item.__setitem__("requested_repeats", 2)),
        ("missing-default", lambda item: item["default_compat_results"].pop()),
        (
            "default-order",
            lambda item: item["default_compat_results"].reverse(),
        ),
        (
            "default-projection-mismatch",
            lambda item: item["default_compat_results"][0].__setitem__(
                "projection_wire_hex", b"stable-wrong-projection".hex()
            ),
        ),
        ("missing-case", lambda item: item["case_results"].pop()),
        ("case-order", lambda item: item["case_results"].reverse()),
        (
            "missing-repeat",
            lambda item: item["case_results"][0]["repeats"].pop(),
        ),
        (
            "duplicate-nonce",
            lambda item: item["case_results"][1]["repeats"][0].__setitem__(
                "run_nonce",
                item["case_results"][0]["repeats"][0]["run_nonce"],
            ),
        ),
        (
            "canned-cross-case",
            lambda item: item["case_results"][1]["repeats"].__setitem__(
                0,
                copy.deepcopy(item["case_results"][0]["repeats"][0]),
            ),
        ),
        (
            "same-topology",
            lambda item: [
                record["sessions"][0]["active"]["identity"].__setitem__(
                    "topology_fingerprint",
                    item["case_results"][0]["repeats"][0]["sessions"][0]["active"]["identity"][
                        "topology_fingerprint"
                    ],
                )
                for record in item["case_results"][1]["repeats"]
            ],
        ),
        (
            "same-projection",
            lambda item: [
                record["sessions"][0]["oracle"].__setitem__(
                    "projection_wire_hex",
                    item["case_results"][0]["repeats"][0]["sessions"][0]["oracle"][
                        "projection_wire_hex"
                    ],
                )
                for record in item["case_results"][1]["repeats"]
            ],
        ),
        (
            "same-counts",
            lambda item: [
                _replace_counts(
                    record,
                    item["case_results"][0]["repeats"][0]["sessions"][0]["active"]["counts"],
                )
                for record in item["case_results"][1]["repeats"]
            ],
        ),
        (
            "stable-session-projection-drift",
            lambda item: _replace_session_wires(
                item["case_results"][0],
                b"stable-wrong-projection".hex(),
                item["default_compat_results"][0]["fifo_wire_hex"],
            ),
        ),
        (
            "stable-session-fifo-drift",
            lambda item: _replace_session_wires(
                item["case_results"][0],
                item["default_compat_results"][0]["projection_wire_hex"],
                b"stable-wrong-fifo".hex(),
            ),
        ),
    )
    for name, mutation in mutations:
        _expect_error(
            name,
            lambda mutation=mutation: validate_controller_result(
                _mutated(summary, mutation),
                descriptors,
            ),
        )


def _replace_counts(record: dict[str, Any], counts: dict[str, int]) -> None:
    for session in record["sessions"]:
        for observation_name in ("active", "released"):
            observation = session[observation_name]
            observation["counts"] = copy.deepcopy(counts)
            nodes, arcs, levels = counts["nodes"], counts["arcs"], counts["levels"]
            logical = {
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
            for field, value in logical.items():
                observation["buffers"][field] = _extent(value)
        session["oracle"]["node_count"] = counts["nodes"]
        session["oracle"]["edge_count"] = counts["arcs"]
        session["oracle"]["level_count"] = counts["levels"]


def _replace_session_wires(
    case_result: dict[str, Any], projection_wire_hex: str, fifo_wire_hex: str
) -> None:
    for record in case_result["repeats"]:
        for session in record["sessions"]:
            session["oracle"]["projection_wire_hex"] = projection_wire_hex
            session["oracle"]["fifo_wire_hex"] = fifo_wire_hex


def _test_raw_loader(summary: dict[str, Any], directory: Path) -> None:
    encoded = json.dumps(summary, sort_keys=True, separators=(",", ":")).encode("utf-8")
    samples = {
        "duplicate": encoded.replace(
            b'{"case_results"',
            b'{"schema":"duplicate","case_results"',
            1,
        ),
        "second-json": encoded + b"\n{}",
        "trailing-garbage": encoded + b" trailing",
        "truncated": encoded[:-1],
        "nan": encoded.replace(b'"requested_repeats":3', b'"requested_repeats":NaN'),
        "infinity": encoded.replace(b'"requested_repeats":3', b'"requested_repeats":Infinity'),
    }
    for name, payload in samples.items():
        path = directory / f"{name}.json"
        path.write_bytes(payload)
        _expect_error(name, lambda path=path: load_json(path))


def _test_default_compat(
    descriptors: Sequence[CaseDescriptor],
    compat_path: Path,
) -> None:
    compat = load_default_compat(compat_path)
    malformed = copy.deepcopy(compat)
    malformed["expected_by_case"]["chain"]["functional_projection"] = []
    with tempfile.TemporaryDirectory(prefix="csr-session-compat-selftest-") as temporary:
        malformed_path = Path(temporary) / "malformed-default-compat.json"
        malformed_path.write_text(json.dumps(malformed), encoding="utf-8")
        _expect_error(
            "default-loader-functional-projection",
            lambda: load_default_compat(malformed_path),
        )

    sample_records: dict[str, tuple[dict[str, Any], ...]] = {}
    synthetic_compat = copy.deepcopy(compat)
    for descriptor in descriptors:
        projection, oracle, _node_count, _arc_count = fixture_bytes(descriptor.name)
        record = {
            "case": descriptor.name,
            "top": descriptor.top,
            "expected_clock": descriptor.expected_clock,
            "direct_projection": projection.hex(),
            "native_graph_oracle": oracle.hex(),
            "complete": True,
        }
        semantic = _actual_default_semantics(record, f"synthetic {descriptor.name}")
        records = tuple(copy.deepcopy(record) for _index in range(3))
        synthetic_compat["expected_by_case"][descriptor.name] = semantic
        sample_records[descriptor.name] = records
    validate_default_records(sample_records, descriptors, synthetic_compat)
    poisoned = copy.deepcopy(synthetic_compat)
    poisoned["expected_by_case"]["chain"]["functional_projection"]["nodes"][0][
        "direction"
    ] = "output"
    _expect_error(
        "default-semantic-drift",
        lambda: validate_default_records(sample_records, descriptors, poisoned),
    )


def _test_subprocess_protocol(
    descriptors: Sequence[CaseDescriptor],
    directory: Path,
) -> None:
    from csr_session_controller import _prepare_slots, run_session_slot

    root = directory / "subprocess"
    root.mkdir()
    probe = root / "fake_probe.py"
    probe.write_text(
        "\n".join(
            (
                "import os",
                "import sys",
                "import threading",
                "from pathlib import Path",
                "args = dict(zip(sys.argv[1::2], sys.argv[2::2], strict=True))",
                "if args.get('--probe-mode') != 'session':",
                "    raise SystemExit(91)",
                "mode = os.environ['CSR_SESSION_SELFTEST_MODE']",
                "if mode == 'timeout':",
                "    threading.Event().wait(60.0)",
                "if mode == 'nonzero':",
                "    raise SystemExit(7)",
                "if mode == 'missing':",
                "    raise SystemExit(0)",
                "payload = Path(os.environ['CSR_SESSION_SELFTEST_TEMPLATE']).read_bytes()",
                "target = Path(args['--result-temp'])",
                "if mode == 'direct-final':",
                "    target = Path(args['--run-dir']) / 'csr-session-result.json'",
                "target.write_bytes(payload)",
                "if mode == 'stderr':",
                "    sys.stderr.buffer.write(b'injected stderr')",
            )
        )
        + "\n",
        encoding="utf-8",
    )
    command = (sys.executable, str(probe))
    descriptor = descriptors[0]

    def make_slot(name: str) -> Any:
        run_directory = root / name
        run_directory.mkdir()
        return _prepare_slots(run_directory, descriptors[:1], 1)[0]

    mode_name = "CSR_SESSION_SELFTEST_MODE"
    template_name = "CSR_SESSION_SELFTEST_TEMPLATE"
    previous = {
        mode_name: os.environ.get(mode_name),
        template_name: os.environ.get(template_name),
    }
    try:
        success_slot = make_slot("success")
        success_template = root / "success-template.json"
        success_template.write_text(
            json.dumps(
                _record(descriptor, success_slot.repeat_index, success_slot.run_nonce),
                sort_keys=True,
                separators=(",", ":"),
            ),
            encoding="utf-8",
        )
        os.environ[mode_name] = "success"
        os.environ[template_name] = str(success_template)
        success = run_session_slot(command, success_slot, 2.0)
        if (
            success["run_nonce"] != success_slot.run_nonce
            or success_slot.temporary_result.exists()
            or not success_slot.final_result.is_file()
        ):
            raise SessionContractError(
                "successful session subprocess did not use atomic temp/final publication"
            )

        for mode in ("stderr", "nonzero", "missing", "direct-final", "timeout"):
            slot = make_slot(mode)
            template = root / f"{mode}-template.json"
            template.write_text(
                json.dumps(
                    _record(descriptor, slot.repeat_index, slot.run_nonce),
                    sort_keys=True,
                    separators=(",", ":"),
                ),
                encoding="utf-8",
            )
            os.environ[mode_name] = mode
            os.environ[template_name] = str(template)
            timeout = 0.02 if mode == "timeout" else 2.0
            _expect_error(
                f"subprocess-{mode}",
                lambda slot=slot, timeout=timeout: run_session_slot(
                    command,
                    slot,
                    timeout,
                ),
            )

        empty_command = make_slot("empty-command")
        _expect_error(
            "empty-probe-command",
            lambda: run_session_slot((), empty_command, 2.0),
        )
        invalid_timeout = make_slot("invalid-timeout")
        _expect_error(
            "invalid-probe-timeout",
            lambda: run_session_slot(command, invalid_timeout, 0.0),
        )

        preexisting_temp = make_slot("preexisting-temp")
        preexisting_temp.temporary_result.write_text("{}", encoding="utf-8")
        _expect_error(
            "preexisting-probe-temp",
            lambda: run_session_slot(command, preexisting_temp, 2.0),
        )
        preexisting_final = make_slot("preexisting-final")
        preexisting_final.final_result.write_text("{}", encoding="utf-8")
        _expect_error(
            "preexisting-probe-final",
            lambda: run_session_slot(command, preexisting_final, 2.0),
        )
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


def run_self_test(manifest_path: Path, compat_path: Path) -> None:
    descriptors = load_case_descriptors(manifest_path)
    _test_default_compat(descriptors, compat_path)
    summary = _summary(descriptors)
    _test_record_validation(descriptors, summary)
    _test_summary_validation(descriptors, summary)
    with tempfile.TemporaryDirectory(prefix="csr-session-selftest-") as temporary:
        _test_raw_loader(summary, Path(temporary))
        _test_subprocess_protocol(descriptors, Path(temporary))


def main(argv: Sequence[str] | None = None) -> int:
    del argv
    raise SessionContractError(
        "invoke this self-test through csr_session_controller.py --self-test"
    )


if __name__ == "__main__":
    raise SystemExit(main())
