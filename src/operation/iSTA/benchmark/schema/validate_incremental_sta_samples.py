#!/usr/bin/env python3
"""Validate incremental STA sample documents without third-party packages."""

import argparse
import copy
import sys
from collections.abc import Callable
from pathlib import Path

from consumer_trace_samples import _consumer_statistics_self_test, validate_consumer_samples
from json_schema_contract import (
    ValidationError,
    validate_schema,
)
from json_schema_contract import (
    fail as _fail,
)
from json_schema_contract import (
    load_json as _load_json,
)
from json_schema_contract import (
    loads_json as _loads_json,
)
from json_schema_contract import (
    resolve_ref as _resolve_ref,
)

SCHEMA_PATH = Path(__file__).with_name("incremental_sta_samples_v1.schema.json")


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
    validate_schema(document, schema)
    if not isinstance(document, dict):
        raise ValidationError("root document must be an object")
    if document.get("schema_version") in ("2.0.0", "3.0.0"):
        validate_consumer_samples(document)
    else:
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
        default=None,
        help="override the schema selected by document version",
    )
    parser.add_argument("--self-test", action="store_true", help="run contract checks")
    args = parser.parse_args(argv)

    if not args.self_test and not args.documents:
        parser.error("provide at least one document or use --self-test")

    try:
        schema = _load_json(args.schema or SCHEMA_PATH)
        if args.self_test:
            _run_self_test(schema)
            _consumer_statistics_self_test()
        for document_path in args.documents:
            document = _load_json(document_path)
            if not isinstance(document, dict):
                _fail("$", "document must be an object")
            selected = args.schema or SCHEMA_PATH.with_name(
                f"incremental_sta_samples_v{document['schema_version'][0]}.schema.json"
                if document.get("schema_version") in ("2.0.0", "3.0.0") else SCHEMA_PATH.name)
            validate_document(document, _load_json(selected))
            print(f"valid: {document_path}")
    except (ValidationError, AssertionError) as error:
        print(f"validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
