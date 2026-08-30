#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# ***************************************************************************************
"""Run legacy-default and session-mode probes in independently validated processes."""

import argparse
import math
import os
import secrets
import subprocess
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_TEST_ROOT = Path(__file__).resolve().parent.parent
_BASELINE_ROOT = _TEST_ROOT / "baseline"
_PROJECTION_ROOT = _TEST_ROOT / "projection"
for _import_root in (_BASELINE_ROOT, _PROJECTION_ROOT):
    if str(_import_root) not in sys.path:
        sys.path.insert(0, str(_import_root))

import projection_controller  # noqa: E402
from baseline_contract import (  # noqa: E402
    BaselineError,
    load_json,
)
from projection_contract import (  # noqa: E402
    CaseDescriptor,
    load_case_descriptors,
    publish_atomic,
    require_regular_owned_file,
    write_json_exclusive,
)
from csr_session_contract import (  # noqa: E402
    CONTROLLER_SCHEMA,
    SessionContractError,
    load_default_compat,
    validate_controller_result,
    validate_default_records,
    validate_session_probe_record,
)

_DEFAULT_MANIFEST = _BASELINE_ROOT / "manifest.json"
_DEFAULT_COMPAT = _PROJECTION_ROOT / "projection_default_compat_v1.json"


@dataclass(frozen=True)
class SessionRunSlot:
    descriptor: CaseDescriptor
    repeat_index: int
    run_nonce: str
    work_directory: Path
    temporary_result: Path
    final_result: Path


@dataclass(frozen=True)
class SessionControllerRun:
    directory: Path
    summary_path: Path
    summary: dict[str, object]


def _assert_absent(path: Path, where: str) -> None:
    if path.exists() or path.is_symlink():
        raise SessionContractError(f"{where} must not preexist: {path}")


def _create_unique_run_directory(root: Path) -> Path:
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    metadata = root.lstat()
    if not root.is_dir() or root.is_symlink():
        raise SessionContractError(f"session run root must be a real directory: {root}")
    if hasattr(os, "geteuid") and metadata.st_uid != os.geteuid():
        raise SessionContractError("session run root is not owned by the current user")
    for _attempt in range(32):
        directory = root / f"csr-session-{os.getpid()}-{secrets.token_hex(16)}"
        try:
            directory.mkdir(mode=0o700)
        except FileExistsError:
            continue
        return directory
    raise SessionContractError("could not allocate a unique session controller directory")


def _prepare_slots(
    run_directory: Path,
    descriptors: Sequence[CaseDescriptor],
    repeats: int,
) -> tuple[SessionRunSlot, ...]:
    slots: list[SessionRunSlot] = []
    for descriptor in descriptors:
        for repeat_index in range(repeats):
            nonce = secrets.token_hex(16)
            work = run_directory / f"{descriptor.name}-{repeat_index}-{nonce}"
            work.mkdir(mode=0o700)
            temporary = work / f".csr-session-result-{nonce}.tmp"
            final = work / "csr-session-result.json"
            slots.append(
                SessionRunSlot(
                    descriptor,
                    repeat_index,
                    nonce,
                    work,
                    temporary,
                    final,
                )
            )
    if len({slot.run_nonce for slot in slots}) != len(slots):
        raise SessionContractError("session controller generated a duplicate nonce")
    for slot in slots:
        _assert_absent(slot.temporary_result, "session probe temporary result")
        _assert_absent(slot.final_result, "session probe final result")
    return tuple(slots)


def _probe_arguments(slot: SessionRunSlot) -> tuple[str, ...]:
    descriptor = slot.descriptor
    return (
        "--tech-lef",
        str(descriptor.tech_lef),
        "--cell-lef",
        str(descriptor.cell_lef),
        "--liberty",
        str(descriptor.liberty),
        "--verilog",
        str(descriptor.verilog_path),
        "--sdc",
        str(descriptor.sdc_path),
        "--top",
        descriptor.top,
        "--case-name",
        descriptor.name,
        "--expected-clock",
        descriptor.expected_clock,
        "--repeat-index",
        str(slot.repeat_index),
        "--input-case-sha256",
        descriptor.input_case_sha256,
        "--run-nonce",
        slot.run_nonce,
        "--run-dir",
        str(slot.work_directory),
        "--result-temp",
        str(slot.temporary_result),
        "--probe-mode",
        "session",
    )


