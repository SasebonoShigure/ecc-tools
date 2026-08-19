"""Pure manifest, snapshot, numeric-comparison, and golden contracts."""

import hashlib
import json
import math
import os
import re
from pathlib import Path
from typing import Any

_NUMBER = (int, float)
_EXPECTED_MANIFEST_SEMANTICS = {
    "analysis_types": ["max", "min"],
    "transitions": ["rise", "fall"],
    "path_sources": ["input", "register"],
    "node_key": "full_pin_name",
    "arc_key": "source,sink,type,owner,lib_source,lib_sink,flags,parallel_ordinal",
    "missing_value": "typed_missing_sentinel",
    "infinite_value": "typed_pos_inf_or_neg_inf_sentinel",
    "root_level": 1,
    "propagation_scope": "full_graph",
}


class BaselineError(RuntimeError):
    """A baseline contract, execution, or comparison failure."""


def _object_without_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise BaselineError(f"duplicate JSON object key: {key!r}")
        result[key] = value
    return result


def _reject_constant(token: str) -> None:
    raise BaselineError(f"non-finite JSON token is forbidden: {token}")


def load_json(path: Path) -> Any:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            parse_constant=_reject_constant,
            object_pairs_hook=_object_without_duplicates,
        )
    except (OSError, json.JSONDecodeError) as exc:
        raise BaselineError(f"cannot read valid JSON from {path}: {exc}") from exc


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")
    os.replace(temporary, path)


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_mapping(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise BaselineError(f"{where} must be an object")
    return value


def require_list(value: Any, where: str) -> list[Any]:
    if not isinstance(value, list):
        raise BaselineError(f"{where} must be an array")
    return value


def require_exact_keys(value: dict[str, Any], expected: set[str], where: str) -> None:
    actual = set(value)
    if actual != expected:
        raise BaselineError(
            f"{where} keys differ; missing={sorted(expected - actual)}, "
            f"extra={sorted(actual - expected)}"
        )


def _resolve_contained(base: Path, relative: str, where: str) -> Path:
    candidate = Path(relative)
    if candidate.is_absolute():
        raise BaselineError(f"{where} must be relative: {relative}")
    resolved = (base / candidate).resolve()
    try:
        resolved.relative_to(base.resolve())
    except ValueError as exc:
        raise BaselineError(f"{where} escapes {base}: {relative}") from exc
    return resolved


def validate_manifest(
    manifest: dict[str, Any], manifest_path: Path, *, allow_unpinned_topology: bool
) -> None:
    require_exact_keys(
        manifest,
        {
            "schema_version",
            "description",
            "source_baseline",
            "time_unit",
            "semantics",
            "tolerances",
            "fresh_process_repeats",
            "golden_directory",
            "cases",
        },
        "manifest",
    )
    if manifest.get("schema_version") != 1:
        raise BaselineError("manifest schema_version must be integer 1")
    if not isinstance(manifest.get("description"), str) or not manifest["description"]:
        raise BaselineError("manifest description must be a non-empty string")
    if manifest.get("time_unit") != "ns":
        raise BaselineError("stage-0 baseline canonical time_unit must be ns")
    source = require_mapping(manifest.get("source_baseline"), "source_baseline")
    require_exact_keys(
        source,
        {"parent_branch", "parent_commit", "ecc_tools_branch", "ecc_tools_base_commit"},
        "source_baseline",
    )
    for branch_field in ("parent_branch", "ecc_tools_branch"):
        if source.get(branch_field) != "IncreGPUSTA":
            raise BaselineError(f"source_baseline.{branch_field} must be IncreGPUSTA")
    for sha_field in ("parent_commit", "ecc_tools_base_commit"):
        if re.fullmatch(r"[0-9a-f]{40}", str(source.get(sha_field))) is None:
            raise BaselineError(f"source_baseline.{sha_field} must be a lowercase Git SHA")
    semantics = require_mapping(manifest.get("semantics"), "semantics")
    require_exact_keys(semantics, set(_EXPECTED_MANIFEST_SEMANTICS), "semantics")
    for field, expected in _EXPECTED_MANIFEST_SEMANTICS.items():
        actual = semantics[field]
        if type(actual) is not type(expected) or actual != expected:
            raise BaselineError(
                f"semantics.{field} must remain frozen at {expected!r}; got {actual!r}"
            )
    tolerances = require_mapping(manifest.get("tolerances"), "tolerances")
    require_exact_keys(tolerances, {"absolute", "relative"}, "tolerances")
    for field in ("absolute", "relative"):
        value = tolerances.get(field)
        if (
            isinstance(value, bool)
            or not isinstance(value, _NUMBER)
            or value < 0
            or not math.isfinite(value)
        ):
            raise BaselineError(f"tolerances.{field} must be a finite non-negative number")
    repeats = require_mapping(manifest.get("fresh_process_repeats"), "fresh_process_repeats")
    require_exact_keys(repeats, {"ctest", "local"}, "fresh_process_repeats")
    for field in ("ctest", "local"):
        if type(repeats.get(field)) is not int or repeats[field] < 3:
            raise BaselineError(f"fresh_process_repeats.{field} must be an integer >= 3")
    if repeats["local"] < repeats["ctest"]:
        raise BaselineError("local repeat count must be at least the CTest repeat count")
    golden_directory = manifest.get("golden_directory")
    if not isinstance(golden_directory, str) or not golden_directory:
        raise BaselineError("golden_directory must be a non-empty relative directory")
    golden_path = _resolve_contained(manifest_path.parent, golden_directory, "golden_directory")
    if golden_path.parent != manifest_path.parent.resolve():
        raise BaselineError("golden_directory must be one direct child of the baseline directory")
    cases = require_list(manifest.get("cases"), "cases")
    if not cases:
        raise BaselineError("manifest must contain at least one case")
    names: set[str] = set()
    for index, raw_case in enumerate(cases):
        case = require_mapping(raw_case, f"cases[{index}]")
        require_exact_keys(
            case,
            {
                "name",
                "top",
                "verilog",
                "sdc",
                "expected_clock",
                "expected_topology_sha256",
                "required_artifacts",
                "input_sha256",
            },
            f"cases[{index}]",
        )
        name = case.get("name")
        if (
            not isinstance(name, str)
            or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", name) is None
            or name in names
        ):
            raise BaselineError(f"cases[{index}].name must be a unique safe slug")
        names.add(name)
        for field in ("top", "verilog", "sdc", "expected_clock"):
            if not isinstance(case.get(field), str) or not case[field]:
                raise BaselineError(f"cases[{index}].{field} must be a non-empty string")
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_$]*", case["top"]) is None:
            raise BaselineError(f"cases[{index}].top must be a safe Verilog identifier")
        verilog = _resolve_contained(
            manifest_path.parent, case["verilog"], f"cases[{index}].verilog"
        )
        sdc = _resolve_contained(manifest_path.parent, case["sdc"], f"cases[{index}].sdc")
        if not verilog.is_file() or not sdc.is_file():
            raise BaselineError(f"cases[{index}] Verilog/SDC input does not exist")
        topology_hash = case.get("expected_topology_sha256")
        if topology_hash is None:
            if not allow_unpinned_topology:
                raise BaselineError(
                    f"cases[{index}].expected_topology_sha256 is unpinned; "
                    "only --record-golden may bootstrap it"
                )
        elif re.fullmatch(r"[0-9a-f]{64}", str(topology_hash)) is None:
            raise BaselineError(
                f"cases[{index}].expected_topology_sha256 must be null or lowercase SHA-256"
            )
        required_artifacts = require_list(
            case.get("required_artifacts"), f"cases[{index}].required_artifacts"
        )
        if not required_artifacts or len(set(required_artifacts)) != len(required_artifacts):
            raise BaselineError(f"cases[{index}].required_artifacts must be non-empty and unique")
        for artifact in required_artifacts:
            if not isinstance(artifact, str) or not artifact:
                raise BaselineError(f"cases[{index}] artifact path must be a non-empty string")
            unknown = artifact.replace("{top}", "")
            if "{" in unknown or "}" in unknown:
                raise BaselineError(
                    f"cases[{index}] artifact contains an unknown placeholder: {artifact}"
                )
            _resolve_contained(Path("/baseline-run"), artifact.format(top=case["top"]), "artifact")
        hashes = require_mapping(case.get("input_sha256"), f"cases[{index}].input_sha256")
        expected_inputs = {
            "fixtures/stage0_tech.lef",
            "fixtures/stage0_cells.lef",
            "fixtures/stage0.lib",
            case["verilog"],
            case["sdc"],
        }
        if set(hashes) != expected_inputs:
            raise BaselineError(
                f"cases[{index}].input_sha256 must pin exactly the five consumed inputs; "
                f"missing={sorted(expected_inputs - set(hashes))}, "
                f"extra={sorted(set(hashes) - expected_inputs)}"
            )
        for relative, expected in hashes.items():
            if not isinstance(relative, str) or not re.fullmatch(r"[0-9a-f]{64}", str(expected)):
                raise BaselineError(f"invalid SHA-256 entry in cases[{index}].input_sha256")
            resolved = _resolve_contained(
                manifest_path.parent, relative, f"cases[{index}].input_sha256"
            )
            if not resolved.is_file():
                raise BaselineError(f"fixture does not exist: {resolved}")
            actual = sha256_file(resolved)
            if actual != expected:
                raise BaselineError(
                    f"fixture SHA-256 mismatch for {relative}: expected {expected}, got {actual}"
                )


