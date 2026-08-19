#!/usr/bin/env python3
"""Validate incremental STA sample documents without third-party packages."""

import argparse
import copy
import json
import math
import re
import sys
from collections.abc import Callable
from datetime import datetime
from pathlib import Path
from typing import NoReturn

SCHEMA_PATH = Path(__file__).with_name("incremental_sta_samples_v1.schema.json")


class ValidationError(ValueError):
    """Raised when input JSON or its schema contract is invalid."""


def _fail(path: str, message: str) -> NoReturn:
    raise ValidationError(f"{path}: {message}")


def _reject_constant(token: str) -> NoReturn:
    raise ValidationError(f"invalid JSON numeric constant {token!r}")


def _object_without_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON object key {key!r}")
        result[key] = value
    return result


def _loads_json(text: str) -> object:
    try:
        return json.loads(
            text,
            parse_constant=_reject_constant,
            object_pairs_hook=_object_without_duplicates,
        )
    except json.JSONDecodeError as error:
        raise ValidationError(
            f"invalid JSON at line {error.lineno}, column {error.colno}: {error.msg}"
        ) from error


def _load_json(path: Path) -> object:
    try:
        return _loads_json(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError) as error:
        raise ValidationError(f"cannot read {path}: {error}") from error


def _resolve_ref(root_schema: dict[str, object], reference: str) -> dict[str, object]:
    if not reference.startswith("#/"):
        raise ValidationError(f"unsupported non-local schema reference {reference!r}")

    current: object = root_schema
    for raw_part in reference[2:].split("/"):
        part = raw_part.replace("~1", "/").replace("~0", "~")
        if not isinstance(current, dict) or part not in current:
            raise ValidationError(f"unresolvable schema reference {reference!r}")
        current = current[part]

    if not isinstance(current, dict):
        raise ValidationError(f"schema reference {reference!r} is not an object")
    return current


def _is_number(value: object) -> bool:
    return type(value) is int or type(value) is float and math.isfinite(value)


def _matches_type(value: object, expected: str) -> bool:
    match expected:
        case "null":
            return value is None
        case "boolean":
            return type(value) is bool
        case "integer":
            return type(value) is int
        case "number":
            return _is_number(value)
        case "string":
            return type(value) is str
        case "array":
            return type(value) is list
        case "object":
            return type(value) is dict
        case _:
            raise ValidationError(f"unsupported schema type {expected!r}")


def _json_equal(left: object, right: object) -> bool:
    if _is_number(left) and _is_number(right):
        return left == right
    return type(left) is type(right) and left == right


def _validate_datetime(value: str, path: str) -> None:
    if not re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})",
        value,
    ):
        _fail(path, "must be an RFC 3339 timestamp with an explicit UTC offset")
    try:
        parsed = datetime.fromisoformat(
            value.removesuffix("Z") + ("+00:00" if value.endswith("Z") else "")
        )
    except ValueError as error:
        _fail(path, f"invalid date-time: {error}")
    if parsed.tzinfo is None:
        _fail(path, "date-time must include a timezone")


def _schema_matches(
    value: object, schema: dict[str, object], root_schema: dict[str, object], path: str
) -> bool:
    try:
        _validate(value, schema, root_schema, path)
    except ValidationError:
        return False
    return True


