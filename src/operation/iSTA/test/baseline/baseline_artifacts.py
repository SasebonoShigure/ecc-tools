"""Production artifact discovery, normalization, and comparison contracts."""

import copy
import re
from pathlib import Path, PurePosixPath
from typing import Any

from baseline_contract import (
    BaselineError,
    assert_equal,
    canonical_bytes,
    load_json,
    require_exact_keys,
    require_list,
    require_mapping,
    sha256_file,
    validate_manifest,
    validate_snapshot,
)

_SHA256 = re.compile(r"[0-9a-f]{64}")
_GOLDEN_KEYS = {
    "golden_schema_version",
    "case",
    "snapshot",
    "artifacts",
    "provenance",
}
_PROVENANCE_KEYS = {
    "source_baseline",
    "tolerances",
    "artifact_paths",
}
_ARTIFACT_KEYS = {
    "required_paths",
    "inventory_paths",
    "structured",
    "text_tokens",
    "binary_sha256",
}
_NUMBER_TOKEN = re.compile(
    r"(?<![A-Za-z0-9_])[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"
    r"(?![A-Za-z0-9_])"
)


def _artifact_paths(value: Any, where: str) -> list[str]:
    paths = require_list(value, where)
    for entry in paths:
        if type(entry) is not str or not entry:
            raise BaselineError(f"{where} must contain non-empty strings")
        path = PurePosixPath(entry)
        if path.is_absolute() or ".." in path.parts or path.as_posix() != entry:
            raise BaselineError(f"{where} contains an unsafe or non-canonical path: {entry!r}")
    if paths != sorted(set(paths)):
        raise BaselineError(f"{where} must be sorted and unique")
    return paths


def _validate_artifact_bundle(artifacts: dict[str, Any], case: dict[str, Any]) -> list[str]:
    require_exact_keys(artifacts, _ARTIFACT_KEYS, "golden.artifacts")
    required = _artifact_paths(artifacts["required_paths"], "golden.artifacts.required_paths")
    expected_required = sorted(
        entry.format(top=case["top"]) for entry in case["required_artifacts"]
    )
    if required != expected_required:
        raise BaselineError("golden artifact required_paths do not match the manifest case")
    inventory = _artifact_paths(artifacts["inventory_paths"], "golden.artifacts.inventory_paths")
    buckets = {
        name: require_mapping(artifacts[name], f"golden.artifacts.{name}")
        for name in ("structured", "text_tokens", "binary_sha256")
    }
    bucket_paths = [set(bucket) for bucket in buckets.values()]
    if any(
        left & right
        for index, left in enumerate(bucket_paths)
        for right in bucket_paths[index + 1 :]
    ):
        raise BaselineError("golden artifact inventory buckets overlap")
    if set().union(*bucket_paths) != set(inventory):
        raise BaselineError("golden artifact inventory does not match its content buckets")
    for relative, digest in buckets["binary_sha256"].items():
        if type(digest) is not str or _SHA256.fullmatch(digest) is None:
            raise BaselineError(f"golden binary artifact has an invalid SHA-256: {relative}")
    return inventory


def validate_golden(golden: dict[str, Any], case: dict[str, Any], manifest: dict[str, Any]) -> None:
    """Reject stale or internally inconsistent checked-in reference payloads."""
    require_exact_keys(golden, _GOLDEN_KEYS, f"golden[{case['name']}]")
    if type(golden.get("golden_schema_version")) is not int or golden["golden_schema_version"] != 1:
        raise BaselineError("golden_schema_version must be integer 1")
    if golden.get("case") != case["name"]:
        raise BaselineError("golden belongs to a different manifest case")
    snapshot = require_mapping(golden.get("snapshot"), "golden.snapshot")
    validate_snapshot(snapshot, case)
    artifacts = require_mapping(golden.get("artifacts"), "golden.artifacts")
    inventory = _validate_artifact_bundle(artifacts, case)
    provenance = require_mapping(golden.get("provenance"), "golden.provenance")
    require_exact_keys(provenance, _PROVENANCE_KEYS, "golden.provenance")
    expected_provenance = {
        "source_baseline": manifest["source_baseline"],
        "tolerances": manifest["tolerances"],
        "artifact_paths": inventory,
    }
    for field, expected in expected_provenance.items():
        if canonical_bytes(provenance[field]) != canonical_bytes(expected):
            raise BaselineError(f"golden provenance {field} is stale or inconsistent")


def normalize_text(text: str, run_dir: Path) -> str:
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    normalized = normalized.replace(str(run_dir), "<RUN_DIR>")
    normalized = re.sub(r'\(DATE\s+"[^"]*"\)', '(DATE "<NORMALIZED>")', normalized)
    normalized = re.sub(
        r"\b20\d\d[-/]\d\d[-/]\d\d[T ]\d\d:\d\d:\d\d(?:\.\d+)?(?:Z|[+-]\d\d:?\d\d)?\b",
        "<TIMESTAMP>",
        normalized,
    )
    normalized = re.sub(r"(?m)^.*(?:Peak memory|Elapsed|Wall time|CPU time).*$\n?", "", normalized)
    return "\n".join(line.rstrip() for line in normalized.split("\n")).rstrip() + "\n"


