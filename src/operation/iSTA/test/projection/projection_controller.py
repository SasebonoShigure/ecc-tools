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
"""Run the native-graph projection probe in fresh, independently validated processes."""

import argparse
import math
import os
import secrets
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from projection_contract import (
    CONTROLLER_SCHEMA,
    BaselineError,
    CaseDescriptor,
    ProjectionContractError,
    ValidatedProbe,
    load_case_descriptors,
    load_json,
    publish_atomic,
    require_regular_owned_file,
    validate_controller_result,
    validate_probe_record,
    validate_repeat_count,
    write_json_exclusive,
)

_DEFAULT_MANIFEST = Path(__file__).resolve().parents[1] / "baseline" / "manifest.json"


@dataclass(frozen=True)
class RunSlot:
    descriptor: CaseDescriptor
    repeat_index: int
    run_nonce: str
    work_directory: Path
    temporary_result: Path
    final_result: Path


@dataclass(frozen=True)
class ControllerRun:
    directory: Path
    summary_path: Path
    summary: dict[str, object]


def _assert_absent(path: Path, where: str) -> None:
    if path.exists() or path.is_symlink():
        raise ProjectionContractError(f"{where} must not preexist: {path}")


def _create_unique_run_directory(root: Path) -> Path:
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    require_regular_root = root.lstat()
    if not root.is_dir() or root.is_symlink():
        raise ProjectionContractError(f"controller run root must be a real directory: {root}")
    if hasattr(os, "geteuid") and require_regular_root.st_uid != os.geteuid():
        raise ProjectionContractError("controller run root is not owned by the current user")
    for _attempt in range(32):
        run_directory = root / f"projection-{os.getpid()}-{secrets.token_hex(16)}"
        try:
            run_directory.mkdir(mode=0o700)
        except FileExistsError:
            continue
        return run_directory
    raise ProjectionContractError("could not allocate a unique controller run directory")


def _prepare_slots(
    run_directory: Path,
    descriptors: tuple[CaseDescriptor, CaseDescriptor],
    repeats: int,
) -> tuple[RunSlot, ...]:
    slots: list[RunSlot] = []
    for descriptor in descriptors:
        for repeat_index in range(repeats):
            nonce = secrets.token_hex(16)
            work = run_directory / f"{descriptor.name}-{repeat_index}-{nonce}"
            work.mkdir(mode=0o700)
            temporary = work / f".projection-result-{nonce}.tmp"
            final = work / "projection-result.json"
            slots.append(RunSlot(descriptor, repeat_index, nonce, work, temporary, final))
    if len({slot.run_nonce for slot in slots}) != len(slots):
        raise ProjectionContractError("controller generated a duplicate run nonce")
    for slot in slots:
        _assert_absent(slot.temporary_result, "probe temporary result")
        _assert_absent(slot.final_result, "probe final result")
    return tuple(slots)


def _probe_arguments(slot: RunSlot) -> tuple[str, ...]:
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
    )


def run_probe_slot(
    probe_command: Sequence[str], slot: RunSlot, timeout_seconds: float
) -> ValidatedProbe:
    if not probe_command or any(not isinstance(item, str) or not item for item in probe_command):
        raise ProjectionContractError("probe command must contain non-empty strings")
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise ProjectionContractError("probe timeout must be positive")
    _assert_absent(slot.temporary_result, "probe temporary result")
    _assert_absent(slot.final_result, "probe final result")
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
        raise ProjectionContractError(
            f"probe launch failed for {slot.descriptor.name}[{slot.repeat_index}]: {exc}"
        ) from exc
    _assert_absent(slot.final_result, "probe-written direct final result")
    if completed.returncode != 0:
        raise ProjectionContractError(
            f"probe {slot.descriptor.name}[{slot.repeat_index}] exited {completed.returncode}"
        )
    if completed.stderr:
        preview = completed.stderr[:400].decode("utf-8", errors="backslashreplace")
        raise ProjectionContractError(
            f"probe {slot.descriptor.name}[{slot.repeat_index}] wrote stderr: {preview!r}"
        )
    require_regular_owned_file(slot.temporary_result, "probe temporary result")
    record = load_json(slot.temporary_result)
    validated = validate_probe_record(
        record,
        slot.descriptor,
        slot.repeat_index,
        slot.run_nonce,
    )
    publish_atomic(slot.temporary_result, slot.final_result, "probe result")
    return validated


def _build_summary(
    descriptors: tuple[CaseDescriptor, CaseDescriptor],
    slots: tuple[RunSlot, ...],
    validated: tuple[ValidatedProbe, ...],
    repeats: int,
) -> dict[str, object]:
    if len(slots) != len(validated):
        raise ProjectionContractError("internal controller result count mismatch")
    paired = tuple(zip(slots, validated, strict=True))
    case_results: list[dict[str, object]] = []
    for descriptor in descriptors:
        records = [item.record for slot, item in paired if slot.descriptor.name == descriptor.name]
        case_results.append({"case": descriptor.name, "repeats": records})
    summary: dict[str, object] = {
        "schema": CONTROLLER_SCHEMA,
        "requested_repeats": repeats,
        "case_results": case_results,
        "complete": True,
    }
    validate_controller_result(summary, descriptors)
    return summary


def run_controller(
    *,
    probe_command: Sequence[str],
    manifest_path: Path,
    run_root: Path,
    repeats: int,
    timeout_seconds: float,
) -> ControllerRun:
    repeats = validate_repeat_count(repeats)
    descriptors = load_case_descriptors(manifest_path)
    run_directory = _create_unique_run_directory(run_root)
    slots = _prepare_slots(run_directory, descriptors, repeats)
    validated = tuple(run_probe_slot(probe_command, slot, timeout_seconds) for slot in slots)
    summary = _build_summary(descriptors, slots, validated, repeats)
    summary_nonce = secrets.token_hex(16)
    temporary = run_directory / f".controller-result-{summary_nonce}.tmp"
    final = run_directory / "controller-result.json"
    _assert_absent(temporary, "controller summary temporary")
    _assert_absent(final, "controller summary final")
    write_json_exclusive(temporary, summary)
    parsed = load_json(temporary)
    validate_controller_result(parsed, descriptors)
    publish_atomic(temporary, final, "controller summary")
    return ControllerRun(run_directory, final, summary)


def _resolve_probe_command(probe: Path) -> tuple[str, ...]:
    probe = probe.resolve()
    if not probe.is_file():
        raise ProjectionContractError(f"projection probe does not exist: {probe}")
    if probe.suffix == ".py":
        return sys.executable, str(probe)
    if not os.access(probe, os.X_OK):
        raise ProjectionContractError(f"projection probe is not executable: {probe}")
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
    parser.add_argument("--run-root", type=Path, help="binary-tree directory for unique runs")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout-seconds", type=_positive_float, default=300.0)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run controller/schema negative validation without the production probe",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        if arguments.self_test:
            from projection_controller_selftest import run_self_test

            run_self_test(arguments.manifest)
            return 0
        if arguments.probe is None or arguments.run_root is None:
            raise ProjectionContractError("--probe and --run-root are required outside --self-test")
        run = run_controller(
            probe_command=_resolve_probe_command(arguments.probe),
            manifest_path=arguments.manifest,
            run_root=arguments.run_root,
            repeats=arguments.repeats,
            timeout_seconds=arguments.timeout_seconds,
        )
        print(run.summary_path)
        return 0
    except (BaselineError, OSError) as exc:
        print(f"projection controller failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
