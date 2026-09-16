#!/usr/bin/env python3
"""Process-isolated controller for Stage 4 fixed semantic buffer traces."""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

from buffer_trace_functional_check import (
    FunctionalEvidenceError,
    self_test_derived_freshness,
    self_test_terminal_empty_round,
    validate_cross_prefix,
    validate_derived_freshness,
    validate_prefix,
    workload_kind,
)

WIRE_SCHEMA = "buffer-replay-probe/1.0.0"
SUMMARY_SCHEMA = "buffer-replay-controller/1.0.0"


class ControllerError(RuntimeError):
    pass


def read_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ControllerError(f"cannot read strict JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise ControllerError(f"{path} must contain one JSON object")
    return value


def write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def require_exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    actual = set(value)
    if actual != expected:
        raise ControllerError(
            f"{label} keys differ: missing={sorted(expected - actual)} "
            f"unknown={sorted(actual - expected)}"
        )


def first_difference(left: Any, right: Any, path: str = "$") -> str | None:
    if type(left) is not type(right):
        return f"{path}: type {type(left).__name__} != {type(right).__name__}"
    if isinstance(left, dict):
        if set(left) != set(right):
            return f"{path}: object keys differ"
        for key in sorted(left):
            difference = first_difference(left[key], right[key], f"{path}.{key}")
            if difference:
                return difference
        return None
    if isinstance(left, list):
        if len(left) != len(right):
            return f"{path}: list length {len(left)} != {len(right)}"
        for index, (left_item, right_item) in enumerate(zip(left, right, strict=True)):
            difference = first_difference(left_item, right_item, f"{path}[{index}]")
            if difference:
                return difference
        return None
    if left != right:
        return f"{path}: {left!r} != {right!r}"
    return None


def trace_batch_count(trace: dict[str, Any]) -> int:
    require_exact_keys(trace, {"replay", "generation_only"}, "trace")
    replay = trace["replay"]
    if not isinstance(replay, dict):
        raise ControllerError("trace.replay must be an object")
    workload = replay.get("workload")
    if not isinstance(workload, dict):
        raise ControllerError("trace.replay.workload must be an object")
    batches = workload.get("batches")
    if not isinstance(batches, list) or not batches:
        raise ControllerError("trace replay requires at least one batch")
    return len(batches)


def validate_source_isolation(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ControllerError(f"{label} must be an object")
    require_exact_keys(
        value,
        {
            "idb_before",
            "idb_after",
            "artifacts_before",
            "artifacts_after",
            "source_unchanged",
        },
        label,
    )
    if value["source_unchanged"] is not True:
        raise ControllerError(f"{label}.source_unchanged is not true")
    for before_name, after_name in (
        ("idb_before", "idb_after"),
        ("artifacts_before", "artifacts_after"),
    ):
        difference = first_difference(value[before_name], value[after_name])
        if difference:
            raise ControllerError(
                f"{label} changed between {before_name}/{after_name}: {difference}"
            )
    for field in ("artifacts_before", "artifacts_after"):
        artifacts = value[field]
        if not isinstance(artifacts, list) or not all(isinstance(item, dict) for item in artifacts):
            raise ControllerError(f"{label}.{field} must be an artifact array")
        roles = [item.get("role") for item in artifacts]
        cell_count, liberty_count = roles.count("cell_lef"), roles.count("liberty")
        expected_roles = [
            "tech_lef", *(["cell_lef"] * cell_count),
            *(["liberty"] * liberty_count), "verilog", "sdc",
        ]
        if cell_count == 0 or liberty_count == 0 or roles != expected_roles:
            raise ControllerError(f"{label}.{field} artifact roles differ")
        paths = [item.get("canonical_path") for item in artifacts]
        if any(not isinstance(path, str) or not path for path in paths):
            raise ControllerError(f"{label}.{field} has an invalid canonical path")
        if len(set(paths)) != len(paths):
            raise ControllerError(f"{label}.{field} aliases source paths")
        for item in artifacts:
            require_exact_keys(
                item,
                {"role", "logical_name", "canonical_path", "size", "bytes_hex"},
                f"{label}.{field} artifact",
            )
            if not isinstance(item.get("bytes_hex"), str):
                raise ControllerError(f"{label}.{field} lacks exact source bytes")
            if not isinstance(item.get("size"), int) or item["size"] < 0:
                raise ControllerError(f"{label}.{field} has an invalid byte size")
            if item.get("size") * 2 != len(item["bytes_hex"]):
                raise ControllerError(f"{label}.{field} byte size differs")
    physical = value["idb_before"]
    require_exact_keys(
        physical,
        {"primary_idb", "optional_netlist_view", "spef_reader_present"},
        f"{label}.idb_before",
    )
    for field in ("primary_idb", "optional_netlist_view"):
        projection = physical[field]
        if not isinstance(projection, dict) or not isinstance(projection.get("present"), bool):
            raise ControllerError(f"{label}.idb_before.{field} lacks presence")
    if physical["primary_idb"]["present"] is not True:
        raise ControllerError(f"{label} lacks the authoritative primary IDB")
    if physical["spef_reader_present"] is not False:
        raise ControllerError(f"{label} unexpectedly loaded physical parasitics")
    return value


def cross_process_source_isolation(value: dict[str, Any]) -> dict[str, Any]:
    """Remove only the per-process path while preserving exact source bytes."""
    normalized = json.loads(json.dumps(value))
    for field in ("artifacts_before", "artifacts_after"):
        artifacts = normalized[field]
        if not isinstance(artifacts, list):
            raise ControllerError(f"source isolation {field} must be an array")
        for index, artifact in enumerate(artifacts):
            if not isinstance(artifact, dict) or "canonical_path" not in artifact:
                raise ControllerError(
                    f"source isolation {field}[{index}] lacks canonical_path"
                )
            del artifact["canonical_path"]
    return normalized


SOURCE_ROLES = ("tech_lef", "cell_lef", "liberty", "verilog", "sdc")


def validate_source_contents(
    trace: dict[str, Any], trace_path: pathlib.Path,
    isolation: dict[str, Any], label: str,
) -> pathlib.Path | None:
    observed = isolation["artifacts_before"]
    workload = trace["replay"]["workload"]
    paths = [pathlib.Path(item["canonical_path"]) for item in observed]
    if any(not path.is_absolute() for path in paths):
        raise ControllerError(f"{label} source path is not absolute")
    if len(set(paths)) != len(paths):
        raise ControllerError(f"{label} source paths alias")
    kind = workload_kind(trace)
    if kind == "synthetic":
        parents = {path.parent for path in paths}
        if len(parents) != 1:
            raise ControllerError(
                f"{label} synthetic sources do not share one owned root")
        parent = next(iter(parents))
        if not parent.name.startswith("ista-buffer-trace-"):
            raise ControllerError(
                f"{label} synthetic source root is not adapter-owned")
        recipe = workload["source_recipe"]
        for index, role in enumerate(SOURCE_ROLES):
            source = recipe[role]
            source_text = source["source_text"]
            logical_name = source["logical_name"]
            evidence_name = f"0-{logical_name}" if role in ("cell_lef", "liberty") else logical_name
            if not isinstance(source_text, str) or not isinstance(logical_name, str):
                raise ControllerError(f"{label} {role} recipe is malformed")
            exact = source_text.encode("utf-8")
            item = observed[index]
            if (item["role"] != role
                    or item["logical_name"] != evidence_name
                    or paths[index].name != logical_name
                    or item["size"] != len(exact)
                    or item["bytes_hex"] != exact.hex()):
                raise ControllerError(
                    f"{label} {role} differs from the synthetic recipe")
        return parent
    if kind != "real_design":
        raise ControllerError(f"{label} has unknown workload kind {kind}")
    artifacts = workload["artifacts"]
    source_root = trace_path.parent.parent
    sources = [("tech_lef", artifacts["tech_lef"], "")]
    sources.extend(("cell_lef", source, f"{index}-")
                   for index, source in enumerate(artifacts["cell_lefs"]))
    sources.extend(("liberty", source, f"{index}-")
                   for index, source in enumerate(artifacts["liberties"]))
    sources.extend((role, artifacts[role], "") for role in ("verilog", "sdc"))
    if len(observed) != len(sources):
        raise ControllerError(f"{label} source artifact count differs")
    for index, (role, source, prefix) in enumerate(sources):
        raw_path = source["path"]
        if not isinstance(raw_path, str) or not raw_path:
            raise ControllerError(f"{label} {role} artifact path is malformed")
        canonical = (source_root / raw_path).resolve(strict=True)
        exact = canonical.read_bytes()
        logical_name = prefix + canonical.name
        item = observed[index]
        if (item["role"] != role
                or item["logical_name"] != logical_name
                or pathlib.Path(item["canonical_path"]) != canonical
                or item["size"] != len(exact)
                or item["bytes_hex"] != exact.hex()):
            raise ControllerError(
                f"{label} {role} differs from its real source artifact")
    return None


def validate_probe_result(
    value: dict[str, Any], prefix: int, repetition: int, kind: str, label: str
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any], dict[str, Any]]:
    require_exact_keys(
        value,
        {
            "schema_version",
            "functional",
            "source_isolation",
            "derived_timing_freshness",
            "run",
        },
        label,
    )
    if value["schema_version"] != WIRE_SCHEMA:
        raise ControllerError(f"{label} has an unknown wire schema")
    functional = value["functional"]
    envelope = value["run"]
    if not isinstance(functional, dict) or not isinstance(envelope, dict):
        raise ControllerError(f"{label} functional/run values must be objects")
    require_exact_keys(
        envelope,
        {
            "child_pid",
            "loader_invocation_count",
            "guarded_work_invocation_count",
            "one_shot_state",
            "prefix_batches",
            "repetition",
        },
        f"{label}.run",
    )
    expected = {
        "loader_invocation_count": 1,
        "guarded_work_invocation_count": 1,
        "one_shot_state": "finished",
        "prefix_batches": prefix,
        "repetition": repetition,
    }
    for field, expected_value in expected.items():
        if envelope[field] != expected_value:
            raise ControllerError(
                f"{label}.run.{field}={envelope[field]!r}, expected {expected_value!r}"
            )
    if not isinstance(envelope["child_pid"], int) or envelope["child_pid"] <= 0:
        raise ControllerError(f"{label}.run.child_pid must be a positive integer")
    if functional.get("prefix_batches") != prefix:
        raise ControllerError(f"{label}.functional has the wrong prefix")
    isolation = validate_source_isolation(value["source_isolation"], f"{label}.source_isolation")
    freshness = value["derived_timing_freshness"]
    validate_derived_freshness(freshness, kind,
                               f"{label}.derived_timing_freshness")
    return functional, isolation, freshness, envelope


def run_child(
    probe: pathlib.Path,
    trace: pathlib.Path,
    prefix: int,
    repetition: int,
    output: pathlib.Path,
    timeout: int,
) -> dict[str, Any]:
    command = [
        str(probe),
        "replay",
        "--trace",
        str(trace),
        "--prefix",
        str(prefix),
        "--repetition",
        str(repetition),
        "--output",
        str(output),
    ]
    completed = subprocess.run(
        command,
        cwd=trace.parent.parent,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if completed.returncode != 0:
        raise ControllerError(
            f"probe failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    if completed.stdout or completed.stderr:
        (output.parent / "stdout.txt").write_text(completed.stdout, encoding="utf-8")
        (output.parent / "stderr.txt").write_text(completed.stderr, encoding="utf-8")
    return read_json(output)


def run_generation(
    probe: pathlib.Path,
    template: pathlib.Path,
    case_id: str,
    seed: int,
    trace_output: pathlib.Path,
    receipt_output: pathlib.Path,
    cwd: pathlib.Path,
    timeout: int,
) -> tuple[dict[str, Any], dict[str, Any]]:
    command = [
        str(probe),
        "generate",
        "--template",
        str(template),
        "--case-id",
        case_id,
        "--seed",
        str(seed),
        "--trace-output",
        str(trace_output),
        "--output",
        str(receipt_output),
    ]
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if completed.returncode != 0:
        raise ControllerError(
            f"generation probe failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    if completed.stdout or completed.stderr:
        (receipt_output.parent / "generation-stdout.txt").write_text(
            completed.stdout, encoding="utf-8")
        (receipt_output.parent / "generation-stderr.txt").write_text(
            completed.stderr, encoding="utf-8")
    return read_json(trace_output), read_json(receipt_output)


def replay(args: argparse.Namespace) -> None:
    probe = args.probe.resolve(strict=True)
    trace = args.trace.resolve(strict=True)
    if args.repeats < 3:
        raise ControllerError("fresh-process evidence requires at least three repetitions")
    trace_value = read_json(trace)
    batch_count = trace_batch_count(trace_value)
    kind = workload_kind(trace_value)
    run_parent = args.run_root.resolve()
    run_parent.mkdir(parents=True, exist_ok=True)
    run_root = pathlib.Path(tempfile.mkdtemp(prefix="run-", dir=run_parent))
    synthetic_source_parents: set[pathlib.Path] = set()

    generation_summary: dict[str, Any] | None = None
    generation_functional: dict[str, Any] | None = None
    generation_pid: int | None = None
    generation_options = (args.template, args.case_id, args.seed)
    if any(value is not None for value in generation_options):
        if not all(value is not None for value in generation_options):
            raise ControllerError(
                "--template, --case-id and --seed must be provided together")
        generated_trace, generation_result = run_generation(
            probe,
            args.template.resolve(strict=True),
            args.case_id,
            args.seed,
            run_root / "generation" / "generated-trace.json",
            run_root / "generation" / "receipt.json",
            trace.parent.parent,
            args.timeout_seconds,
        )
        for field in ("replay", "generation_only"):
            difference = first_difference(trace_value[field], generated_trace[field])
            if difference:
                raise ControllerError(
                    f"fresh generation differs from frozen {field}: {difference}")
        generation_functional, generation_isolation, generation_freshness, envelope = (
            validate_probe_result(
                generation_result, batch_count, 0, kind, "fresh generation")
        )
        parent = validate_source_contents(
            trace_value, trace, generation_isolation, "fresh generation")
        if parent is not None:
            if parent in synthetic_source_parents:
                raise ControllerError("synthetic source root was reused")
            synthetic_source_parents.add(parent)
        validate_prefix(trace_value, generation_functional, batch_count)
        generation_pid = envelope["child_pid"]
        generation_summary = {
            "functional": generation_functional,
            "source_isolation": generation_isolation,
            "derived_timing_freshness": generation_freshness,
            "run": envelope,
            "frozen_replay_and_generation_fields_exact": True,
        }

    prefix_summaries: list[dict[str, Any]] = []
    prefix_functionals: list[dict[str, Any]] = []
    all_pids: set[int] = set()
    if generation_pid is not None:
        all_pids.add(generation_pid)
    for prefix in range(0, batch_count + 1):
        functionals: list[dict[str, Any]] = []
        isolations: list[dict[str, Any]] = []
        freshness_receipts: list[dict[str, Any]] = []
        envelopes: list[dict[str, Any]] = []
        for repetition in range(args.repeats):
            output = run_root / f"prefix-{prefix}" / f"run-{repetition}" / "receipt.json"
            value = run_child(
                probe, trace, prefix, repetition, output, args.timeout_seconds
            )
            functional, isolation, freshness, envelope = validate_probe_result(
                value, prefix, repetition, kind, str(output)
            )
            parent = validate_source_contents(
                trace_value, trace, isolation, str(output))
            if parent is not None:
                if parent in synthetic_source_parents:
                    raise ControllerError("synthetic source root was reused")
                synthetic_source_parents.add(parent)
            validate_prefix(trace_value, functional, prefix)
            if envelope["child_pid"] in all_pids:
                raise ControllerError("fresh evidence reused a child PID")
            all_pids.add(envelope["child_pid"])
            functionals.append(functional)
            isolations.append(isolation)
            freshness_receipts.append(freshness)
            envelopes.append(envelope)
        for index in range(1, len(functionals)):
            difference = first_difference(functionals[0], functionals[index])
            if difference:
                raise ControllerError(
                    f"prefix {prefix} functional receipts differ: {difference}"
                )
            difference = first_difference(
                cross_process_source_isolation(isolations[0]),
                cross_process_source_isolation(isolations[index]),
            )
            if difference:
                raise ControllerError(
                    f"prefix {prefix} source receipts differ: {difference}"
                )
            difference = first_difference(freshness_receipts[0],
                                          freshness_receipts[index])
            if difference:
                raise ControllerError(
                    f"prefix {prefix} freshness receipts differ: {difference}"
                )
        summary = {
            "prefix_batches": prefix,
            "functional_equality": True,
            "source_equality": True,
            "functional": functionals[0],
            "source_isolation": isolations[0],
            "derived_timing_freshness": freshness_receipts[0],
            "runs": envelopes,
        }
        write_json(run_root / f"prefix-{prefix}" / "functional-equality.json", summary)
        prefix_summaries.append(summary)
        prefix_functionals.append(functionals[0])

    functional_evidence = validate_cross_prefix(trace_value, prefix_functionals)
    functional_evidence["source_recipe_or_artifact_bytes_exact"] = True
    functional_evidence["synthetic_source_roots_globally_unique"] = (
        kind == "synthetic")
    if generation_functional is not None:
        difference = first_difference(generation_functional, prefix_functionals[-1])
        if difference:
            raise ControllerError(
                f"generation and fresh full-prefix receipts differ: {difference}")
        functional_evidence["generation_vs_fresh_full_prefix_exact"] = True
    else:
        functional_evidence["generation_vs_fresh_full_prefix_exact"] = False
    write_json(
        run_root / "controller-summary.json",
        {
            "schema_version": SUMMARY_SCHEMA,
            "trace": str(trace),
            "batch_count": batch_count,
            "repeats_per_prefix": args.repeats,
            "all_child_pids_distinct": True,
            "prefixes": prefix_summaries,
            "functional_evidence": functional_evidence,
            "generation": generation_summary,
        },
    )


def self_test() -> None:
    baseline = {"a": [1, {"b": "same"}], "c": True}
    if first_difference(baseline, json.loads(json.dumps(baseline))) is not None:
        raise ControllerError("field equality rejected equal values")
    difference = first_difference(baseline, {"a": [1, {"b": "changed"}], "c": True})
    if difference != "$.a[1].b: 'same' != 'changed'":
        raise ControllerError("field equality did not locate the changed leaf")
    roles = ["tech_lef", "cell_lef", "liberty", "verilog", "sdc"]
    suffixes = ["tech.lef", "cell.lef", "stage4.lib", "design.v", "design.sdc"]
    source_texts = ["T", "C", "L", "V", "S"]
    artifacts = [
        {
            "role": role,
            "logical_name": f"0-{suffix}" if role in ("cell_lef", "liberty") else suffix,
            "canonical_path": f"/tmp/ista-buffer-trace-run-a/{suffix}",
            "size": 1,
            "bytes_hex": source_texts[index].encode("utf-8").hex(),
        }
        for index, (role, suffix) in enumerate(zip(roles, suffixes, strict=True))
    ]
    physical = {
        "primary_idb": {"present": True, "instances": ["u0"]},
        "optional_netlist_view": {"present": False, "instances": []},
        "spef_reader_present": False,
    }
    source = {
        "idb_before": physical,
        "idb_after": json.loads(json.dumps(physical)),
        "artifacts_before": artifacts,
        "artifacts_after": json.loads(json.dumps(artifacts)),
        "source_unchanged": True,
    }
    validate_source_isolation(source, "self-test")
    synthetic_trace = {"replay": {
        "header": {"semantic_workload_kind": "synthetic"},
        "workload": {
            "source_recipe": {
                role: {"logical_name": suffix, "source_text": source_text}
                for role, suffix, source_text in zip(
                    roles, suffixes, source_texts, strict=True)
            },
        },
    }}
    validate_source_contents(
        synthetic_trace, pathlib.Path("/tmp/unused/trace.json"), source,
        "self-test")
    broken = dict(source)
    broken["idb_after"] = {"instances": ["u1"]}
    try:
        validate_source_isolation(broken, "self-test")
    except ControllerError:
        pass
    else:
        raise ControllerError("source mutation was accepted")
    other_root = json.loads(json.dumps(source))
    for field in ("artifacts_before", "artifacts_after"):
        for artifact in other_root[field]:
            artifact["canonical_path"] = artifact["canonical_path"].replace(
                "/ista-buffer-trace-run-a/",
                "/ista-buffer-trace-run-b/",
            )
    validate_source_contents(synthetic_trace, pathlib.Path("/tmp/unused/trace.json"),
                             other_root, "self-test other root")
    if first_difference(source, other_root) is None:
        raise ControllerError("run-local path difference was not represented")
    if first_difference(
        cross_process_source_isolation(source),
        cross_process_source_isolation(other_root),
    ) is not None:
        raise ControllerError("different temporary roots changed source comparability")
    self_test_terminal_empty_round()
    self_test_derived_freshness()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--probe", type=pathlib.Path)
    parser.add_argument("--trace", type=pathlib.Path)
    parser.add_argument("--run-root", type=pathlib.Path)
    parser.add_argument("--template", type=pathlib.Path)
    parser.add_argument("--case-id")
    parser.add_argument("--seed", type=int)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout-seconds", type=int, default=300)
    return parser.parse_args()


def main() -> int:
    try:
        args = parse_args()
        if args.self_test:
            self_test()
        else:
            if not args.probe or not args.trace or not args.run_root:
                raise ControllerError("--probe, --trace and --run-root are required")
            replay(args)
        return 0
    except (ControllerError, FunctionalEvidenceError, OSError,
            subprocess.SubprocessError) as error:
        print(f"buffer trace controller: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
