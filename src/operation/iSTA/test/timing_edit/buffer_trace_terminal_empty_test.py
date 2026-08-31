#!/usr/bin/env python3
"""Prove a production-generated nonempty batch followed by an empty round."""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

from buffer_trace_controller import (
    ControllerError,
    first_difference,
    run_child,
    run_generation,
    trace_batch_count,
    validate_probe_result,
    validate_source_contents,
    write_json,
)
from buffer_trace_functional_check import (
    FunctionalEvidenceError,
    validate_cross_prefix,
    validate_prefix,
)


class TerminalEmptyError(RuntimeError):
    pass


def strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise TerminalEmptyError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    parser.add_argument("--base-template", type=pathlib.Path, required=True)
    parser.add_argument("--verilog", type=pathlib.Path, required=True)
    parser.add_argument("--sdc", type=pathlib.Path, required=True)
    parser.add_argument("--top", required=True)
    parser.add_argument("--run-root", type=pathlib.Path, required=True)
    parser.add_argument("--case-id", default="synthetic_terminal_empty")
    parser.add_argument("--seed", type=int, default=43)
    parser.add_argument("--timeout-seconds", type=int, default=300)
    return parser.parse_args()


def terminal_shape(trace: dict[str, Any]) -> None:
    generation = trace["generation_only"]
    batches = trace["replay"]["workload"]["batches"]
    rounds = generation["rounds"]
    if generation["profile"]["top_k"] != 4:
        raise TerminalEmptyError("terminal fixture did not use top_k four")
    if len(batches) != 1 or len(rounds) != 2:
        raise TerminalEmptyError(
            "terminal fixture must produce one replay batch and two rounds")
    if generation["stop_reason"] != "empty_accepted_batch":
        raise TerminalEmptyError("generation did not stop on an empty batch")
    first, terminal = rounds
    if first["batch_ordinal"] != 1 or terminal["batch_ordinal"] != 2:
        raise TerminalEmptyError("terminal round ordinals drifted")
    if len(first["accepted_tokens"]) != 1:
        raise TerminalEmptyError("round one did not accept exactly one insertion")
    if terminal["accepted_tokens"] != []:
        raise TerminalEmptyError("terminal round unexpectedly accepted an insertion")
    if len(first["evaluations"]) != 5 or len(terminal["evaluations"]) != 6:
        raise TerminalEmptyError(
            "production extractor exposed unexpected nonnumeric candidates")
    first_numeric = [item for item in first["evaluations"]
                     if item.get("numeric") is not None]
    terminal_numeric = [item for item in terminal["evaluations"]
                        if item.get("numeric") is not None]
    if len(first_numeric) != 5 or len(terminal_numeric) != 6:
        raise TerminalEmptyError(
            "production extractor did not expose the expected 5->6 numeric DAG")
    token = first["accepted_tokens"][0]
    first_positive = [item for item in first_numeric
                      if item["numeric"]["gain_ns"] > 0.0]
    if len(first_positive) != 1 or first_positive[0]["decision"] != "accepted":
        raise TerminalEmptyError("round one lacks one exact positive acceptance")
    accepted = first_positive[0]
    if accepted.get("accepted_creation_token") != token:
        raise TerminalEmptyError("round-one accepted token identity drifted")
    if accepted["net"] != {"kind": "existing", "baseline_name": "target_in"}:
        raise TerminalEmptyError("round-one acceptance did not use target_in")
    if accepted["driver"] != {
        "kind": "existing",
        "baseline_key": {
            "kind": "port", "full_name": "target_in",
            "instance_name": "", "pin_name": "",
        },
    }:
        raise TerminalEmptyError("round-one acceptance driver identity drifted")
    if accepted["candidate_pin"] != {
        "kind": "existing",
        "baseline_key": {
            "kind": "instance_pin", "full_name": "target:A",
            "instance_name": "target", "pin_name": "A",
        },
    }:
        raise TerminalEmptyError("round-one acceptance pin identity drifted")
    first_guard = [item for item in first_numeric
                   if item["candidate_pin"].get("baseline_key", {}).get(
                       "full_name") == "guard:A"]
    if (len(first_guard) != 1
            or first_guard[0]["net"] != {
                "kind": "existing", "baseline_name": "target_in"}
            or first_guard[0]["decision"] != "rejected"
            or first_guard[0]["rejection_reason"] != "non_positive_gain"):
        raise TerminalEmptyError("round-one guard did not provide typed criticality")
    terminal_positive = [item for item in terminal_numeric
                         if item["numeric"]["gain_ns"] > 0.0]
    terminal_nonpositive = [item for item in terminal_numeric
                            if item["numeric"]["gain_ns"] <= 0.0]
    if (len(terminal_positive) != 1
            or terminal_positive[0]["decision"] != "non_critical_net"):
        raise TerminalEmptyError(
            "created output-net candidate was not the sole positive noncritical net")
    positive = terminal_positive[0]
    if positive["net"] != {
        "kind": "created", "role": "output_net", "creation_token": token,
    }:
        raise TerminalEmptyError("terminal positive net identity drifted")
    if positive["driver"] != {
        "kind": "created", "role": "output", "creation_token": token,
    }:
        raise TerminalEmptyError("terminal positive driver identity drifted")
    if positive["candidate_pin"] != {
        "kind": "existing",
        "baseline_key": {
            "kind": "instance_pin", "full_name": "target:A",
            "instance_name": "target", "pin_name": "A",
        },
    }:
        raise TerminalEmptyError("terminal positive pin identity drifted")
    if len(terminal_nonpositive) != 5:
        raise TerminalEmptyError(
            "four critical nets did not contain five nonpositive candidates")
    for evaluation in terminal_nonpositive:
        if (evaluation["decision"] != "rejected"
                or evaluation["rejection_reason"] != "non_positive_gain"):
            raise TerminalEmptyError(
                "terminal nonpositive evaluation lacks its typed rejection")
    critical_names = sorted(
        item["net"].get("baseline_name", "") for item in terminal_nonpositive)
    if critical_names != [
        "dummy_in0", "dummy_in1", "dummy_in2", "target_in", "target_in",
    ]:
        raise TerminalEmptyError("terminal critical-net membership drifted")
    created_input = {
        "kind": "created", "role": "input", "creation_token": token,
    }
    input_evaluations = [
        item for item in terminal_nonpositive
        if item["candidate_pin"] == created_input
    ]
    if (len(input_evaluations) != 1
            or input_evaluations[0]["net"] != {
                "kind": "existing", "baseline_name": "target_in"}
            or input_evaluations[0]["driver"] != accepted["driver"]):
        raise TerminalEmptyError("terminal created-input identity drifted")
    positive_slack = positive["numeric"]["slack_ns"]
    critical_nets = {"dummy_in0", "dummy_in1", "dummy_in2", "target_in"}
    for net_name in critical_nets:
        critical_slack = min(
            item["numeric"]["slack_ns"] for item in terminal_nonpositive
            if item["net"]["baseline_name"] == net_name)
        if positive_slack - critical_slack <= 1.0:
            raise TerminalEmptyError(
                f"terminal critical net lacks structural slack margin: {net_name}")


