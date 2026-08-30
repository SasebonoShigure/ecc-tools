#!/usr/bin/env python3
"""Run complete reference-input poison probes in fresh validated processes."""

import argparse
import math
import os
import secrets
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import reference_input_contract as contract

_DEFAULT_MANIFEST = Path(__file__).resolve().parents[1] / "baseline" / "manifest.json"


@dataclass(frozen=True)
class RunSlot:
    descriptor: contract.CaseDescriptor
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
        raise contract.ReferenceInputContractError(f"{where} must not preexist: {path}")


def _create_unique_run_directory(root: Path) -> Path:
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    metadata = root.lstat()
    if not root.is_dir() or root.is_symlink():
        raise contract.ReferenceInputContractError(
            f"controller run root must be a real directory: {root}"
        )
    if hasattr(os, "geteuid") and metadata.st_uid != os.geteuid():
        raise contract.ReferenceInputContractError(
            "controller run root is not owned by the current user"
        )
    for _attempt in range(32):
        candidate = root / f"reference-input-{os.getpid()}-{secrets.token_hex(16)}"
        try:
            candidate.mkdir(mode=0o700)
        except FileExistsError:
            continue
        return candidate
    raise contract.ReferenceInputContractError(
        "could not allocate a unique reference-input run directory"
    )


def _prepare_slots(
    run_directory: Path,
    descriptors: tuple[contract.CaseDescriptor, contract.CaseDescriptor],
    repeats: int,
) -> tuple[RunSlot, ...]:
    slots: list[RunSlot] = []
    for descriptor in descriptors:
        for repeat_index in range(repeats):
            nonce = secrets.token_hex(16)
            work = run_directory / f"{descriptor.name}-{repeat_index}-{nonce}"
            work.mkdir(mode=0o700)
            temporary = work / f".reference-input-result-{nonce}.tmp"
            final = work / "reference-input-result.json"
            slots.append(RunSlot(descriptor, repeat_index, nonce, work, temporary, final))
    if len({slot.run_nonce for slot in slots}) != len(slots):
        raise contract.ReferenceInputContractError("controller generated a duplicate run nonce")
    for slot in slots:
        _assert_absent(slot.temporary_result, "probe temporary result")
        _assert_absent(slot.final_result, "probe final result")
    return tuple(slots)


def _probe_arguments(slot: RunSlot) -> tuple[str, ...]:
    descriptor = slot.descriptor
    context = contract.CANONICAL_CONTEXT
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
        "--liberty-corner",
        context["liberty_corner"],
        "--temperature",
        context["temperature"],
        "--rc-corner",
        context["rc_corner"],
        "--repeat-index",
        str(slot.repeat_index),
        "--input-case-sha256",
        descriptor.input_case_sha256,
        "--session-token",
        str(contract.SESSION_TOKEN),
        "--run-nonce",
        slot.run_nonce,
        "--run-dir",
        str(slot.work_directory),
        "--result-temp",
        str(slot.temporary_result),
    )


