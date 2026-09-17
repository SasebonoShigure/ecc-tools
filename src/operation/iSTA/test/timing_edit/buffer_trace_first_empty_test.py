#!/usr/bin/env python3
"""Run and validate the production-loader first-empty generation boundary."""

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

from buffer_trace_controller import (
    ControllerError,
    require_exact_keys,
    validate_source_isolation,
)
from buffer_trace_functional_check import (
    FunctionalEvidenceError,
    validate_derived_freshness,
)


class FirstEmptyError(RuntimeError):
    pass


def strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise FirstEmptyError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
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


def validate(value: object, artifacts: list[pathlib.Path]) -> None:
    if not isinstance(value, dict):
        raise FirstEmptyError("receipt must be one JSON object")
    require_exact_keys(
        value,
        {
            "schema_version",
            "error_code",
            "error_what",
            "generation_zero_exact",
            "native_database_installed",
            "negative_gain_selection_exact",
            "source_isolation",
            "derived_timing_freshness",
            "loader_invocation_count",
            "guarded_work_invocation_count",
            "one_shot_state",
        },
        "first-empty receipt",
    )
    expected_scalars = {
        "schema_version": "buffer-first-empty-receipt/2.0.0",
        "error_code": "invalid_generated_artifact",
        "error_what": (
            "invalid_generated_artifact: empty_accepted_batch: "
            "no replayable batch was generated"
        ),
        "generation_zero_exact": True,
        "native_database_installed": False,
        "negative_gain_selection_exact": True,
        "loader_invocation_count": 1,
        "guarded_work_invocation_count": 1,
        "one_shot_state": "finished",
    }
    for field, expected in expected_scalars.items():
        if value[field] != expected:
            raise FirstEmptyError(
                f"{field}={value[field]!r}, expected {expected!r}")
    source = validate_source_isolation(
        value["source_isolation"], "first-empty source isolation")
    observed = source["artifacts_before"]
    canonical_paths = [
        pathlib.Path(item["canonical_path"]) for item in observed
    ]
    if any(not path.is_absolute() for path in canonical_paths):
        raise FirstEmptyError("materialized artifact path is not absolute")
    if len(set(canonical_paths)) != len(canonical_paths):
        raise FirstEmptyError("materialized artifact paths alias")
    input_paths = {path.resolve(strict=True) for path in artifacts}
    if input_paths.intersection(canonical_paths):
        raise FirstEmptyError(
            "synthetic artifacts reused an input fixture path"
        )
    parents = {path.parent for path in canonical_paths}
    if len(parents) != 1 or not next(iter(parents)).name.startswith(
        "ista-buffer-trace-"
    ):
        raise FirstEmptyError("materialized artifacts do not share one owned root")
    roles = ["tech_lef", "cell_lef", "liberty", "verilog", "sdc"]
    for index, (role, path) in enumerate(zip(roles, artifacts, strict=True)):
        canonical = path.resolve(strict=True)
        with canonical.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        item = observed[index]
        if item["role"] != role:
            raise FirstEmptyError(f"artifact {index} role differs")
        logical_name = f"0-{canonical.name}" if role in ("cell_lef", "liberty") else canonical.name
        if item["logical_name"] != logical_name or canonical_paths[index].name != canonical.name:
            raise FirstEmptyError(f"artifact {role} logical name differs")
        if item["size"] != canonical.stat().st_size:
            raise FirstEmptyError(f"artifact {role} byte size differs")
        if item["sha256"] != digest:
            raise FirstEmptyError(f"artifact {role} SHA-256 differs")
    validate_derived_freshness(
        value["derived_timing_freshness"],
        "synthetic",
        "first-empty derived timing freshness",
    )


def main() -> int:
    args = parse_args()
    try:
        probe = args.probe.resolve(strict=True)
        artifact_paths = [
            args.tech_lef,
            args.cell_lef,
            args.liberty,
            args.verilog,
            args.sdc,
        ]
        run_parent = args.run_root.resolve()
        run_parent.mkdir(parents=True, exist_ok=True)
        run_dir = pathlib.Path(
            tempfile.mkdtemp(prefix="run-", dir=run_parent))
        output = run_dir / "receipt.json"
        command = [
            str(probe),
            "expect-first-empty",
            "--tech-lef", str(artifact_paths[0].resolve(strict=True)),
            "--cell-lef", str(artifact_paths[1].resolve(strict=True)),
            "--liberty", str(artifact_paths[2].resolve(strict=True)),
            "--verilog", str(artifact_paths[3].resolve(strict=True)),
            "--sdc", str(artifact_paths[4].resolve(strict=True)),
            "--top", args.top,
            "--output", str(output),
        ]
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=args.timeout_seconds,
        )
        if completed.returncode != 0:
            raise FirstEmptyError(
                f"probe failed ({completed.returncode})\n"
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}")
        value = json.loads(
            output.read_text(encoding="utf-8"),
            object_pairs_hook=strict_object,
        )
        validate(value, artifact_paths)
        print(f"first-empty production boundary passed: {output}")
        return 0
    except (
        ControllerError,
        FirstEmptyError,
        FunctionalEvidenceError,
        OSError,
        subprocess.SubprocessError,
        json.JSONDecodeError,
    ) as error:
        print(f"first-empty production boundary failed: {error}",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
