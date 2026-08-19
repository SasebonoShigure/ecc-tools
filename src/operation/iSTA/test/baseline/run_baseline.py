#!/usr/bin/env python3
"""Fresh-process orchestration for the stage-0 iSTA full-STA baseline.

The native probe owns production C++ state extraction.  The sibling contract
and artifact modules own validation and comparison; this controller owns only
process isolation and the explicit golden-recording boundary.
"""

import argparse
import os
import platform
import subprocess
import sys
import tempfile
import time
import uuid
from collections.abc import Mapping
from pathlib import Path
from typing import Any
from unittest.mock import patch

from baseline_artifacts import (
    artifact_bundle,
    compare_artifacts,
    run_artifact_self_test,
    run_golden_self_test,
    validate_golden,
)
from baseline_contract import (
    BaselineError,
    assert_equal,
    atomic_json,
    golden_payload,
    load_json,
    require_mapping,
    run_contract_self_test,
    validate_manifest,
    validate_repeat_count,
    validate_snapshot,
)

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_MANIFEST = SCRIPT_DIR / "manifest.json"


def validate_generated_destination(candidate: Path, golden_directory: Path, where: str) -> None:
    """Reject generated output paths that resolve inside checked-in references."""
    resolved = candidate.resolve()
    golden = golden_directory.resolve()
    try:
        resolved.relative_to(golden)
    except ValueError:
        return
    raise BaselineError(f"{where} resolves inside checked-in golden directory: {resolved}")


def select_temporary_parent(environment: Mapping[str, str]) -> Path:
    """Select the Linux harness temp parent without tempfile's writable probe."""
    for variable in ("TMPDIR", "TEMP", "TMP"):
        candidate = environment.get(variable)
        if candidate:
            return Path(candidate).resolve()
    return Path("/tmp").resolve()


def create_temporary_output_root(temporary_parent: Path, golden_directory: Path) -> Path:
    """Create a default output only beneath an already validated temporary parent."""
    parent = temporary_parent.resolve()
    validate_generated_destination(parent, golden_directory, "temporary output parent")
    output_root = Path(tempfile.mkdtemp(prefix="ista-stage0-baseline-", dir=parent)).resolve()
    validate_generated_destination(output_root, golden_directory, "output root")
    return output_root


def _expect_destination_error(candidate: Path, golden_directory: Path, where: str) -> None:
    try:
        validate_generated_destination(candidate, golden_directory, where)
    except BaselineError:
        return
    raise BaselineError(f"path negative self-test {where!r} was accepted")


def run_path_self_test() -> None:
    golden = Path("/baseline/golden")
    validate_generated_destination(Path("/baseline/build"), golden, "safe sibling")
    _expect_destination_error(golden, golden, "output root")
    _expect_destination_error(golden / "session-1", golden, "session root")
    _expect_destination_error(golden / "controller-result.json", golden, "result")
    with (
        patch("tempfile.gettempdir") as probe_directory,
        patch("tempfile.mkdtemp") as make_directory,
    ):
        temporary_parent = select_temporary_parent(
            {"TMPDIR": str(golden), "TEMP": "/ignored", "TMP": "/also-ignored"}
        )
        try:
            create_temporary_output_root(temporary_parent, golden)
        except BaselineError:
            pass
        else:
            raise BaselineError("temporary output parent negative self-test was accepted")
        if probe_directory.called or make_directory.called:
            raise BaselineError("tempfile probed or created output before rejecting its parent")
    print("generated-path self-test: 6 checks passed")


def run_self_test(manifest_path: Path) -> int:
    run_contract_self_test(manifest_path)
    run_golden_self_test(manifest_path)
    run_artifact_self_test()
    run_path_self_test()
    print("controller self-test: all checks passed")
    return 0