def _validate(
    value: object,
    schema: dict[str, object],
    root_schema: dict[str, object],
    path: str = "$",
) -> None:
    reference = schema.get("$ref")
    if reference is not None:
        if not isinstance(reference, str):
            raise ValidationError("schema $ref must be a string")
        _validate(value, _resolve_ref(root_schema, reference), root_schema, path)
        return

    all_of = schema.get("allOf")
    if all_of is not None:
        if not isinstance(all_of, list) or not all(isinstance(item, dict) for item in all_of):
            raise ValidationError("schema allOf must be an array of objects")
        for item in all_of:
            _validate(value, item, root_schema, path)

    any_of = schema.get("anyOf")
    if any_of is not None:
        if not isinstance(any_of, list) or not all(isinstance(item, dict) for item in any_of):
            raise ValidationError("schema anyOf must be an array of objects")
        if not any(_schema_matches(value, item, root_schema, path) for item in any_of):
            _fail(path, "must match at least one anyOf branch")

    negated = schema.get("not")
    if negated is not None:
        if not isinstance(negated, dict):
            raise ValidationError("schema not must be an object")
        if _schema_matches(value, negated, root_schema, path):
            _fail(path, "must not match the excluded schema")

    condition = schema.get("if")
    if condition is not None:
        if not isinstance(condition, dict):
            raise ValidationError("schema if must be an object")
        branch_name = "then" if _schema_matches(value, condition, root_schema, path) else "else"
        branch = schema.get(branch_name)
        if branch is not None:
            if not isinstance(branch, dict):
                raise ValidationError(f"schema {branch_name} must be an object")
            _validate(value, branch, root_schema, path)

    if "const" in schema and not _json_equal(value, schema["const"]):
        _fail(path, f"must equal {schema['const']!r}")

    enum = schema.get("enum")
    if enum is not None:
        if not isinstance(enum, list):
            raise ValidationError("schema enum must be an array")
        if not any(_json_equal(value, candidate) for candidate in enum):
            _fail(path, f"must be one of {enum!r}")

    expected_types = schema.get("type")
    if expected_types is not None:
        if isinstance(expected_types, str):
            expected = [expected_types]
        elif isinstance(expected_types, list) and all(
            isinstance(item, str) for item in expected_types
        ):
            expected = expected_types
        else:
            raise ValidationError("schema type must be a string or string array")
        if not any(_matches_type(value, item) for item in expected):
            _fail(path, f"expected {' or '.join(expected)}, got {type(value).__name__}")

    if value is None:
        return

    if type(value) is dict:
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        if not isinstance(properties, dict) or not isinstance(required, list):
            raise ValidationError("schema object properties/required have invalid types")

        missing = [key for key in required if key not in value]
        if missing:
            _fail(path, f"missing required properties: {', '.join(missing)}")

        if schema.get("additionalProperties") is False:
            unexpected = sorted(set(value) - set(properties))
            if unexpected:
                _fail(path, f"unexpected properties: {', '.join(unexpected)}")

        for key, child_schema in properties.items():
            if key in value:
                if not isinstance(child_schema, dict):
                    raise ValidationError(f"schema for property {key!r} is not an object")
                _validate(value[key], child_schema, root_schema, f"{path}.{key}")

    if type(value) is list:
        minimum_items = schema.get("minItems")
        if minimum_items is not None and len(value) < minimum_items:
            _fail(path, f"must contain at least {minimum_items} items")

        if schema.get("uniqueItems") is True:
            canonical_items = [
                json.dumps(item, sort_keys=True, separators=(",", ":")) for item in value
            ]
            if len(set(canonical_items)) != len(canonical_items):
                _fail(path, "array items must be unique")

        item_schema = schema.get("items")
        if item_schema is not None:
            if not isinstance(item_schema, dict):
                raise ValidationError("schema items must be an object")
            for index, item in enumerate(value):
                _validate(item, item_schema, root_schema, f"{path}[{index}]")

    if type(value) is str:
        minimum_length = schema.get("minLength")
        if minimum_length is not None and len(value) < minimum_length:
            _fail(path, f"must contain at least {minimum_length} characters")

        pattern = schema.get("pattern")
        if pattern is not None:
            if not isinstance(pattern, str):
                raise ValidationError("schema pattern must be a string")
            if re.search(pattern, value) is None:
                _fail(path, f"does not match {pattern!r}")

        if schema.get("format") == "date-time":
            _validate_datetime(value, path)

    if _is_number(value):
        minimum = schema.get("minimum")
        maximum = schema.get("maximum")
        exclusive_minimum = schema.get("exclusiveMinimum")
        if minimum is not None and value < minimum:
            _fail(path, f"must be >= {minimum}")
        if maximum is not None and value > maximum:
            _fail(path, f"must be <= {maximum}")
        if exclusive_minimum is not None and value <= exclusive_minimum:
            _fail(path, f"must be > {exclusive_minimum}")


SUMMARY_STAT_FIELDS = ("min", "median", "p95", "max", "mad", "mean", "stddev")
SPEEDUP_FIELDS = (
    "speedup_vs_cpu_full",
    "speedup_vs_cpu_incremental",
    "speedup_vs_gpu_full",
)


