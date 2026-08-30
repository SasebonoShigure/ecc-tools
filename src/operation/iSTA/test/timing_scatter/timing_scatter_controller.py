#!/usr/bin/env python3
"""Run and strictly validate the Step 6 native timing-scatter rerun probe."""

import argparse
import json
import os
import secrets
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn

_PROBE_SCHEMA = "ista-csr-timing-scatter-rerun-probe/1.0.0"
_CONTROLLER_SCHEMA = "ista-csr-timing-scatter-rerun-controller/1.0.0"
_TIMEOUT_SECONDS = 60.0
_MAX_STDOUT_BYTES = 1 << 20


class RerunProtocolError(RuntimeError):
    """The probe process or its JSON record violated the rerun protocol."""


@dataclass(frozen=True)
class ProbeCase:
    name: str
    fallback_mode: str


_CASES = (
    ProbeCase("chain", "equal"),
    ProbeCase("chain", "different"),
    ProbeCase("diamond", "none"),
)

_EXPECTED_KEYS = frozenset(
    {
        "schema",
        "case",
        "fallback_mode",
        "run_nonce",
        "baseline_result_materialized",
        "poison_changed",
        "scatter_immediate_exact_restore",
        "writes_applied",
        "delay_calculator_init_calls",
        "timing_propagator_calls",
        "graph_builder_calls",
        "forward_rerun_families_exact",
        "arc_rerun_families_exact",
        "arc_storage_preserved",
        "source_bytes_preserved",
        "non_rerun_required_restored_by_scatter",
        "non_rerun_paths_restored_by_scatter",
        "non_rerun_summary_preserved_by_scatter",
        "fallback_target_observed",
        "installed_fallback_equal_to_dynamic",
        "fallback_behavior_verified",
        "complete",
    }
)

_TRUE_EVIDENCE = (
    "baseline_result_materialized",
    "poison_changed",
    "scatter_immediate_exact_restore",
    "forward_rerun_families_exact",
    "arc_rerun_families_exact",
    "arc_storage_preserved",
    "source_bytes_preserved",
    "non_rerun_required_restored_by_scatter",
    "non_rerun_paths_restored_by_scatter",
    "non_rerun_summary_preserved_by_scatter",
    "fallback_behavior_verified",
    "complete",
)


def _fail(message: str) -> NoReturn:
    raise RerunProtocolError(message)


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            _fail(f"probe JSON contains duplicate key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> NoReturn:
    _fail(f"probe JSON contains non-finite constant: {value}")


def _parse_record(payload: bytes) -> dict[str, object]:
    if not payload or len(payload) > _MAX_STDOUT_BYTES:
        _fail("probe stdout must be one nonempty bounded JSON record")
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise RerunProtocolError("probe stdout is not strict UTF-8") from exc
    try:
        parsed = json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_constant=_reject_constant,
        )
    except (json.JSONDecodeError, RerunProtocolError) as exc:
        if isinstance(exc, RerunProtocolError):
            raise
        raise RerunProtocolError("probe stdout is malformed or contains trailing data") from exc
    if type(parsed) is not dict:
        _fail("probe JSON root must be an object")
    return parsed


def _require_exact_int(record: dict[str, object], key: str, expected: int) -> None:
    value = record.get(key)
    if type(value) is not int or value != expected:
        _fail(f"probe field {key} must be exact integer {expected}")


def _validate_record(
    record: dict[str, object],
    case: ProbeCase,
    nonce: str,
) -> dict[str, object]:
    if frozenset(record) != _EXPECTED_KEYS:
        missing = sorted(_EXPECTED_KEYS - frozenset(record))
        extra = sorted(frozenset(record) - _EXPECTED_KEYS)
        _fail(f"probe JSON keys differ; missing={missing}, extra={extra}")
    if record["schema"] != _PROBE_SCHEMA:
        _fail("probe schema differs")
    if record["case"] != case.name or record["fallback_mode"] != case.fallback_mode:
        _fail("probe case or fallback mode differs from the requested slot")
    if record["run_nonce"] != nonce:
        _fail("probe did not echo the unpredictable run nonce")
    for key in _TRUE_EVIDENCE:
        if record[key] is not True:
            _fail(f"probe evidence field {key} is not exactly true")
    writes = record["writes_applied"]
    if type(writes) is not int or writes <= 0:
        _fail("probe writes_applied must be one positive non-bool integer")
    _require_exact_int(record, "delay_calculator_init_calls", 1)
    _require_exact_int(record, "timing_propagator_calls", 1)
    _require_exact_int(record, "graph_builder_calls", 0)

    if case.name == "chain":
        if record["fallback_target_observed"] is not True:
            _fail("chain probe did not observe a fallback target")
        expected_equal = case.fallback_mode == "equal"
        if record["installed_fallback_equal_to_dynamic"] is not expected_equal:
            _fail("chain probe fallback equality evidence differs")
    else:
        if record["fallback_target_observed"] is not False:
            _fail("diamond probe unexpectedly reported a fallback target")
        if record["installed_fallback_equal_to_dynamic"] is not None:
            _fail("diamond probe fallback equality must be null")
    return record


def _resolve_probe(path: Path) -> Path:
    try:
        resolved = path.resolve(strict=True)
        metadata = resolved.stat()
    except OSError as exc:
        raise RerunProtocolError(f"probe cannot be resolved: {path}") from exc
    if not stat.S_ISREG(metadata.st_mode) or not os.access(resolved, os.X_OK):
        _fail(f"probe must be an executable regular file: {resolved}")
    return resolved


def _run_probe(probe: Path, case: ProbeCase) -> dict[str, object]:
    nonce = secrets.token_hex(16)
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    command = (
        str(probe),
        "--case",
        case.name,
        "--fallback-mode",
        case.fallback_mode,
        "--run-nonce",
        nonce,
    )
    try:
        completed = subprocess.run(
            command,
            env=environment,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=_TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RerunProtocolError(
            f"probe launch failed for {case.name}/{case.fallback_mode}: {exc}"
        ) from exc
    if completed.returncode != 0:
        stderr = completed.stderr[:400].decode("utf-8", errors="backslashreplace")
        _fail(
            f"probe {case.name}/{case.fallback_mode} exited "
            f"{completed.returncode}: {stderr!r}"
        )
    if completed.stderr:
        preview = completed.stderr[:400].decode("utf-8", errors="backslashreplace")
        _fail(f"successful probe wrote stderr: {preview!r}")
    return _validate_record(_parse_record(completed.stdout), case, nonce)


def run_controller(probe: Path) -> dict[str, object]:
    executable = _resolve_probe(probe)
    records = [_run_probe(executable, case) for case in _CASES]
    if records[0]["writes_applied"] != records[1]["writes_applied"]:
        _fail("chain equal/different slots reported different write counts")
    if records[0]["writes_applied"] <= records[2]["writes_applied"]:
        _fail("chain fixture write count must exceed the diamond fixture count")
    if len({record["run_nonce"] for record in records}) != len(records):
        _fail("controller generated or accepted a duplicate run nonce")
    return {
        "schema": _CONTROLLER_SCHEMA,
        "case_results": records,
        "complete": True,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    try:
        arguments = _parser().parse_args(argv)
        summary = run_controller(arguments.probe)
        print(json.dumps(summary, sort_keys=True, separators=(",", ":")))
        return 0
    except (RerunProtocolError, OSError, ValueError) as exc:
        print(f"timing_scatter_controller: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
