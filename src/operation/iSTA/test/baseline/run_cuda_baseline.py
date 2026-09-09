#!/usr/bin/env python3
"""Real-design CUDA full baseline versus independent fresh CPU executions."""

import argparse
import tempfile
from pathlib import Path

from baseline_artifacts import artifact_bundle, compare_artifacts
from baseline_contract import assert_equal, atomic_json, load_json, validate_manifest
from run_baseline import run_probe, validate_generated_destination


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
    for case in manifest["cases"]:
        cpu_dir = run_root / case["name"] / "cpu"
        expected = run_probe(args.probe.resolve(), case, baseline, cpu_dir)
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