def run_session_slot(
    probe_command: Sequence[str],
    slot: SessionRunSlot,
    timeout_seconds: float,
) -> dict[str, Any]:
    if not probe_command or any(not isinstance(item, str) or not item for item in probe_command):
        raise SessionContractError("probe command must contain non-empty strings")
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise SessionContractError("session probe timeout must be positive")
    _assert_absent(slot.temporary_result, "session probe temporary result")
    _assert_absent(slot.final_result, "session probe final result")
    command = (*probe_command, *_probe_arguments(slot))
    try:
        completed = subprocess.run(
            command,
            cwd=slot.work_directory,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=False,
            timeout=timeout_seconds,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise SessionContractError(
            f"session probe launch failed for "
            f"{slot.descriptor.name}[{slot.repeat_index}]: {exc}"
        ) from exc
    _assert_absent(slot.final_result, "session probe-written direct final result")
    if completed.returncode != 0:
        raise SessionContractError(
            f"session probe {slot.descriptor.name}[{slot.repeat_index}] "
            f"exited {completed.returncode}"
        )
    if completed.stderr:
        preview = completed.stderr[:400].decode("utf-8", errors="backslashreplace")
        raise SessionContractError(
            f"session probe {slot.descriptor.name}[{slot.repeat_index}] "
            f"wrote stderr: {preview!r}"
        )
    require_regular_owned_file(slot.temporary_result, "session probe temporary result")
    record = load_json(slot.temporary_result)
    validated = validate_session_probe_record(
        record,
        slot.descriptor,
        slot.repeat_index,
        slot.run_nonce,
    )
    publish_atomic(slot.temporary_result, slot.final_result, "session probe result")
    return validated


def _legacy_records(
    summary: Mapping[str, Any],
    descriptors: Sequence[CaseDescriptor],
) -> dict[str, tuple[Mapping[str, Any], ...]]:
    raw_results = summary.get("case_results")
    if not isinstance(raw_results, list) or len(raw_results) != len(descriptors):
        raise SessionContractError("legacy controller summary case count changed")
    result: dict[str, tuple[Mapping[str, Any], ...]] = {}
    for index, descriptor in enumerate(descriptors):
        item = raw_results[index]
        if not isinstance(item, dict) or item.get("case") != descriptor.name:
            raise SessionContractError("legacy controller summary order changed")
        repeats = item.get("repeats")
        if not isinstance(repeats, list) or len(repeats) != 3:
            raise SessionContractError("legacy controller did not return three repeats")
        if any(not isinstance(record, dict) for record in repeats):
            raise SessionContractError("legacy controller repeat must be an object")
        result[descriptor.name] = tuple(repeats)
    return result


def _build_summary(
    descriptors: Sequence[CaseDescriptor],
    slots: Sequence[SessionRunSlot],
    records: Sequence[dict[str, Any]],
    repeats: int,
    default_results: list[dict[str, Any]],
) -> dict[str, object]:
    if len(slots) != len(records):
        raise SessionContractError("internal session result count mismatch")
    paired = tuple(zip(slots, records, strict=True))
    case_results: list[dict[str, object]] = []
    for descriptor in descriptors:
        case_records = [
            record
            for slot, record in paired
            if slot.descriptor.name == descriptor.name
        ]
        case_results.append(
            {
                "case": descriptor.name,
                "input_case_sha256": descriptor.input_case_sha256,
                "repeats": case_records,
            }
        )
    summary: dict[str, object] = {
        "schema": CONTROLLER_SCHEMA,
        "requested_repeats": repeats,
        "default_compat_results": default_results,
        "case_results": case_results,
        "complete": True,
    }
    validate_controller_result(summary, descriptors)
    return summary


def run_controller(
    *,
    probe_command: Sequence[str],
    manifest_path: Path,
    default_compat_path: Path,
    run_root: Path,
    repeats: int,
    timeout_seconds: float,
) -> SessionControllerRun:
    repeats = projection_controller.validate_repeat_count(repeats)
    descriptors = load_case_descriptors(manifest_path)
    compat = load_default_compat(default_compat_path)

    legacy_run = projection_controller.run_controller(
        probe_command=probe_command,
        manifest_path=manifest_path,
        run_root=run_root.resolve().parent / "projection-runs",
        repeats=3,
        timeout_seconds=timeout_seconds,
    )
    legacy_records = _legacy_records(legacy_run.summary, descriptors)
    default_results = validate_default_records(legacy_records, descriptors, compat)

    run_directory = _create_unique_run_directory(run_root)
    slots = _prepare_slots(run_directory, descriptors, repeats)
    default_nonces = {
        str(record["run_nonce"])
        for records in legacy_records.values()
        for record in records
    }
    if default_nonces.intersection(slot.run_nonce for slot in slots):
        raise SessionContractError("default and session phases reused a nonce")
    records = tuple(run_session_slot(probe_command, slot, timeout_seconds) for slot in slots)
    summary = _build_summary(descriptors, slots, records, repeats, default_results)

    summary_nonce = secrets.token_hex(16)
    temporary = run_directory / f".csr-session-controller-{summary_nonce}.tmp"
    final = run_directory / "csr-session-controller-result.json"
    _assert_absent(temporary, "session controller summary temporary")
    _assert_absent(final, "session controller summary final")
    write_json_exclusive(temporary, summary)
    parsed = load_json(temporary)
    validate_controller_result(parsed, descriptors)
    publish_atomic(temporary, final, "session controller summary")
    return SessionControllerRun(run_directory, final, summary)


def _resolve_probe_command(probe: Path) -> tuple[str, ...]:
    probe = probe.resolve()
    if not probe.is_file():
        raise SessionContractError(f"projection probe does not exist: {probe}")
    if probe.suffix == ".py":
        return sys.executable, str(probe)
    if not os.access(probe, os.X_OK):
        raise SessionContractError(f"projection probe is not executable: {probe}")
    return (str(probe),)


def _positive_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("timeout must be numeric") from exc
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("timeout must be positive")
    return parsed


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, help="projection probe executable")
    parser.add_argument("--manifest", type=Path, default=_DEFAULT_MANIFEST)
    parser.add_argument("--default-compat", type=Path, default=_DEFAULT_COMPAT)
    parser.add_argument("--run-root", type=Path, help="binary-tree directory for unique runs")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout-seconds", type=_positive_float, default=75.0)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run strict raw-byte/controller negative validation",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        if arguments.self_test:
            from csr_session_controller_selftest import run_self_test

            run_self_test(arguments.manifest, arguments.default_compat)
            return 0
        if arguments.probe is None or arguments.run_root is None:
            raise SessionContractError("--probe and --run-root are required outside --self-test")
        run = run_controller(
            probe_command=_resolve_probe_command(arguments.probe),
            manifest_path=arguments.manifest,
            default_compat_path=arguments.default_compat,
            run_root=arguments.run_root,
            repeats=arguments.repeats,
            timeout_seconds=arguments.timeout_seconds,
        )
        print(run.summary_path)
        return 0
    except (BaselineError, OSError) as exc:
        print(f"CSR session controller failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
