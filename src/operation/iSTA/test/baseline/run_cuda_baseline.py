#!/usr/bin/env python3
"""Real-design CUDA full baseline versus independent fresh CPU executions."""

import argparse
import json
import tempfile
from pathlib import Path

from baseline_artifacts import artifact_bundle, compare_artifacts
from baseline_contract import BaselineError, assert_equal, atomic_json, load_json, validate_manifest
from run_baseline import run_probe, validate_generated_destination


def require_sequential_coverage(snapshot: dict, *, propagated: bool = False) -> None:
    paths = [
        path
        for group in snapshot["timing"]["path_groups"]
        for endpoint in group["endpoints"]
        for path in endpoint["paths"]
    ]
    if not {"setup", "hold", "recovery", "removal"} <= {path["check_type"] for path in paths}:
        raise BaselineError("sequential fixture did not execute all four timing check kinds")
    points = {point["node"]: point for point in snapshot["timing"]["points"]}
    triggers = {
        path["crpr_clock_transition"]
        for point in points.values()
        for path in point["path_states"]
        if path["source_type"] == "register"
    }
    if triggers != {"rise", "fall"}:
        raise BaselineError("sequential fixture lost rising/falling register launch identities")
    if not propagated and any(path["clock_reconvergence_pessimism"] != 0.0 for path in paths):
        raise BaselineError("ideal-clock fixture acquired nonzero CRPR correction")
    for pin in ("launch:CK", "capture:CK"):
        point = points[pin]
        if not point["is_clock_point"] or not point["clock_physical"]["predecessor"]:
            raise BaselineError(f"clock fanout did not reach {pin}")
        if not any(lane["value"] > 0.0 for lane in point["clock_physical"]["arrival"]):
            raise BaselineError(f"physical clock buffer delay was lost at {pin}")
        if propagated:
            if point["clock_effective"] != point["clock_physical"]:
                raise BaselineError(f"propagated clock lost physical values or identity at {pin}")
        elif any(lane["value"] != 0.0 for lane in point["clock_effective"]["arrival"]):
            raise BaselineError(f"ideal effective clock changed at {pin}")
    conditions = {
        json.loads(entry["timing_arc"].split("#parallel=")[0])[-1]
        for arc in snapshot["timing"]["arcs"]
        for entry in arc["timing_arc_delay"]
        if isinstance(entry["timing_arc"], str)
    }
    if not {"RN", "!RN"} <= conditions:
        raise BaselineError("conditional CTQ candidates were not both evaluated")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    args.output_root = args.output_root.resolve()
    baseline = Path(__file__).resolve().parent
    validate_generated_destination(args.output_root, baseline / "golden", "CUDA output root")
    args.output_root.mkdir(parents=True, exist_ok=True)
    run_root = Path(tempfile.mkdtemp(prefix="run-", dir=args.output_root))
    atomic_json(
        args.output_root / "verdict.json",
        {
            "schema": "ista-cuda-native-baseline/1.0.0",
            "run_root": str(run_root),
            "status": "not_completed",
        },
    )
    manifest = load_json(baseline / "manifest.json")
    validate_manifest(manifest, baseline / "manifest.json")
    absolute = float(manifest["tolerances"]["absolute"])
    relative = float(manifest["tolerances"]["relative"])
    results = []
    sequential_case = {
        "name": "sequential",
        "top": "stage10_sequential",
        "verilog": "fixtures/sequential.v",
        "sdc": "fixtures/sequential.sdc",
        "liberty": "fixtures/sequential.lib",
        "cell_lef": "fixtures/sequential_cells.lef",
        "expected_clock": "clk",
        "required_artifacts": manifest["cases"][0]["required_artifacts"],
    }
    propagated_case = {
        **sequential_case,
        "name": "sequential_propagated",
        "sdc": "fixtures/sequential_propagated.sdc",
    }
    for case in [*manifest["cases"], sequential_case, propagated_case]:
        cpu_dir = run_root / case["name"] / "cpu"
        expected = run_probe(args.probe.resolve(), case, baseline, cpu_dir)
        if case["name"].startswith("sequential"):
            require_sequential_coverage(
                expected, propagated=case["name"] == "sequential_propagated"
            )
        expected_artifacts = artifact_bundle(cpu_dir, case)
        for repeat in range(2):
            cuda_dir = run_root / case["name"] / f"cuda-{repeat}"
            actual = run_probe(args.probe.resolve(), case, baseline, cuda_dir, backend="cuda")
            assert_equal(expected, actual, case["name"], absolute, relative)
            compare_artifacts(
                expected_artifacts,
                artifact_bundle(cuda_dir, case),
                absolute,
                relative,
                case["name"],
            )
            results.append({"case": case["name"], "repeat": repeat, "equivalent": True})
    for name, command in (
        ("missing_clock", "set_propagated_clock\n"),
        ("unknown_clock", "set_propagated_clock [get_clocks {clk missing}]\n"),
    ):
        sdc = run_root / f"{name}.sdc"
        sdc.write_text((baseline / sequential_case["sdc"]).read_text() + command)
        case = {**sequential_case, "name": name, "sdc": str(sdc)}
        destination = run_root / name
        try:
            run_probe(args.probe.resolve(), case, baseline, destination)
        except BaselineError as error:
            stderr = (destination / "probe.stderr.log").read_text()
            if "set_propagated_clock" not in stderr or (destination / "snapshot.json").exists():
                raise BaselineError(
                    f"{name} did not reject malformed clocks before publication"
                ) from error
        else:
            raise BaselineError(f"{name} was silently accepted")
        results.append({"case": name, "rejected": True})
    atomic_json(
        args.output_root / "verdict.json",
        {
            "schema": "ista-cuda-native-baseline/1.0.0",
            "run_root": str(run_root),
            "status": "completed",
            "results": results,
        },
    )


if __name__ == "__main__":
    main()