_ALLOWED_SENTINELS = {"missing", "pos_inf", "neg_inf"}
_ENUM_FIELDS = {
    "analysis": {"none", "max", "min"},
    "transition": {"none", "rise", "fall"},
    "input_transition": {"none", "rise", "fall"},
    "output_transition": {"none", "rise", "fall"},
    "state_transition": {"none", "rise", "fall"},
    "predecessor_transition": {"none", "rise", "fall"},
    "crpr_clock_transition": {"none", "rise", "fall"},
    "source_type": {"none", "input", "register"},
    "direction": {"none", "input", "output", "inout"},
    "type": {"none", "cell", "net"},
    "arc_type": {"none", "cell", "net"},
    "check_type": {"none", "setup", "hold", "recovery", "removal", "width", "period"},
}


def _validate_json_contract(value: Any, path: str = "snapshot") -> None:
    if value is None:
        raise BaselineError(f"{path}: raw null is forbidden; use a typed sentinel")
    if type(value) in {bool, int, str}:
        return
    if type(value) is float:
        if not math.isfinite(value):
            raise BaselineError(f"{path}: raw non-finite float is forbidden")
        return
    if isinstance(value, list):
        for index, item in enumerate(value):
            _validate_json_contract(item, f"{path}[{index}]")
        return
    if isinstance(value, dict):
        if "sentinel" in value:
            if set(value) != {"sentinel"} or value["sentinel"] not in _ALLOWED_SENTINELS:
                raise BaselineError(f"{path}: malformed or unknown typed sentinel")
            return
        for key, item in value.items():
            enum_context = path.startswith(("snapshot.topology", "snapshot.timing"))
            collection_field = key == "predecessor_transition" and isinstance(item, list)
            if (
                enum_context
                and key in _ENUM_FIELDS
                and not collection_field
                and (type(item) is not str or item not in _ENUM_FIELDS[key])
            ):
                raise BaselineError(f"{path}.{key}: invalid enum value {item!r}")
            _validate_json_contract(item, f"{path}.{key}")
        return
    raise BaselineError(f"{path}: unsupported JSON value type {type(value).__name__}")