def _validate_incremental_case(case: dict[str, object], path: str) -> None:
    if case["batch_size"] == 0:
        _fail(f"{path}.batch_size", "incremental_update requires a non-empty batch")
    if case["trace_hash"] is None:
        _fail(f"{path}.trace_hash", "incremental_update requires trace provenance")
    if case["changed_nodes"] == 0 and case["changed_arcs"] == 0:
        _fail(path, "incremental_update must change at least one node or arc")
    if case["mutation_kind"] == "baseline":
        _fail(f"{path}.mutation_kind", "incremental_update cannot use baseline mutation")
    if case["semantic_workload_kind"] == "baseline":
        _fail(
            f"{path}.semantic_workload_kind",
            "incremental_update cannot use baseline workload",
        )


def _validate_batch_sample(sample: dict[str, object], kind: object, path: str) -> None:
    batch_sample = sample["batch_sample"]
    if kind == "baseline_only":
        if batch_sample is not None:
            _fail(f"{path}.batch_sample", "baseline_only has no measured edit batch")
        return
    if not isinstance(batch_sample, dict):
        _fail(f"{path}.batch_sample", f"{kind} requires a measured batch sample")

    timing_fields = ("update_core_ms", "session_step_ms", "artifact_step_ms")
    if any(batch_sample[field] is None for field in timing_fields):
        _fail(
            f"{path}.batch_sample",
            "measured batches require update_core/session_step/artifact_step timings",
        )
    timings = [batch_sample[field] for field in timing_fields]
    if timings != sorted(timings):
        _fail(
            f"{path}.batch_sample",
            "timings must satisfy update_core_ms <= session_step_ms <= artifact_step_ms",
        )


def _validate_correctness(correctness: dict[str, object], path: str) -> None:
    if correctness["pass"] is not True:
        return
    if correctness["mismatch_count"] != 0:
        _fail(f"{path}.mismatch_count", "passing correctness requires zero mismatches")

    bounded_fields = ("max_abs_error", "max_rel_error", "wns_delta", "tns_delta")
    required_fields = (*bounded_fields, "tolerance")
    if any(correctness[field] is None for field in required_fields):
        _fail(path, "passing correctness requires all error metrics and tolerance")
    tolerance = correctness["tolerance"]
    for field in bounded_fields:
        if abs(correctness[field]) > tolerance:
            _fail(f"{path}.{field}", "absolute value exceeds correctness tolerance")


def _validate_summary(
    summary: dict[str, object], correctness: dict[str, object], path: str
) -> None:
    n = summary["n"]
    stats = [summary[field] for field in SUMMARY_STAT_FIELDS]
    if n is None:
        if any(value is not None for value in stats):
            _fail(path, "unmeasured summary requires all statistic fields to be null")
    elif any(value is None for value in stats):
        _fail(path, "measured summary requires every statistic field")
    else:
        minimum, median, p95, maximum, _mad, mean, _stddev = stats
        if not minimum <= median <= p95 <= maximum:
            _fail(path, "statistics must satisfy min <= median <= p95 <= max")
        if not minimum <= mean <= maximum:
            _fail(f"{path}.mean", "mean must lie within [min, max]")

    has_speedup = any(summary[field] is not None for field in SPEEDUP_FIELDS)
    if has_speedup and (correctness["pass"] is not True or n is None):
        _fail(path, "speedup requires passing correctness and a measured summary")


def _validate_semantics(document: dict[str, object]) -> None:
    samples = document["samples"]
    if not isinstance(samples, list):
        raise ValidationError("$.samples must be an array")
    for index, raw_sample in enumerate(samples):
        if not isinstance(raw_sample, dict):
            raise ValidationError(f"$.samples[{index}] must be an object")
        sample = raw_sample
        mode = sample["mode"]
        case = sample["case"]
        correctness = sample["correctness"]
        summary = sample["summary"]
        batch_sample = sample["batch_sample"]
        if (
            not isinstance(mode, dict)
            or not isinstance(case, dict)
            or not isinstance(correctness, dict)
            or not isinstance(summary, dict)
            or batch_sample is not None
            and not isinstance(batch_sample, dict)
        ):
            raise ValidationError(f"$.samples[{index}] semantic groups must be objects")
        path = f"$.samples[{index}]"
        kind = mode["execution_kind"]
        if kind != "fallback_full" and mode["requested_backend"] != mode["executed_backend"]:
            _fail(f"{path}.mode", "backend mismatch requires execution_kind=fallback_full")
        if kind == "fallback_full" and not mode["fallback_reason"]:
            _fail(f"{path}.mode.fallback_reason", "fallback requires a stable non-empty reason")
        if kind == "incremental_update":
            _validate_incremental_case(case, f"{path}.case")
        _validate_batch_sample(sample, kind, path)
        _validate_correctness(correctness, f"{path}.correctness")
        if kind == "baseline_only":
            for field in (
                "batch_size",
                "changed_nodes",
                "changed_arcs",
                "buffers_inserted",
            ):
                if case[field] != 0:
                    _fail(f"{path}.case.{field}", "baseline_only requires zero mutation work")
            if case["affected_fraction"] != 0 or case["trace_hash"] is not None:
                _fail(f"{path}.case", "baseline_only requires zero affected fraction and no trace")
        _validate_summary(summary, correctness, f"{path}.summary")
        if kind in {"baseline_only", "fallback_full"}:
            for field in SPEEDUP_FIELDS:
                if summary[field] is not None:
                    _fail(
                        f"{path}.summary.{field}",
                        "baseline/fallback samples cannot report speedup",
                    )