def run_probe(
    probe: Path, case: dict[str, Any], baseline_dir: Path, run_dir: Path
) -> dict[str, Any]:
    fixture = baseline_dir / "fixtures"
    snapshot_path = run_dir / "snapshot.json"
    command = [
        str(probe),
        "--tech-lef",
        str(fixture / "stage0_tech.lef"),
        "--cell-lef",
        str(fixture / "stage0_cells.lef"),
        "--liberty",
        str(fixture / "stage0.lib"),
        "--verilog",
        str(baseline_dir / case["verilog"]),
        "--sdc",
        str(baseline_dir / case["sdc"]),
        "--top",
        case["top"],
        "--case-name",
        case["name"],
        "--run-dir",
        str(run_dir),
        "--snapshot",
        str(snapshot_path),
        "--expected-clock",
        case["expected_clock"],
    ]
    environment = os.environ.copy()
    environment.update(
        {
            "LC_ALL": "C",
            "LANG": "C",
            "TZ": "UTC",
            "OMP_NUM_THREADS": "1",
            "OPENBLAS_NUM_THREADS": "1",
            "MKL_NUM_THREADS": "1",
        }
    )
    completed = subprocess.run(
        command,
        cwd=baseline_dir,
        env=environment,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "probe.stdout.log").write_text(completed.stdout, encoding="utf-8", newline="\n")
    (run_dir / "probe.stderr.log").write_text(completed.stderr, encoding="utf-8", newline="\n")
    if completed.returncode != 0:
        raise BaselineError(
            f"probe failed for {case['name']} with exit {completed.returncode}; "
            f"see {run_dir / 'probe.stderr.log'}"
        )
    if not snapshot_path.is_file():
        raise BaselineError(
            f"probe returned success without an atomic snapshot for {case['name']}; "
            "this also catches Logger paths that terminate with exit(0)"
        )
    snapshot = require_mapping(load_json(snapshot_path), "snapshot")
    validate_snapshot(snapshot, case)
    return snapshot