def validate_snapshot(snapshot: dict[str, Any], case: dict[str, Any]) -> None:
    require_exact_keys(
        snapshot,
        {
            "schema_version",
            "probe",
            "case",
            "fixture",
            "config",
            "semantics",
            "library_units",
            "constraints",
            "topology",
            "timing",
            "power",
            "complete",
        },
        "snapshot",
    )
    if (
        snapshot.get("schema_version") != 1
        or snapshot.get("probe") != "ista-stage0-full-baseline-v1"
    ):
        raise BaselineError("unsupported native snapshot schema/probe revision")
    if snapshot.get("complete") is not True or snapshot.get("case") != case["name"]:
        raise BaselineError("snapshot is incomplete or belongs to a different case")
    expected_fixture = {
        "case_name": case["name"],
        "top": case["top"],
        "inputs": {
            "tech_lef": "stage0_tech.lef",
            "cell_lef": "stage0_cells.lef",
            "liberty": "stage0.lib",
            "verilog": Path(case["verilog"]).name,
            "sdc": Path(case["sdc"]).name,
        },
    }
    if snapshot.get("fixture") != expected_fixture:
        raise BaselineError("snapshot fixture identity does not match the manifest case")
    expected_config = {
        "thread_number": 1,
        "output_timing_reports": 1,
        "output_timing_features": 1,
        "timing_path_limit": 20,
        "max_paths": 20,
        "nworst": 20,
        "timing_corner": "",
        "timing_report_delay_type": "max",
        "timing_report_start_end_type": "all",
        "is_path_report_number_specified": True,
        "slack_lesser_than": {"sentinel": "missing"},
        "slack_greater_than": {"sentinel": "missing"},
    }
    if snapshot.get("config") != expected_config:
        raise BaselineError("snapshot contains an unexpected effective STA configuration")
    expected_semantics = {
        "canonical_time_unit": "ns",
        "node_key": "full_pin_name",
        "arc_key": (
            "[source,sink,type,owner,library_source,library_sink,clock,"
            "disabled,loop_disabled]+parallel_ordinal"
        ),
        "missing": {"sentinel": "missing"},
        "positive_infinity": {"sentinel": "pos_inf"},
        "negative_infinity": {"sentinel": "neg_inf"},
        "nan": "invalid",
    }
    if snapshot.get("semantics") != expected_semantics:
        raise BaselineError("snapshot timing/sentinel semantics changed")
    if snapshot.get("library_units") != {
        "source_time": "ns",
        "source_capacitance": "pf",
        "source_resistance": "kohm",
    }:
        raise BaselineError("fixture library units changed")
    topology = require_mapping(snapshot.get("topology"), "snapshot.topology")
    require_exact_keys(
        topology,
        {
            "nodes",
            "arcs",
            "incoming",
            "outgoing",
            "start_points",
            "end_points",
            "timing_order",
            "counts",
        },
        "snapshot.topology",
    )
    nodes = require_list(topology["nodes"], "snapshot.topology.nodes")
    arcs = require_list(topology["arcs"], "snapshot.topology.arcs")
    if not nodes or not arcs:
        raise BaselineError("snapshot topology must contain nodes and arcs")
    node_keys = [node.get("key") for node in nodes if isinstance(node, dict)]
    arc_keys = [arc.get("key") for arc in arcs if isinstance(arc, dict)]
    if len(node_keys) != len(nodes) or len(set(node_keys)) != len(nodes):
        raise BaselineError("snapshot node keys must be present and unique")
    if len(arc_keys) != len(arcs) or len(set(arc_keys)) != len(arcs):
        raise BaselineError("snapshot arc keys must be present and unique")
    node_set = set(node_keys)
    for field in ("incoming", "outgoing"):
        adjacency = require_mapping(topology[field], f"snapshot.topology.{field}")
        if set(adjacency) != node_set:
            raise BaselineError(f"snapshot {field} rows do not match node keys")
    for field in ("start_points", "end_points", "timing_order"):
        values = require_list(topology[field], f"snapshot.topology.{field}")
        if not set(values).issubset(node_set) or len(values) != len(set(values)):
            raise BaselineError(f"snapshot {field} contains missing or duplicate nodes")
    if set(topology["timing_order"]) != node_set:
        raise BaselineError("snapshot timing_order must cover every node exactly once")
    counts = require_mapping(topology["counts"], "snapshot.topology.counts")
    require_exact_keys(
        counts,
        {"instances", "pins", "nets", "arcs", "timing_points", "path_groups"},
        "snapshot.topology.counts",
    )
    if any(type(value) is not int or value < 0 for value in counts.values()):
        raise BaselineError("snapshot topology counts must be non-negative integers")
    if (
        counts["pins"] != len(nodes)
        or counts["timing_points"] != len(nodes)
        or counts["arcs"] != len(arcs)
    ):
        raise BaselineError("snapshot topology counts disagree with node/arc arrays")
    timing = require_mapping(snapshot.get("timing"), "snapshot.timing")
    require_exact_keys(timing, {"points", "arcs", "path_groups", "summary"}, "snapshot.timing")
    if len(require_list(timing["points"], "snapshot.timing.points")) != len(nodes):
        raise BaselineError("snapshot timing-point array does not match topology")
    if len(require_list(timing["arcs"], "snapshot.timing.arcs")) != len(arcs):
        raise BaselineError("snapshot timing-arc array does not match topology")
    groups = require_list(timing["path_groups"], "snapshot.timing.path_groups")
    if not groups or counts["path_groups"] != len(groups):
        raise BaselineError("snapshot path-group count is empty or inconsistent")
    _validate_json_contract(snapshot)