def validate_document(document: object, schema: object) -> None:
    if not isinstance(schema, dict):
        raise ValidationError("root schema must be an object")
    _validate(document, schema, schema)
    if not isinstance(document, dict):
        raise ValidationError("root document must be an object")
    _validate_semantics(document)


def _valid_document() -> dict[str, object]:
    metadata = {
        "parent_sha": "1" * 40,
        "ecc_tools_sha": "2" * 40,
        "dirty": False,
        "build_type": "Release",
    }
    metadata.update(
        {"compiler": "GCC 13.3.0", "flags": ["-O3"], "cpu_model": "test cpu", "omp_threads": 1}
    )
    metadata.update(dict.fromkeys(("cuda_runtime", "driver", "gpu_name", "compute_capability")))
    metadata.update(
        {"seed": 0, "timestamp": "2026-08-19T00:00:00Z", "dataset_manifest_hash": "3" * 64}
    )
    return {
        "schema_version": "1.0.0",
        "run_metadata": metadata,
        "samples": [_valid_baseline_sample()],
    }


def _measured_batch_sample(schema: dict[str, object]) -> dict[str, object]:
    batch_schema = _resolve_ref(schema, "#/$defs/batchSample")
    fields = batch_schema.get("properties")
    if not isinstance(fields, dict):
        raise ValidationError("batchSample schema properties must be an object")
    sample = dict.fromkeys(fields)
    sample.update({"cold_or_warm": "cold", "warmup": False, "repetition": 0})
    sample.update({"update_core_ms": 1.0, "session_step_ms": 2.0, "artifact_step_ms": 3.0})
    return sample


def _valid_baseline_sample() -> dict[str, object]:
    case = dict.fromkeys(("batch_size", "changed_nodes", "changed_arcs", "affected_fraction"), 0)
    case.update({"vertices": 2, "arcs": 1, "levels": 2, "buffers_inserted": 0})
    case.update({"design": "stage0_smoke", "corner": "default", "mutation_kind": "baseline"})
    case.update({"trace_hash": None, "baseline_graph_hash": "4" * 64})
    case.update({"semantic_workload_kind": "baseline", "comparability_status": "non_comparable"})
    mode = {
        "requested_backend": "cpu",
        "requested_update_strategy": "incremental",
        "fallback_policy": "error",
    }
    mode.update({"executed_backend": "cpu", "executed_update_strategy": "full"})
    mode.update({"execution_kind": "baseline_only", "propagation_scope": "full_graph"})
    mode.update({"incremental_batch_executed": False, "fallback_to_full": False})
    mode["fallback_reason"] = "not_applicable_no_mutation"
    correctness = {"pass": True, "mismatch_count": 0, "tolerance": 0}
    correctness.update(
        dict.fromkeys(("max_abs_error", "max_rel_error", "wns_delta", "tns_delta"), 0)
    )
    return {
        "case": case,
        "mode": mode,
        "batch_sample": None,
        "run_aggregate": dict.fromkeys(
            (
                "process_setup_ms",
                "cuda_context_ms",
                "baseline_build_ms",
                "all_batches_ms",
                "close_ms",
                "cold_end_to_end_ms",
            )
        ),
        "correctness": correctness,
        "summary": dict.fromkeys((*SUMMARY_STAT_FIELDS, *SPEEDUP_FIELDS, "n")),
    }


