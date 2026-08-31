#!/usr/bin/env python3
"""Verify that malformed source sets fail before loader or guarded work."""

import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Any


SCHEMA_VERSION = "buffer-preloader-rejection/1.0.0"
EXPECTED_ERROR_CODE = "artifact_materialization_rejected"
ROLES = ("tech_lef", "cell_lef", "liberty", "verilog", "sdc")
OPTION_BY_ROLE = {
    "tech_lef": "--tech-lef",
    "cell_lef": "--cell-lef",
    "liberty": "--liberty",
    "verilog": "--verilog",
    "sdc": "--sdc",
}


class PreloaderRejectionError(RuntimeError):
    pass


@dataclass(frozen=True)
class SourceSnapshot:
    role: str
    lexical_path: pathlib.Path
    canonical_path: pathlib.Path
    lexical_is_symlink: bool
    lexical_link_text: str | None
    size: int
    exact_bytes: bytes


@dataclass(frozen=True)
class RejectionCase:
    name: str
    inputs: dict[str, pathlib.Path]
    must_remain_missing: pathlib.Path | None = None
    alias_path: pathlib.Path | None = None
    alias_link_text: str | None = None


def strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise PreloaderRejectionError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Exercise fail-closed production artifact preloading")
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    parser.add_argument("--tech-lef", type=pathlib.Path, required=True)
    parser.add_argument("--cell-lef", type=pathlib.Path, required=True)
    parser.add_argument("--liberty", type=pathlib.Path, required=True)
    parser.add_argument("--verilog", type=pathlib.Path, required=True)
    parser.add_argument("--sdc", type=pathlib.Path, required=True)
    parser.add_argument("--top", required=True)
    parser.add_argument("--run-root", type=pathlib.Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=300)
    return parser.parse_args()


def absolute_lexical(path: pathlib.Path) -> pathlib.Path:
    return pathlib.Path(os.path.abspath(os.fspath(path)))


def capture_source(role: str, path: pathlib.Path) -> SourceSnapshot:
    lexical = absolute_lexical(path)
    if not lexical.exists():
        raise PreloaderRejectionError(
            f"original {role} source does not exist: {lexical}")
    canonical = lexical.resolve(strict=True)
    if not canonical.is_file():
        raise PreloaderRejectionError(
            f"original {role} source is not a regular file: {canonical}")
    is_symlink = lexical.is_symlink()
    link_text = os.readlink(lexical) if is_symlink else None
    exact_bytes = canonical.read_bytes()
    size = canonical.stat().st_size
    if len(exact_bytes) != size:
        raise PreloaderRejectionError(
            f"original {role} source changed while it was captured")
    return SourceSnapshot(
        role=role,
        lexical_path=lexical,
        canonical_path=canonical,
        lexical_is_symlink=is_symlink,
        lexical_link_text=link_text,
        size=size,
        exact_bytes=exact_bytes,
    )


def capture_sources(args: argparse.Namespace) -> tuple[SourceSnapshot, ...]:
    paths = {
        "tech_lef": args.tech_lef,
        "cell_lef": args.cell_lef,
        "liberty": args.liberty,
        "verilog": args.verilog,
        "sdc": args.sdc,
    }
    snapshots = tuple(capture_source(role, paths[role]) for role in ROLES)
    canonical_paths = [snapshot.canonical_path for snapshot in snapshots]
    if len(set(canonical_paths)) != len(canonical_paths):
        raise PreloaderRejectionError(
            "the five original sources must have distinct canonical paths")
    return snapshots


def assert_sources_unchanged(snapshots: tuple[SourceSnapshot, ...]) -> None:
    for snapshot in snapshots:
        lexical = snapshot.lexical_path
        if not lexical.exists():
            raise PreloaderRejectionError(
                f"original {snapshot.role} source disappeared: {lexical}")
        if lexical.is_symlink() is not snapshot.lexical_is_symlink:
            raise PreloaderRejectionError(
                f"original {snapshot.role} source path type changed: {lexical}")
        if snapshot.lexical_is_symlink:
            link_text = os.readlink(lexical)
            if link_text != snapshot.lexical_link_text:
                raise PreloaderRejectionError(
                    f"original {snapshot.role} symlink target changed")
        canonical = lexical.resolve(strict=True)
        if canonical != snapshot.canonical_path:
            raise PreloaderRejectionError(
                f"original {snapshot.role} canonical path changed: "
                f"{canonical} != {snapshot.canonical_path}")
        exact_bytes = canonical.read_bytes()
        size = canonical.stat().st_size
        if size != snapshot.size:
            raise PreloaderRejectionError(
                f"original {snapshot.role} byte size changed: "
                f"{size} != {snapshot.size}")
        if exact_bytes != snapshot.exact_bytes:
            raise PreloaderRejectionError(
                f"original {snapshot.role} exact bytes changed")