def compare_values(
    expected: Any, actual: Any, path: str, absolute: float, relative: float, errors: list[str]
) -> None:
    if isinstance(expected, bool) or isinstance(actual, bool):
        if type(expected) is not type(actual) or expected != actual:
            errors.append(f"{path}: expected {expected!r}, got {actual!r}")
        return
    if isinstance(expected, _NUMBER) and isinstance(actual, _NUMBER):
        if type(expected) is int and type(actual) is int:
            if expected != actual:
                errors.append(f"{path}: expected integer {expected}, got {actual}")
            return
        if type(expected) is not float or type(actual) is not float:
            errors.append(f"{path}: numeric JSON type changed")
            return
        left = float(expected)
        right = float(actual)
        if not math.isfinite(left) or not math.isfinite(right):
            errors.append(f"{path}: raw non-finite numbers are forbidden; use a typed sentinel")
            return
        limit = absolute + relative * max(abs(left), abs(right))
        if abs(left - right) > limit:
            errors.append(f"{path}: expected {left:.17g}, got {right:.17g}, tolerance {limit:.3g}")
        return
    if isinstance(expected, dict) and isinstance(actual, dict):
        expected_keys = set(expected)
        actual_keys = set(actual)
        if expected_keys != actual_keys:
            errors.append(
                f"{path}: object keys differ; "
                f"missing={sorted(expected_keys - actual_keys)}, "
                f"extra={sorted(actual_keys - expected_keys)}"
            )
        for key in sorted(expected_keys & actual_keys):
            compare_values(expected[key], actual[key], f"{path}.{key}", absolute, relative, errors)
        return
    if isinstance(expected, list) and isinstance(actual, list):
        if len(expected) != len(actual):
            errors.append(f"{path}: array length expected {len(expected)}, got {len(actual)}")
        for index, (left, right) in enumerate(zip(expected, actual, strict=False)):
            compare_values(left, right, f"{path}[{index}]", absolute, relative, errors)
        return
    if type(expected) is not type(actual) or expected != actual:
        errors.append(f"{path}: expected {expected!r}, got {actual!r}")