def _valid_incremental_document(schema: dict[str, object]) -> dict[str, object]:
    document = _valid_document()
    sample = document["samples"][0]
    sample["case"].update({"mutation_kind": "buffer_insert", "batch_size": 1})
    sample["case"].update({"changed_nodes": 2, "changed_arcs": 1, "affected_fraction": 0.5})
    sample["case"].update({"buffers_inserted": 1, "trace_hash": "5" * 64})
    sample["case"].update(
        {"semantic_workload_kind": "fixed_trace", "comparability_status": "exact"}
    )
    sample["mode"].update({"executed_update_strategy": "incremental", "fallback_reason": None})
    sample["mode"].update(
        {"execution_kind": "incremental_update", "incremental_batch_executed": True}
    )
    sample["batch_sample"] = _measured_batch_sample(schema)
    sample["correctness"].update({"max_abs_error": 0.01, "max_rel_error": 0.01})
    sample["correctness"].update({"wns_delta": -0.01, "tns_delta": 0.01, "tolerance": 0.01})
    sample["summary"].update({"n": 3, "min": 1.0, "median": 2.0, "p95": 2.5})
    sample["summary"].update({"max": 3.0, "mad": 0.5, "mean": 2.0, "stddev": 0.8})
    sample["summary"]["speedup_vs_cpu_full"] = 1.5
    return document


def _expect_rejected(name: str, action: Callable[[], object]) -> None:
    try:
        action()
    except ValidationError:
        return
    raise AssertionError(f"negative self-test {name!r} was accepted")