def main() -> int:
    args = parse_args()
    try:
        probe = args.probe.resolve(strict=True)
        base_template = args.base_template.resolve(strict=True)
        verilog = args.verilog.resolve(strict=True)
        sdc = args.sdc.resolve(strict=True)
        template = json.loads(
            base_template.read_text(encoding="utf-8"),
            object_pairs_hook=strict_object,
        )
        workload = template.get("workload")
        if not isinstance(workload, dict) or workload.get("kind") != "synthetic":
            raise TerminalEmptyError("base template is not synthetic")
        if workload.get("batches") != []:
            raise TerminalEmptyError("base template already contains replay batches")
        recipe = workload.get("source_recipe")
        if not isinstance(recipe, dict):
            raise TerminalEmptyError("base template lacks its source recipe")
        recipe["verilog"] = {
            "logical_name": verilog.name,
            "source_text": verilog.read_text(encoding="utf-8"),
        }
        recipe["sdc"] = {
            "logical_name": sdc.name,
            "source_text": sdc.read_text(encoding="utf-8"),
        }
        recipe["top"] = args.top

        run_parent = args.run_root.resolve()
        run_parent.mkdir(parents=True, exist_ok=True)
        run_root = pathlib.Path(tempfile.mkdtemp(prefix="run-", dir=run_parent))
        generated_template = run_root / "terminal-empty.template.json"
        trace_path = run_root / "terminal-empty.trace.json"
        generation_path = run_root / "generation.json"
        write_json(generated_template, template)
        trace, generation = run_generation(
            probe, generated_template, args.case_id, args.seed,
            trace_path, generation_path, base_template.parents[1],
            args.timeout_seconds,
        )
        terminal_shape(trace)
        batch_count = trace_batch_count(trace)
        generation_functional, generation_source, _, generation_run = (
            validate_probe_result(
                generation, batch_count, 0, "synthetic", "terminal generation")
        )
        generation_parent = validate_source_contents(
            trace, trace_path, generation_source, "terminal generation")
        validate_prefix(trace, generation_functional, batch_count)
        if generation_run["child_pid"] <= 0:
            raise TerminalEmptyError("generation child PID is invalid")

        functionals: list[dict[str, Any]] = []
        source_parents = {generation_parent}
        pids = {generation_run["child_pid"]}
        for prefix in range(batch_count + 1):
            output = run_root / f"prefix-{prefix}.json"
            result = run_child(
                probe, trace_path, prefix, 0, output, args.timeout_seconds)
            functional, source, _, envelope = validate_probe_result(
                result, prefix, 0, "synthetic", f"terminal prefix {prefix}")
            source_parent = validate_source_contents(
                trace, trace_path, source, f"terminal prefix {prefix}")
            if source_parent in source_parents:
                raise TerminalEmptyError("terminal fresh process reused a source root")
            source_parents.add(source_parent)
            if envelope["child_pid"] in pids:
                raise TerminalEmptyError("terminal fresh process reused a PID")
            pids.add(envelope["child_pid"])
            validate_prefix(trace, functional, prefix)
            functionals.append(functional)
        evidence = validate_cross_prefix(trace, functionals)
        if evidence["generation_preselection_timing_exact"] is not True:
            raise TerminalEmptyError("terminal preselection timing was not exact")
        difference = first_difference(generation_functional, functionals[-1])
        if difference:
            raise TerminalEmptyError(
                f"generation/full replay functional fields differ: {difference}")
        print(json.dumps({
            "trace": str(trace_path),
            "one_batch_then_terminal_empty": True,
            "all_terminal_numeric_recomputed": True,
            "generation_vs_full_replay_exact": True,
        }, sort_keys=True))
        return 0
    except (
        ControllerError,
        FunctionalEvidenceError,
        TerminalEmptyError,
        OSError,
        UnicodeError,
        json.JSONDecodeError,
        subprocess.SubprocessError,
    ) as error:
        print(f"terminal-empty production validation failed: {error}",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