def assert_equal(expected: Any, actual: Any, label: str, absolute: float, relative: float) -> None:
    errors: list[str] = []
    compare_values(expected, actual, label, absolute, relative, errors)
    if errors:
        preview = "\n".join(f"  - {entry}" for entry in errors[:40])
        suffix = "" if len(errors) <= 40 else f"\n  - ... {len(errors) - 40} more differences"
        raise BaselineError(f"{label} mismatch ({len(errors)} differences):\n{preview}{suffix}")


def semantic_hashes(
    snapshot: dict[str, Any], case: dict[str, Any], manifest: dict[str, Any]
) -> tuple[str, str]:
    topology = snapshot.get("topology")
    if not isinstance(topology, dict):
        raise BaselineError("probe snapshot has no topology object")
    config = snapshot.get("config")
    if not isinstance(config, dict):
        raise BaselineError("probe snapshot has no config object")
    config_contract = {
        "case": case["name"],
        "top": case["top"],
        "expected_clock": case["expected_clock"],
        "input_sha256": case["input_sha256"],
        "config": config,
        "source_baseline": manifest["source_baseline"],
        "semantics": manifest["semantics"],
        "time_unit": manifest["time_unit"],
        "tolerances": manifest["tolerances"],
    }
    return sha256_bytes(canonical_bytes(topology)), sha256_bytes(canonical_bytes(config_contract))


def golden_payload(
    case: dict[str, Any],
    manifest: dict[str, Any],
    snapshot: dict[str, Any],
    artifacts: dict[str, Any],
) -> dict[str, Any]:
    topology_hash, config_hash = semantic_hashes(snapshot, case, manifest)
    return {
        "golden_schema_version": 1,
        "case": case["name"],
        "topology_hash": topology_hash,
        "config_hash": config_hash,
        "snapshot": snapshot,
        "artifacts": artifacts,
        "provenance": {
            "input_sha256": case["input_sha256"],
            "source_baseline": manifest["source_baseline"],
            "tolerances": manifest["tolerances"],
            "artifact_paths": artifacts["inventory_paths"],
            "full_snapshot_sha256_diagnostic": sha256_bytes(canonical_bytes(snapshot)),
        },
    }