def run_probe_slot(
    probe_command: Sequence[str],
    slot: RunSlot,
    timeout_seconds: float,
) -> contract.ValidatedProbe:
    if not probe_command or any(not isinstance(item, str) or not item for item in probe_command):
        raise contract.ReferenceInputContractError("probe command must contain nonempty strings")
    if not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
        raise contract.ReferenceInputContractError("probe timeout must be positive")
    _assert_absent(slot.temporary_result, "probe temporary result")
    _assert_absent(slot.final_result, "probe final result")
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    try:
        completed = subprocess.run(
            (*probe_command, *_probe_arguments(slot)),
            cwd=slot.work_directory,
            env=environment,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=False,
            timeout=timeout_seconds,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise contract.ReferenceInputContractError(
            f"probe launch failed for {slot.descriptor.name}[{slot.repeat_index}]: {exc}"
        ) from exc
    _assert_absent(slot.final_result, "probe-written direct final result")
    if completed.returncode != 0:
        raise contract.ReferenceInputContractError(
            f"probe {slot.descriptor.name}[{slot.repeat_index}] exited {completed.returncode}"
        )
    if completed.stdout or completed.stderr:
        raise contract.ReferenceInputContractError(
            f"probe {slot.descriptor.name}[{slot.repeat_index}] wrote stdout/stderr"
        )
    contract.require_regular_owned_file(slot.temporary_result, "probe temporary result")
    record = contract.load_json(slot.temporary_result)
    validated = contract.validate_probe_record(
        record,
        slot.descriptor,
        slot.repeat_index,
        slot.run_nonce,
        contract.CANONICAL_CONTEXT,
    )
    contract.publish_atomic(
        slot.temporary_result,
        slot.final_result,
        f"probe {slot.descriptor.name}[{slot.repeat_index}]",
    )
    published = contract.load_json(slot.final_result)
    if published != validated.record:
        raise contract.ReferenceInputContractError(
            "published probe record differs after atomic rename"
        )
    return validated


def _build_summary(
    descriptors: tuple[contract.CaseDescriptor, contract.CaseDescriptor],
    repeats: int,
    slots: tuple[RunSlot, ...],
    validated: tuple[contract.ValidatedProbe, ...],
) -> dict[str, object]:
    by_slot = {slot: result for slot, result in zip(slots, validated, strict=True)}
    case_results: list[dict[str, object]] = []
    for descriptor in descriptors:
        rows: list[dict[str, object]] = []
        for repeat_index in range(repeats):
            slot = next(
                item
                for item in slots
                if item.descriptor == descriptor and item.repeat_index == repeat_index
            )
            result = by_slot[slot]
            rows.append(
                {
                    "repeat_index": repeat_index,
                    "input_case_sha256": descriptor.input_case_sha256,
                    "session_token": contract.SESSION_TOKEN,
                    "candidate_wire_sha256": result.candidate_wire_sha256,
                    "raw_projection_sha256": result.raw_projection_sha256,
                    "touched_families": list(result.touched_families),
                }
            )
        case_results.append({"case": descriptor.name, "repeats": rows})
    return {
        "schema": contract.CONTROLLER_SCHEMA,
        "requested_repeats": repeats,
        "case_results": case_results,
        "complete": True,
    }


def run_controller(
    manifest_path: Path,
    probe_command: Sequence[str],
    run_root: Path,
    repeats: int,
    timeout_seconds: float,
) -> ControllerRun:
    if type(repeats) is not int or repeats < 3:
        raise contract.ReferenceInputContractError(
            "controller repeats must be a non-bool integer >= 3"
        )
    descriptors = contract.load_case_descriptors(manifest_path.resolve())
    run_directory = _create_unique_run_directory(run_root)
    slots = _prepare_slots(run_directory, descriptors, repeats)
    validated = tuple(run_probe_slot(probe_command, slot, timeout_seconds) for slot in slots)
    summary = _build_summary(descriptors, repeats, slots, validated)
    contract.validate_controller_result(summary, descriptors)
    temporary = run_directory / ".reference-input-summary.tmp"
    final = run_directory / "reference-input-summary.json"
    _assert_absent(temporary, "controller summary temporary")
    _assert_absent(final, "controller summary final")
    contract.write_json_exclusive(temporary, summary)
    parsed = contract.load_json(temporary)
    contract.validate_controller_result(parsed, descriptors)
    contract.publish_atomic(temporary, final, "controller summary")
    if contract.load_json(final) != summary:
        raise contract.ReferenceInputContractError("published controller summary differs")
    return ControllerRun(run_directory, final, summary)


def _resolve_probe_command(probe: Path) -> tuple[str, ...]:
    resolved = probe.resolve()
    if not resolved.is_file():
        raise contract.ReferenceInputContractError(f"probe is not a regular file: {resolved}")
    if resolved.suffix == ".py":
        return (sys.executable, str(resolved))
    return (str(resolved),)


def _repeat_count(value: str) -> int:
    try:
        result = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("repeats must be an integer") from exc
    if result < 3:
        raise argparse.ArgumentTypeError("repeats must be >= 3")
    return result


def _positive_float(value: str) -> float:
    try:
        result = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("timeout must be numeric") from exc
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError("timeout must be finite and positive")
    return result


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=_DEFAULT_MANIFEST)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--repeats", type=_repeat_count, default=3)
    parser.add_argument("--timeout-seconds", type=_positive_float, default=120.0)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    result = run_controller(
        arguments.manifest,
        _resolve_probe_command(arguments.probe),
        arguments.run_root,
        arguments.repeats,
        arguments.timeout_seconds,
    )
    print(result.summary_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