def tokenize_text(text: str, run_dir: Path) -> list[dict[str, Any]]:
    normalized = normalize_text(text, run_dir)
    tokens: list[dict[str, Any]] = []
    cursor = 0
    for match in _NUMBER_TOKEN.finditer(normalized):
        if match.start() > cursor:
            tokens.append({"kind": "text", "value": normalized[cursor : match.start()]})
        raw = match.group(0)
        if re.fullmatch(r"[-+]?\d+", raw):
            tokens.append({"kind": "integer", "value": int(raw)})
        else:
            tokens.append({"kind": "float", "value": float(raw)})
        cursor = match.end()
    if cursor < len(normalized):
        tokens.append({"kind": "text", "value": normalized[cursor:]})
    return tokens


def _expect_comparison_failure(name: str, reference: Any, candidate: Any) -> None:
    try:
        assert_equal(reference, candidate, name, 1e-9, 1e-9)
    except BaselineError:
        return
    raise BaselineError(f"artifact negative self-test {name!r} was accepted")


def _expect_golden_failure(
    name: str, golden: dict[str, Any], case: dict[str, Any], manifest: dict[str, Any]
) -> None:
    try:
        validate_golden(golden, case, manifest)
    except BaselineError:
        return
    raise BaselineError(f"golden negative self-test {name!r} was accepted")


def run_golden_self_test(manifest_path: Path) -> None:
    manifest = require_mapping(load_json(manifest_path), "manifest")
    validate_manifest(manifest, manifest_path)
    references: list[tuple[dict[str, Any], dict[str, Any]]] = []
    golden_dir = manifest_path.parent / manifest["golden_directory"]
    for case in manifest["cases"]:
        golden = require_mapping(load_json(golden_dir / f"{case['name']}.json"), "golden")
        validate_golden(golden, case, manifest)
        references.append((case, golden))

    case, reference = references[0]
    wrong_case = copy.deepcopy(reference)
    wrong_case["case"] = "wrong-case"
    _expect_golden_failure("wrong-case", wrong_case, case, manifest)
    stale_provenance = copy.deepcopy(reference)
    stale_provenance["provenance"]["source_baseline"]["parent_commit"] = "0" * 40
    _expect_golden_failure("stale-provenance", stale_provenance, case, manifest)
    extra_top_level = copy.deepcopy(reference)
    extra_top_level["unexpected"] = True
    _expect_golden_failure("extra-top-level-key", extra_top_level, case, manifest)
    extra_provenance = copy.deepcopy(reference)
    extra_provenance["provenance"]["unexpected"] = True
    _expect_golden_failure("extra-provenance-key", extra_provenance, case, manifest)
    print("golden self-test passed")


def run_artifact_self_test() -> None:
    run_dir = Path("/unused")
    reference = tokenize_text("(IOPATH A Y (0.1000000000))\n", run_dir)
    within_tolerance = tokenize_text("(IOPATH A Y (0.1000000005))\n", run_dir)
    assert_equal(reference, within_tolerance, "artifact-float-tolerance", 1e-9, 1e-9)

    _expect_comparison_failure(
        "sdf-delay-change",
        reference,
        tokenize_text("(IOPATH A Y (0.2000000000))\n", run_dir),
    )
    _expect_comparison_failure(
        "sdf-bus-index-change",
        tokenize_text("(INTERCONNECT data[0] out)\n", run_dir),
        tokenize_text("(INTERCONNECT data[1] out)\n", run_dir),
    )
    _expect_comparison_failure(
        "report-content-change",
        tokenize_text("Total Power = 1.0 mW\n", run_dir),
        tokenize_text("garbage\n", run_dir),
    )
    print("artifact self-test: 4 checks passed")


def artifact_bundle(run_dir: Path, case: dict[str, Any]) -> dict[str, Any]:
    required = [entry.format(top=case["top"]) for entry in case["required_artifacts"]]
    for relative in required:
        path = run_dir / relative
        if not path.is_file() or path.stat().st_size == 0:
            raise BaselineError(f"required production artifact is missing or empty: {path}")

    inventory_paths: list[str] = []
    structured: dict[str, Any] = {}
    text_tokens: dict[str, list[dict[str, Any]]] = {}
    binary_sha256: dict[str, str] = {}
    excluded = {"snapshot.json", "probe.stdout.log", "probe.stderr.log", "sta.log"}
    for path in sorted(item for item in run_dir.rglob("*") if item.is_file()):
        relative = path.relative_to(run_dir).as_posix()
        if relative in excluded:
            continue
        inventory_paths.append(relative)
        if path.suffix.lower() == ".json":
            structured[relative] = load_json(path)
        elif path.suffix.lower() in {".rpt", ".sdf", ".txt"}:
            try:
                text_tokens[relative] = tokenize_text(
                    path.read_text(encoding="utf-8", errors="strict"), run_dir
                )
            except UnicodeDecodeError as exc:
                raise BaselineError(f"text artifact is not UTF-8: {path}") from exc
        else:
            binary_sha256[relative] = sha256_file(path)
    return {
        "required_paths": sorted(required),
        "inventory_paths": inventory_paths,
        "structured": structured,
        "text_tokens": text_tokens,
        "binary_sha256": binary_sha256,
    }


def compare_artifacts(
    reference: dict[str, Any],
    candidate: dict[str, Any],
    absolute: float,
    relative: float,
    label: str,
) -> None:
    assert_equal(reference, candidate, label, absolute, relative)