def _run_self_test(schema: object) -> None:
    if not isinstance(schema, dict):
        raise ValidationError("root schema must be an object")
    baseline = _valid_document()
    incremental = _valid_incremental_document(schema)
    negative_documents: list[tuple[str, dict[str, object]]] = []

    def add_variant(
        name: str, source: dict[str, object], group: str, changes: dict[str, object]
    ) -> None:
        document = copy.deepcopy(source)
        if group == "root":
            target = document
        elif group == "run_metadata":
            target = document["run_metadata"]
        elif group == "sample":
            target = document["samples"][0]
        else:
            target = document["samples"][0][group]
        target.update(changes)
        negative_documents.append((name, document))

    add_variant("bool-as-integer", baseline, "run_metadata", {"seed": True})
    add_variant("bad-version", baseline, "root", {"schema_version": "2.0.0"})
    add_variant("bad-enum", baseline, "mode", {"requested_backend": "gpu"})
    add_variant("bad-sha256", baseline, "case", {"baseline_graph_hash": "A" * 64})
    add_variant("extra-property", baseline, "sample", {"extra": 1})

    missing = copy.deepcopy(baseline)
    del missing["samples"][0]["correctness"]["pass"]
    negative_documents.append(("missing-property", missing))

    add_variant("contradictory-fallback", baseline, "mode", {"fallback_to_full": True})
    add_variant("unrecorded-backend-mismatch", baseline, "mode", {"executed_backend": "cuda"})
    add_variant("mutated-baseline", baseline, "case", {"batch_size": 1})
    add_variant(
        "baseline-with-batch",
        baseline,
        "sample",
        {"batch_sample": _measured_batch_sample(schema)},
    )

    for name, changes in (
        ("incremental-empty-batch", {"batch_size": 0}),
        ("incremental-missing-trace", {"trace_hash": None}),
        ("incremental-no-graph-change", {"changed_nodes": 0, "changed_arcs": 0}),
        ("incremental-baseline-mutation", {"mutation_kind": "baseline"}),
        ("incremental-baseline-workload", {"semantic_workload_kind": "baseline"}),
    ):
        document = copy.deepcopy(incremental)
        document["samples"][0]["case"].update(changes)
        negative_documents.append((name, document))

    update_without_batch = copy.deepcopy(incremental)
    update_without_batch["samples"][0]["batch_sample"] = None
    negative_documents.append(("update-without-batch", update_without_batch))

    for field in ("update_core_ms", "session_step_ms", "artifact_step_ms"):
        document = copy.deepcopy(incremental)
        document["samples"][0]["batch_sample"][field] = None
        negative_documents.append((f"measured-batch-null-{field}", document))

    for name, changes in (
        ("core-after-session", {"update_core_ms": 2.1}),
        ("session-after-artifact", {"session_step_ms": 3.1}),
    ):
        document = copy.deepcopy(incremental)
        document["samples"][0]["batch_sample"].update(changes)
        negative_documents.append((name, document))

    mismatch = copy.deepcopy(incremental)
    mismatch["samples"][0]["correctness"]["mismatch_count"] = 1
    negative_documents.append(("passing-with-mismatch", mismatch))

    for field in ("max_abs_error", "max_rel_error", "wns_delta", "tns_delta", "tolerance"):
        document = copy.deepcopy(incremental)
        document["samples"][0]["correctness"][field] = None
        negative_documents.append((f"passing-null-{field}", document))

    for field, value in (
        ("max_abs_error", 0.02),
        ("max_rel_error", 0.02),
        ("wns_delta", -0.02),
        ("tns_delta", 0.02),
    ):
        document = copy.deepcopy(incremental)
        document["samples"][0]["correctness"][field] = value
        negative_documents.append((f"passing-over-tolerance-{field}", document))

    failed_speedup = copy.deepcopy(incremental)
    failed_speedup["samples"][0]["correctness"]["pass"] = False
    negative_documents.append(("failed-correctness-speedup", failed_speedup))

    zero_summary = copy.deepcopy(incremental)
    zero_summary["samples"][0]["summary"]["n"] = 0
    negative_documents.append(("zero-sized-summary", zero_summary))

    for field in SUMMARY_STAT_FIELDS:
        document = copy.deepcopy(incremental)
        document["samples"][0]["summary"][field] = None
        negative_documents.append((f"measured-summary-null-{field}", document))

    for name, changes in (
        ("min-after-median", {"min": 2.1}),
        ("median-after-p95", {"median": 2.6}),
        ("p95-after-max", {"p95": 3.1}),
        ("mean-before-min", {"mean": 0.9}),
        ("mean-after-max", {"mean": 3.1}),
    ):
        document = copy.deepcopy(incremental)
        document["samples"][0]["summary"].update(changes)
        negative_documents.append((name, document))

    summaryless_speedup = copy.deepcopy(incremental)
    summaryless_speedup["samples"][0]["summary"]["n"] = None
    for field in SUMMARY_STAT_FIELDS:
        summaryless_speedup["samples"][0]["summary"][field] = None
    negative_documents.append(("speedup-without-summary", summaryless_speedup))

    stray_stat = copy.deepcopy(baseline)
    stray_stat["samples"][0]["summary"]["min"] = 1.0
    negative_documents.append(("unmeasured-summary-with-stat", stray_stat))

    for name, document in negative_documents:
        _expect_rejected(name, lambda document=document: validate_document(document, schema))

    raw_negative_checks = (
        ("NaN", lambda: _loads_json('{"value": NaN}')),
        ("Infinity", lambda: _loads_json('{"value": Infinity}')),
        ("duplicate-key", lambda: _loads_json('{"value": 1, "value": 2}')),
    )
    for name, action in raw_negative_checks:
        _expect_rejected(name, action)

    cuda_baseline = copy.deepcopy(baseline)
    cuda_baseline["samples"][0]["mode"]["requested_backend"] = "cuda"
    cuda_baseline["samples"][0]["mode"]["executed_backend"] = "cuda"
    raw_incremental = copy.deepcopy(incremental)
    raw_incremental["samples"][0]["summary"].update(
        {"n": None, **dict.fromkeys(SUMMARY_STAT_FIELDS), "speedup_vs_cpu_full": None}
    )
    positive_documents = (baseline, cuda_baseline, incremental, raw_incremental)
    for document in positive_documents:
        validate_document(document, schema)

    checks = len(positive_documents) + len(negative_documents) + len(raw_negative_checks)
    print(f"self-test: {checks} checks passed")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("documents", nargs="*", type=Path, help="sample documents to validate")
    parser.add_argument(
        "--schema",
        type=Path,
        default=SCHEMA_PATH,
        help=f"schema path (default: {SCHEMA_PATH.name})",
    )
    parser.add_argument("--self-test", action="store_true", help="run contract checks")
    args = parser.parse_args(argv)

    if not args.self_test and not args.documents:
        parser.error("provide at least one document or use --self-test")

    try:
        schema = _load_json(args.schema)
        if args.self_test:
            _run_self_test(schema)
        for document_path in args.documents:
            validate_document(_load_json(document_path), schema)
            print(f"valid: {document_path}")
    except (ValidationError, AssertionError) as error:
        print(f"validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