def validate_repeat_count(value: Any) -> int:
    if type(value) is not int or value < 3:
        raise BaselineError("fresh-process baseline requires --repeats >= 3")
    return value


def _expect_baseline_error(name: str, action: Any) -> None:
    try:
        action()
    except BaselineError:
        return
    raise BaselineError(f"negative controller self-test {name!r} was accepted")


def run_contract_self_test(manifest_path: Path) -> None:
    manifest = require_mapping(load_json(manifest_path), "manifest")
    validate_manifest(manifest, manifest_path, allow_unpinned_topology=True)

    missing_input = json.loads(canonical_bytes(manifest))
    del missing_input["cases"][0]["input_sha256"]["fixtures/stage0.lib"]
    _expect_baseline_error(
        "missing-consumed-input",
        lambda: validate_manifest(missing_input, manifest_path, allow_unpinned_topology=True),
    )

    escaping_golden = json.loads(canonical_bytes(manifest))
    escaping_golden["golden_directory"] = "../golden"
    _expect_baseline_error(
        "escaping-golden-directory",
        lambda: validate_manifest(escaping_golden, manifest_path, allow_unpinned_topology=True),
    )
    escaping_case = json.loads(canonical_bytes(manifest))
    escaping_case["cases"][0]["name"] = "../../escaped"
    _expect_baseline_error(
        "escaping-case-name",
        lambda: validate_manifest(escaping_case, manifest_path, allow_unpinned_topology=True),
    )
    typo_transition = json.loads(canonical_bytes(manifest))
    typo_transition["semantics"]["transitions"][1] = "fal"
    _expect_baseline_error(
        "semantics-transition-typo",
        lambda: validate_manifest(typo_transition, manifest_path, allow_unpinned_topology=True),
    )
    wrong_root_level = json.loads(canonical_bytes(manifest))
    wrong_root_level["semantics"]["root_level"] = 0
    _expect_baseline_error(
        "semantics-root-level",
        lambda: validate_manifest(wrong_root_level, manifest_path, allow_unpinned_topology=True),
    )
    extra_semantic = json.loads(canonical_bytes(manifest))
    extra_semantic["semantics"]["unexpected"] = True
    _expect_baseline_error(
        "semantics-extra-key",
        lambda: validate_manifest(extra_semantic, manifest_path, allow_unpinned_topology=True),
    )
    _expect_baseline_error(
        "unknown-sentinel",
        lambda: _validate_json_contract({"sentinel": "bogus"}),
    )
    _expect_baseline_error("raw-null", lambda: _validate_json_contract(None))
    _expect_baseline_error(
        "unknown-enum",
        lambda: _validate_json_contract({"analysis": "bogus"}, "snapshot.timing.self_test"),
    )
    _expect_baseline_error("two-repeats", lambda: validate_repeat_count(2))
    validate_repeat_count(3)

    errors: list[str] = []
    compare_values(1, 1.0, "numeric-type", 1e-9, 1e-9, errors)
    if not errors:
        raise BaselineError("contract self-test accepted an integer/float type change")
    errors.clear()
    compare_values(1.0, 1.0 + 5e-10, "tolerant-float", 1e-9, 1e-9, errors)
    if errors:
        raise BaselineError("contract self-test rejected an in-tolerance float change")
    compare_values(1.0, 1.1, "out-of-tolerance-float", 1e-9, 1e-9, errors)
    if not errors:
        raise BaselineError("contract self-test accepted an out-of-tolerance float change")

    hash_snapshot = {"topology": {}, "config": {}}
    case = manifest["cases"][0]
    _, base_config_hash = semantic_hashes(hash_snapshot, case, manifest)
    changed_source = json.loads(canonical_bytes(manifest))
    changed_source["source_baseline"]["parent_commit"] = "0" * 40
    _, changed_source_hash = semantic_hashes(hash_snapshot, case, changed_source)
    if changed_source_hash == base_config_hash:
        raise BaselineError("config hash does not cover source baseline provenance")
    changed_tolerance = json.loads(canonical_bytes(manifest))
    changed_tolerance["tolerances"]["absolute"] *= 10
    _, changed_tolerance_hash = semantic_hashes(hash_snapshot, case, changed_tolerance)
    if changed_tolerance_hash == base_config_hash:
        raise BaselineError("config hash does not cover comparison tolerance")

    print("contract self-test: 15 checks passed")