def validate_receipt(value: object, case_name: str) -> None:
    if not isinstance(value, dict):
        raise PreloaderRejectionError(
            f"{case_name}: receipt must be one JSON object")
    expected_keys = {
        "schema_version",
        "error_code",
        "work_entered",
        "loader_invocation_count",
        "guarded_work_invocation_count",
        "one_shot_state",
    }
    actual_keys = set(value)
    if actual_keys != expected_keys:
        raise PreloaderRejectionError(
            f"{case_name}: receipt keys differ; "
            f"missing={sorted(expected_keys - actual_keys)}, "
            f"unknown={sorted(actual_keys - expected_keys)}")
    if value["schema_version"] != SCHEMA_VERSION:
        raise PreloaderRejectionError(
            f"{case_name}: schema_version={value['schema_version']!r}")
    if value["error_code"] != EXPECTED_ERROR_CODE:
        raise PreloaderRejectionError(
            f"{case_name}: error_code={value['error_code']!r}")
    if value["work_entered"] is not False:
        raise PreloaderRejectionError(
            f"{case_name}: guarded work was entered")
    for field in ("loader_invocation_count", "guarded_work_invocation_count"):
        if type(value[field]) is not int or value[field] != 0:
            raise PreloaderRejectionError(
                f"{case_name}: {field}={value[field]!r}, expected integer 0")
    if value["one_shot_state"] != "finished":
        raise PreloaderRejectionError(
            f"{case_name}: one_shot_state={value['one_shot_state']!r}")


def make_cases(
    snapshots: tuple[SourceSnapshot, ...], suite_dir: pathlib.Path
) -> tuple[RejectionCase, ...]:
    base = {snapshot.role: snapshot.canonical_path for snapshot in snapshots}
    cases: list[RejectionCase] = []
    suffixes = {
        "tech_lef": ".lef",
        "cell_lef": ".lef",
        "liberty": ".lib",
        "verilog": ".v",
        "sdc": ".sdc",
    }
    for role in ROLES:
        missing = suite_dir / f"missing-{role}{suffixes[role]}"
        if missing.exists() or missing.is_symlink():
            raise PreloaderRejectionError(
                f"missing-path fixture already exists: {missing}")
        inputs = dict(base)
        inputs[role] = missing
        cases.append(RejectionCase(
            name=f"missing-{role}",
            inputs=inputs,
            must_remain_missing=missing,
        ))

    alias_path = suite_dir / "cell-lef-alias-to-tech.lef"
    try:
        alias_path.symlink_to(base["tech_lef"])
    except OSError as error:
        raise PreloaderRejectionError(
            f"required symlink alias creation failed: {error}") from error
    if not alias_path.is_symlink():
        raise PreloaderRejectionError(
            f"required symlink alias was not created: {alias_path}")
    alias_link_text = os.readlink(alias_path)
    if alias_path.resolve(strict=True) != base["tech_lef"]:
        raise PreloaderRejectionError(
            "symlink alias does not resolve to the tech LEF source")
    symlink_inputs = dict(base)
    symlink_inputs["cell_lef"] = alias_path
    cases.append(RejectionCase(
        name="symlink-tech-cell-canonical-alias",
        inputs=symlink_inputs,
        alias_path=alias_path,
        alias_link_text=alias_link_text,
    ))
    return tuple(cases)


def run_case(
    probe: pathlib.Path,
    top: str,
    case: RejectionCase,
    suite_dir: pathlib.Path,
    timeout_seconds: int,
) -> None:
    case_dir = suite_dir / case.name
    case_dir.mkdir()
    output = case_dir / "receipt.json"
    command = [str(probe), "expect-preloader-reject"]
    for role in ROLES:
        command.extend((OPTION_BY_ROLE[role], str(case.inputs[role])))
    command.extend(("--top", top, "--output", str(output)))
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout_seconds,
    )
    if completed.returncode != 0:
        raise PreloaderRejectionError(
            f"{case.name}: probe failed ({completed.returncode})\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}")
    try:
        value = json.loads(
            output.read_text(encoding="utf-8"),
            object_pairs_hook=strict_object,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PreloaderRejectionError(
            f"{case.name}: cannot read strict receipt {output}: {error}") from error
    validate_receipt(value, case.name)
    if case.must_remain_missing is not None:
        missing = case.must_remain_missing
        if missing.exists() or missing.is_symlink():
            raise PreloaderRejectionError(
                f"{case.name}: rejected missing source path was created")
    if case.alias_path is not None:
        if not case.alias_path.is_symlink():
            raise PreloaderRejectionError(
                f"{case.name}: symlink alias was replaced")
        if os.readlink(case.alias_path) != case.alias_link_text:
            raise PreloaderRejectionError(
                f"{case.name}: symlink alias target changed")


def main() -> int:
    args = parse_args()
    snapshots: tuple[SourceSnapshot, ...] | None = None
    try:
        if args.timeout_seconds <= 0:
            raise PreloaderRejectionError(
                "--timeout-seconds must be positive")
        if not args.top:
            raise PreloaderRejectionError("--top must not be empty")
        probe = args.probe.resolve(strict=True)
        if not probe.is_file() or not os.access(probe, os.X_OK):
            raise PreloaderRejectionError(
                f"probe is not an executable file: {probe}")
        snapshots = capture_sources(args)
        try:
            run_root = absolute_lexical(args.run_root)
            run_root.mkdir(parents=True, exist_ok=True)
            suite_dir = pathlib.Path(tempfile.mkdtemp(
                prefix="preloader-rejection-", dir=run_root))
            cases = make_cases(snapshots, suite_dir)
            for case in cases:
                run_case(
                    probe,
                    args.top,
                    case,
                    suite_dir,
                    args.timeout_seconds,
                )
                assert_sources_unchanged(snapshots)
        finally:
            assert_sources_unchanged(snapshots)
        print(
            f"preloader rejection boundary passed: "
            f"{len(cases)} fresh processes in {suite_dir}")
        return 0
    except (
        PreloaderRejectionError,
        OSError,
        UnicodeError,
        subprocess.SubprocessError,
    ) as error:
        print(f"preloader rejection boundary failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