def execute_case(
    action: str,
    probe: Path,
    case: dict[str, Any],
    manifest: dict[str, Any],
    manifest_path: Path,
    output_root: Path,
    repeats: int,
) -> tuple[dict[str, Any], dict[str, Any]]:
    absolute = float(manifest["tolerances"]["absolute"])
    relative = float(manifest["tolerances"]["relative"])
    baseline_dir = manifest_path.parent
    snapshots: list[dict[str, Any]] = []
    artifacts: list[dict[str, Any]] = []
    started = time.perf_counter()
    for index in range(repeats):
        run_dir = output_root / case["name"] / f"run-{index + 1:02d}-{uuid.uuid4().hex[:8]}"
        snapshot = run_probe(probe, case, baseline_dir, run_dir)
        bundle = artifact_bundle(run_dir, case)
        snapshots.append(snapshot)
        artifacts.append(bundle)
        if index:
            assert_equal(
                snapshots[0], snapshot, f"{case['name']}.fresh_process[{index}]", absolute, relative
            )
            compare_artifacts(
                artifacts[0], bundle, absolute, relative, f"{case['name']}.artifacts[{index}]"
            )

    current = golden_payload(case, manifest, snapshots[0], artifacts[0])
    validate_golden(current, case, manifest)
    expected_topology = case.get("expected_topology_sha256")
    if expected_topology is not None and expected_topology != current["topology_hash"]:
        raise BaselineError(
            f"{case['name']}: manifest topology hash changed; "
            f"expected {expected_topology}, got {current['topology_hash']}"
        )
    golden_path = baseline_dir / manifest["golden_directory"] / f"{case['name']}.json"
    if action != "record":
        if not golden_path.is_file():
            raise BaselineError(
                f"golden is absent for {case['name']}: {golden_path}; "
                "use --record-golden explicitly"
            )
        golden = require_mapping(load_json(golden_path), f"golden {case['name']}")
        validate_golden(golden, case, manifest)
        if golden.get("topology_hash") != current["topology_hash"]:
            raise BaselineError(f"{case['name']}: topology hash changed")
        if golden.get("config_hash") != current["config_hash"]:
            raise BaselineError(f"{case['name']}: configuration/input hash changed")
        assert_equal(
            golden.get("snapshot"),
            current["snapshot"],
            f"{case['name']}.golden.snapshot",
            absolute,
            relative,
        )
        compare_artifacts(
            require_mapping(golden.get("artifacts"), "golden.artifacts"),
            current["artifacts"],
            absolute,
            relative,
            f"{case['name']}.golden.artifacts",
        )
    elapsed = time.perf_counter() - started
    result = {
        "case": case["name"],
        "repeats": repeats,
        "topology_hash": current["topology_hash"],
        "config_hash": current["config_hash"],
        "full_snapshot_sha256_diagnostic": current["provenance"]["full_snapshot_sha256_diagnostic"],
        "artifact_paths": current["provenance"]["artifact_paths"],
        "elapsed_controller_seconds": elapsed,
        "status": "recorded" if action == "record" else "verified",
    }
    return result, current


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--probe", type=Path, help="current-source sta_full_baseline_probe executable"
    )
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--output-root",
        type=Path,
        default=None,
        help="generated run directory (never a golden directory)",
    )
    parser.add_argument("--repeats", type=int, default=None, help="fresh OS processes per case")
    parser.add_argument(
        "--case", action="append", dest="cases", help="run only a named case; may be repeated"
    )
    parser.add_argument(
        "--result", type=Path, default=None, help="write machine-readable controller summary"
    )
    parser.add_argument(
        "--record-golden",
        action="store_true",
        help="explicitly replace checked-in golden references",
    )
    parser.add_argument(
        "--self-test", action="store_true", help="run controller contract checks without native STA"
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    manifest_path = args.manifest.resolve()
    if args.self_test:
        return run_self_test(manifest_path)
    manifest = require_mapping(load_json(manifest_path), "manifest")
    validate_manifest(manifest, manifest_path, allow_unpinned_topology=args.record_golden)
    if args.probe is None:
        raise BaselineError("--probe is required unless --self-test is used")
    probe = args.probe.resolve()
    if not probe.is_file() or not os.access(probe, os.X_OK):
        raise BaselineError(f"probe is not an executable file: {probe}")
    repeats = (
        args.repeats
        if args.repeats is not None
        else int(manifest["fresh_process_repeats"]["local"])
    )
    repeats = validate_repeat_count(repeats)
    selected = set(args.cases or [])
    cases = [case for case in manifest["cases"] if not selected or case["name"] in selected]
    missing = selected - {case["name"] for case in cases}
    if missing:
        raise BaselineError(f"unknown case(s): {', '.join(sorted(missing))}")
    action = "record" if args.record_golden else "verify"
    golden_directory = (manifest_path.parent / manifest["golden_directory"]).resolve()
    if args.output_root is None:
        temporary_parent = select_temporary_parent(os.environ)
        output_root = create_temporary_output_root(temporary_parent, golden_directory)
        owns_output = True
    else:
        output_root = args.output_root.resolve()
        owns_output = False
    validate_generated_destination(output_root, golden_directory, "output root")
    session_root = output_root / f"session-{int(time.time())}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    validate_generated_destination(session_root, golden_directory, "session root")
    result_path = args.result.resolve() if args.result else session_root / "controller-result.json"
    validate_generated_destination(result_path, golden_directory, "result")
    output_root.mkdir(parents=True, exist_ok=True)
    session_root.mkdir(parents=True, exist_ok=False)
    summary = {
        "controller_schema_version": 1,
        "action": action,
        "probe": str(probe),
        "manifest": str(manifest_path),
        "session_root": str(session_root),
        "host": {"platform": platform.platform(), "python": platform.python_version()},
        "cases": [],
        "complete": False,
    }
    recordings: list[tuple[dict[str, Any], dict[str, Any]]] = []
    try:
        for case in cases:
            case_result, current = execute_case(
                action, probe, case, manifest, manifest_path, session_root, repeats
            )
            summary["cases"].append(case_result)
            if action == "record":
                recordings.append((case, current))
        if action == "record":
            for case, current in recordings:
                golden_path = (
                    manifest_path.parent / manifest["golden_directory"] / f"{case['name']}.json"
                )
                atomic_json(golden_path, current)
        summary["complete"] = True
        atomic_json(result_path, summary)
        print(
            f"stage-0 baseline {action} passed: {len(cases)} case(s), "
            f"{repeats} fresh process(es) each"
        )
        print(f"result: {result_path}")
        if owns_output:
            print(f"generated runs retained for audit: {output_root}")
        return 0
    except Exception:
        atomic_json(result_path, summary)
        raise


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except BaselineError as exc:
        print(f"stage-0 baseline error: {exc}", file=sys.stderr)
        raise SystemExit(2) from None
