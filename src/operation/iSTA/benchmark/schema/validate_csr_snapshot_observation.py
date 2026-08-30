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
"""Validate the normative CSR snapshot observation schema and corpus."""

import argparse
import copy
import math
import sys
from pathlib import Path

from json_schema_contract import ValidationError, fail, load_json, validate_schema

SCHEMA_PATH = Path(__file__).with_name("csr_snapshot_observation_v1.schema.json")
MANIFEST_PATH = Path(__file__).with_name("csr_snapshot_contract_manifest_v1.json")
_U64_MAX = (1 << 64) - 1
_FINGERPRINT_KEYS = (
    "topology_fingerprint",
    "clock_constraint_fingerprint",
    "library_fingerprint",
    "corner_fingerprint",
    "parasitic_fingerprint",
    "timing_input_fingerprint",
)
_REASONS = (
    "parasitic_net",
    "sequential_instance",
    "sequential_launch",
    "timing_check",
    "crpr",
    "propagated_clock",
    "clock_tree_fanout",
    "clock_gating_or_divider",
    "conditional_timing_arc",
    "case_analysis",
)
_INVALIDATION_REASONS = (
    "topology_changed",
    "clock_constraint_changed",
    "library_changed",
    "corner_changed",
    "parasitic_changed",
    "timing_input_changed",
)
_ACTION_TOKENS = {
    "read",
    "reference_measurement",
    "scatter_rejection",
    "scatter_success",
    "invalidation",
    "release",
}
_RECIPE_OPS = {
    "replace_value",
    "delete_key",
    "add_key",
    "add_non_string_key",
    "nan",
    "positive_infinity",
    "negative_infinity",
    "unknown_enum",
    "duplicate_key",
}


def _duration(value: object, field: str) -> float:
    try:
        result = float(value)
    except OverflowError:
        fail(field, "must convert to a finite float", "non_finite")
    if not math.isfinite(result):
        fail(field, "must be finite", "non_finite")
    return result


def _validate_extent(extent: dict[str, object], field: str) -> None:
    if extent["capacity"] < extent["logical_size"]:
        fail(field, "capacity must be at least logical_size")


def _validate_support(support: dict[str, object]) -> None:
    reasons = support["unsupported_reasons"]
    ordered = sorted(set(reasons), key=_REASONS.index)
    if reasons != ordered:
        fail(
            "$.support.unsupported_reasons",
            "reasons must be unique and in stable enum order",
        )
    if support["reference_supported"] != (not reasons):
        fail("$.support", "reference_supported must equal reasons.empty")


def _validate_rejection(event: dict[str, object], identity: dict[str, object]) -> None:
    field = "$.last_rejection"
    if event["writes_applied"] != 0:
        fail(f"{field}.writes_applied", "must be zero")
    current = event["current"]
    if current != identity:
        fail(f"{field}.current", "must equal the outer identity")
    source = event["source"]
    reason = event["reason"]
    if reason == "schema_mismatch":
        valid = source is None
    elif source is None:
        valid = False
    elif reason == "session_mismatch":
        valid = source["session_token"] != current["session_token"]
    elif reason == "generation_mismatch":
        valid = (
            source["session_token"] == current["session_token"]
            and source["graph_generation"] != current["graph_generation"]
        )
    elif reason == "source_identity_mismatch":
        valid = (
            source["session_token"] == current["session_token"]
            and source["graph_generation"] == current["graph_generation"]
            and source != current
        )
    else:
        valid = source == current
    if not valid:
        fail(field, "reason/source identity relationship is invalid")


def _first_invalidation(identity: dict[str, object], current: dict[str, object]) -> str | None:
    for key, reason in zip(_FINGERPRINT_KEYS, _INVALIDATION_REASONS, strict=True):
        if current[key] != identity[key]:
            return reason
    return None


