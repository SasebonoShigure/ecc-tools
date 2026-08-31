#!/usr/bin/env python3
"""Validate frozen Stage 4 trace metadata without treating digests as correctness."""

import argparse
import json
import pathlib
import sys
from typing import Any


class ManifestError(RuntimeError):
    pass


def object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ManifestError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"),
                           object_pairs_hook=object_pairs)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ManifestError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise ManifestError(f"{path} must contain an object")
    return value


def exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    if set(value) != expected:
        raise ManifestError(f"{label} has a noncanonical key set")


def validate(manifest_path: pathlib.Path) -> None:
    manifest = read_json(manifest_path)
    exact_keys(manifest, {"schema_version", "correctness_basis",
                          "integrity_usage", "traces"}, "manifest")
    if manifest["schema_version"] != "buffer-trace-fixture-manifest/1.0.0":
        raise ManifestError("unknown fixture manifest version")
    if manifest["correctness_basis"] != "field_level_functional_receipts":
        raise ManifestError("functional field receipts are not the correctness basis")
    if manifest["integrity_usage"] != "artifact_identity_auxiliary_only":
        raise ManifestError("digest usage is not auxiliary-only")
    traces = manifest["traces"]
    if not isinstance(traces, list) or len(traces) != 2:
        raise ManifestError("manifest must freeze exactly two Stage 4 traces")
    expected_paths = {"synthetic_two_batch.json", "real_buffer_fanout.json"}
    if {entry.get("path") for entry in traces} != expected_paths:
        raise ManifestError("manifest trace path set differs")
    entry_keys = {
        "path", "trace_schema_version", "selection_mode", "workload_kind",
        "profile_id", "seed", "comparability_status", "comparability_reason",
        "batch_count", "accepted_edit_count", "auxiliary_sha256",
    }
    for index, entry in enumerate(traces):
        if not isinstance(entry, dict):
            raise ManifestError(f"manifest.traces[{index}] must be an object")
        exact_keys(entry, entry_keys, f"manifest.traces[{index}]")
        relative = pathlib.PurePosixPath(entry["path"])
        if relative.is_absolute() or len(relative.parts) != 1:
            raise ManifestError("trace path must be one repository-local leaf")
        trace = read_json(manifest_path.parent / entry["path"])
        exact_keys(trace, {"replay", "generation_only", "integrity"},
                   f"trace {entry['path']}")
        header = trace["replay"]["header"]
        workload = trace["replay"]["workload"]
        batches = workload["batches"]
        tokens = [edit["creation_token"] for batch in batches
                  for edit in batch["edits"]]
        observed = {
            "trace_schema_version": header["schema_version"],
            "selection_mode": header["selection_mode"],
            "workload_kind": header["semantic_workload_kind"],
            "profile_id": header["profile_id"],
            "seed": tokens[0]["seed"],
            "comparability_status": header["comparability_status"],
            "comparability_reason": header.get("comparability_reason"),
            "batch_count": len(batches),
            "accepted_edit_count": len(tokens),
            "auxiliary_sha256": trace["integrity"]["frozen_artifact_digest"],
        }
        for field, value in observed.items():
            if entry[field] != value:
                raise ManifestError(f"{entry['path']} metadata differs at {field}")
        if trace["integrity"]["algorithm"] != "sha256":
            raise ManifestError(f"{entry['path']} uses an unknown auxiliary digest")
        if any(token["seed"] != entry["seed"] for token in tokens):
            raise ManifestError(f"{entry['path']} has inconsistent edit seeds")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        validate(args.manifest.resolve(strict=True))
        print("buffer trace fixture manifest: 2 frozen traces validated")
        return 0
    except ManifestError as error:
        print(f"fixture manifest validation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