def _validate_invalidation(event: dict[str, object], identity: dict[str, object]) -> None:
    field = "$.last_invalidation"
    if event["writes_applied"] != 0:
        fail(f"{field}.writes_applied", "must be zero")
    if event["previous_identity"] != identity:
        fail(f"{field}.previous_identity", "must equal the outer identity")
    inferred = _first_invalidation(identity, event["current_fingerprints"])
    if inferred is None:
        fail(field, "at least one current fingerprint must differ or be null")
    if event["reason"] != inferred:
        fail(f"{field}.reason", "must identify the first changed fingerprint")


def _validate_semantics(document: dict[str, object]) -> None:
    counts = document["counts"]
    buffers = document["buffers"]
    for key, extent in buffers.items():
        _validate_extent(extent, f"$.buffers.{key}")
    nodes, arcs, levels = counts["nodes"], counts["arcs"], counts["levels"]
    expected = {
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
    for key, logical in expected.items():
        if logical > _U64_MAX:
            fail(f"$.buffers.{key}.logical_size", "derived size overflows u64", "overflow")
        if buffers[key]["logical_size"] != logical:
            fail(
                f"$.buffers.{key}.logical_size",
                "does not match the count-derived logical size",
            )
    if (nodes == 0) != (levels == 0):
        fail("$.counts", "nodes are empty exactly when levels are empty")
    if nodes == 0 and arcs != 0:
        fail("$.counts.arcs", "an empty graph cannot contain arcs")
    if nodes and not 1 <= levels <= nodes:
        fail("$.counts.levels", "must be in [1, nodes] for a nonempty graph")
    _validate_support(document["support"])
    timing = document["timing_ms"]
    for key in ("export", "reference", "scatter"):
        if timing[key] is not None:
            _duration(timing[key], f"$.timing_ms.{key}")
    host = document["host_bytes"]
    if host["capacity"] < host["logical"]:
        fail("$.host_bytes", "capacity must be at least logical")
    identity = document["identity"]
    rejection = document["last_rejection"]
    invalidation = document["last_invalidation"]
    if rejection is not None:
        _validate_rejection(rejection, identity)
    if invalidation is not None:
        _validate_invalidation(invalidation, identity)
    if (rejection is not None or invalidation is not None) and timing["scatter"] is None:
        fail("$.timing_ms.scatter", "an event requires scatter timing")
    state = document["state"]
    if state == "ready":
        valid_events = invalidation is None
    elif state == "invalidated":
        valid_events = rejection is None and invalidation is not None
    else:
        valid_events = invalidation is None or rejection is None and invalidation is not None
    if not valid_events:
        fail("$.state", "state and event combination is invalid")


def validate_snapshot_document(document: object, schema: object) -> None:
    """Run declarative shape checks followed by frozen cross-field semantics."""

    validate_schema(document, schema)
    if document is not None:
        if not isinstance(document, dict):
            fail("$", "must be a mapping", "value_type")
        _validate_semantics(document)


def _target_parent(document: object, target: str) -> tuple[dict[object, object], str]:
    if not target.startswith("$."):
        raise AssertionError(f"unsupported mutation target {target!r}")
    parts = target[2:].split(".")
    current = document
    for part in parts[:-1]:
        if not isinstance(current, dict):
            raise AssertionError(f"non-object mutation parent for {target!r}")
        current = current[part]
    if not isinstance(current, dict):
        raise AssertionError(f"non-object mutation parent for {target!r}")
    return current, parts[-1]


def _rejection_fixture(document: dict[str, object], name: str) -> dict[str, object]:
    identity = copy.deepcopy(document["identity"])
    source = None if name == "schema_mismatch_rejection" else copy.deepcopy(identity)
    return {
        "reason": "schema_mismatch",
        "writes_applied": 0,
        "source": source,
        "current": copy.deepcopy(identity),
    }


def _apply_recipe(document: object, recipe: dict[str, object]) -> object:
    operation, target = recipe["op"], recipe["target"]
    if target == "$fixture.empty_graph_with_arc":
        document["counts"]["arcs"] = 1
        for key in ("arc_keys", "out_arc_ids", "in_arc_ids"):
            document["buffers"][key] = {"logical_size": 1, "capacity": 1}
        return document
    if operation == "add_non_string_key":
        if not isinstance(document, dict):
            raise AssertionError("non-string-key recipe requires a mapping")
        document[recipe["operand"]] = 1
        return document
    if target == "$":
        return copy.deepcopy(recipe["operand"])
    parent, key = _target_parent(document, target)
    if operation == "delete_key":
        del parent[key]
        return document
    operand = recipe["operand"]
    if isinstance(operand, str) and operand.startswith("fixture:"):
        operand = _rejection_fixture(document, operand.removeprefix("fixture:"))
    values = {
        "nan": math.nan,
        "positive_infinity": math.inf,
        "negative_infinity": -math.inf,
    }
    parent[key] = values.get(operation, copy.deepcopy(operand))
    return document


def _assert_expected(case: dict[str, object], action: object) -> None:
    expected = case["expected"]
    try:
        action()
    except ValidationError as error:
        if expected["validity"] != "invalid":
            raise AssertionError(f"{case['case_id']}: unexpectedly rejected: {error}") from error
        if (error.field, error.category) != (expected["field"], expected["category"]):
            raise AssertionError(
                f"{case['case_id']}: got {(error.field, error.category)!r}, "
                f"expected {(expected['field'], expected['category'])!r}"
            ) from error
        exact = expected.get("message")
        prefix = expected.get("message_prefix")
        if exact is not None and error.message != exact:
            raise AssertionError(
                f"{case['case_id']}: message mismatch {error.message!r}"
            ) from error
        if prefix is not None and not error.message.startswith(prefix):
            raise AssertionError(
                f"{case['case_id']}: message prefix mismatch {error.message!r}"
            ) from error
        return
    if expected["validity"] != "valid":
        raise AssertionError(f"{case['case_id']}: invalid case was accepted")


def _load_baseline(name: str, cases: dict[str, object], corpus_root: Path) -> object:
    baseline = cases["baselines"][name]
    return (
        copy.deepcopy(baseline)
        if not isinstance(baseline, str)
        else load_json(corpus_root / baseline)
    )


def _validate_case_metadata(case: dict[str, object], lane: str) -> None:
    case_id = case.get("case_id")
    if type(case_id) is not str or not case_id:
        raise AssertionError(f"{lane}: case_id must be a nonempty string")
    if case.get("lane") != lane:
        raise AssertionError(f"{case_id}: wrong lane")
    recipe = case.get("recipe")
    if recipe is not None:
        if not isinstance(recipe, dict) or recipe.get("op") not in _RECIPE_OPS:
            raise AssertionError(f"{case_id}: unknown recipe operation")
        if type(recipe.get("target")) is not str:
            raise AssertionError(f"{case_id}: recipe target must be a string")
    expected = case.get("expected")
    if not isinstance(expected, dict) or expected.get("validity") not in {"valid", "invalid"}:
        raise AssertionError(f"{case_id}: invalid expected result")
    if type(expected.get("field")) is not str or type(expected.get("category")) is not str:
        raise AssertionError(f"{case_id}: expected field/category must be strings")
    message_keys = {"message", "message_prefix"} & set(expected)
    if len(message_keys) != 1 or type(expected[next(iter(message_keys))]) is not str:
        raise AssertionError(f"{case_id}: expected must have one string message form")
    if expected["validity"] == "valid":
        if (expected["field"], expected["category"], expected.get("message")) != (
            "-",
            "-",
            "-",
        ):
            raise AssertionError(f"{case_id}: valid expected result must use '-' fields")
    elif expected["field"] == "-" or expected["category"] == "-":
        raise AssertionError(f"{case_id}: invalid expected result must be structured")


def _run_corpus(schema: object, manifest_path: Path) -> int:
    manifest = load_json(manifest_path)
    if not isinstance(manifest, dict) or manifest.get("schema_version") != "1.0.0":
        raise AssertionError("invalid snapshot corpus manifest")
    root = manifest_path.parent
    assets = manifest["lane_case_assets"]
    if not isinstance(assets, dict) or set(assets) != {"raw_loader", "mapping", "transition"}:
        raise AssertionError("manifest lane_case_assets must name exactly three lanes")
    raw_cases = load_json(root / assets["raw_loader"])
    mapping_cases = load_json(root / assets["mapping"])
    transition_cases = load_json(root / assets["transition"])
    for lane, document in (
        ("raw_loader", raw_cases),
        ("mapping", mapping_cases),
        ("transition", transition_cases),
    ):
        if (
            not isinstance(document, dict)
            or document.get("schema_version") != "1.0.0"
            or not isinstance(document.get("cases"), list)
        ):
            raise AssertionError(f"invalid {lane} case inventory")
    corpus_root = (root / assets["mapping"]).parent
    case_ids: set[str] = set()
    checks = 0
    for case in raw_cases["cases"]:
        _validate_case_metadata(case, "raw_loader")
        if case["case_id"] in case_ids:
            raise AssertionError(f"duplicate corpus case id {case['case_id']!r}")
        case_ids.add(case["case_id"])
        if type(case.get("asset")) is not str:
            raise AssertionError(f"{case['case_id']}: raw asset must be a string")
        asset = (root / assets["raw_loader"]).parent / case["asset"]
        _assert_expected(
            case, lambda asset=asset: validate_snapshot_document(load_json(asset), schema)
        )
        checks += 1
    for case in mapping_cases["cases"]:
        _validate_case_metadata(case, "mapping")
        if case["case_id"] in case_ids:
            raise AssertionError(f"duplicate corpus case id {case['case_id']!r}")
        case_ids.add(case["case_id"])
        baseline = _load_baseline(case["baseline"], mapping_cases, corpus_root)
        validate_snapshot_document(baseline, schema)
        mutated = _apply_recipe(copy.deepcopy(baseline), case["recipe"])
        _assert_expected(case, lambda mutated=mutated: validate_snapshot_document(mutated, schema))
        checks += 1
    for case in transition_cases["cases"]:
        _validate_case_metadata(case, "transition")
        if case["case_id"] in case_ids:
            raise AssertionError(f"duplicate corpus case id {case['case_id']!r}")
        case_ids.add(case["case_id"])
        if case.get("baseline") not in {
            "absent",
            "ready",
            "ready_rejected",
            "invalidated",
            "released",
        }:
            raise AssertionError(f"{case['case_id']}: unknown transition baseline")
        action = case.get("action")
        if not isinstance(action, dict) or action.get("token") not in _ACTION_TOKENS:
            raise AssertionError(f"{case['case_id']}: unknown action token")
        after = case.get("after")
        if after != "reducer" and (
            not isinstance(after, dict) or after.get("op") not in _RECIPE_OPS
        ):
            raise AssertionError(f"{case['case_id']}: invalid after specification")
        checks += 1
    for case in manifest["typed_cpp_cases"]:
        if case["case_id"] in case_ids:
            raise AssertionError(f"duplicate corpus case id {case['case_id']!r}")
        case_ids.add(case["case_id"])
        if case["recipe"]["op"] not in _RECIPE_OPS:
            raise AssertionError(f"{case['case_id']}: unknown recipe operation")
        action = case["action_token"]
        if action != "-" and action not in _ACTION_TOKENS:
            raise AssertionError(f"{case['case_id']}: unknown action token")
        checks += 1
    return checks


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("documents", nargs="*", type=Path)
    parser.add_argument("--schema", type=Path, default=SCHEMA_PATH)
    parser.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if not args.self_test and not args.documents:
        parser.error("provide at least one document or use --self-test")
    try:
        schema = load_json(args.schema)
        if args.self_test:
            checks = _run_corpus(schema, args.manifest)
            print(f"snapshot self-test: {checks} checks passed")
        for path in args.documents:
            validate_snapshot_document(load_json(path), schema)
            print(f"valid: {path}")
    except (ValidationError, AssertionError, KeyError, TypeError) as error:
        print(f"validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
