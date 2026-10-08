#!/usr/bin/env python3
"""Run two data-only Q/D batches with propagated clocks through existing oracles."""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def require_propagated(functional: dict) -> None:
    graph, timing = functional["graph"], functional["timing"]
    clocks = graph["reference_payload"]["clocks"]
    if len(clocks) != 1 or clocks[0]["map_key"] != "clk" or clocks[0]["propagated"] is not True:
        raise RuntimeError("fixture clock was not propagated")
    nodes = {node["key"]["full_name"]: index for index, node in enumerate(graph["nodes"])}
    for name in ("launch:CK", "capture:CK"):
        for lane in range(nodes[name] * 4, nodes[name] * 4 + 4):
            physical = timing["physical_clock"]
            if physical["arrivals"]["presence"][lane] != 1 or (
                physical["arrivals"]["values"][lane]["value"] <= 0.0
            ):
                raise RuntimeError(f"{name} did not retain positive clock-tree delay")
            for field, values in physical.items():
                effective = timing["effective_clock"][field]
                if any(effective[key][lane] != values[key][lane] for key in ("presence", "values")):
                    raise RuntimeError(f"{name} lost propagated {field}")


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ("oracle", "cpu_probe", "cuda_full_probe", "cuda_incremental_probe", "run_root"):
        parser.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    args = parser.parse_args()
    args.run_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="propagated-", dir=args.run_root.resolve()))
    tests = Path(__file__).resolve().parents[1]
    fixture = tests / "baseline/fixtures"
    commands = []

    def run(label: str, command: list[str]) -> None:
        with (root / f"{label}.stdout.log").open("w") as out, (
            root / f"{label}.stderr.log"
        ).open("w") as err:
            result = subprocess.run(command, stdout=out, stderr=err, timeout=180, check=False)
        commands.append({"label": label, "command": command, "exit_code": result.returncode})
        (root / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode != 0:
            raise RuntimeError(f"{label} failed: {root / f'{label}.stderr.log'}")

    files = {
        "tech_lef": tests / "timing_edit/fixtures/buffer_fanout/tech.lef",
        "cell_lef": fixture / "sequential_cells.lef",
        "liberty": fixture / "sequential.lib",
        "verilog": fixture / "sequential.v",
        "sdc": fixture / "sequential_propagated.sdc",
    }
    source_args = [item for name, path in files.items()
                   for item in ("--" + name.replace("_", "-"), str(path))]
    top = "stage10_sequential"
    artifacts = {
        "tech_lef": {"path": str(files["tech_lef"])},
        "cell_lefs": [{"path": str(files["cell_lef"])}],
        "liberties": [{"path": str(files["liberty"])}],
        "verilog": {"path": str(files["verilog"])},
        "sdc": {"path": str(files["sdc"])},
        "top": top,
        "analysis_context": {
            "liberty_corner": "slow", "temperature": "25", "rc_corner": "__no_rc_corner__",
        },
    }
    controllers = [
        ("cpu-incremental",
         tests / "timing_incremental_levelization/incremental_levelization_controller.py",
         ["--probe", str(args.cpu_probe.resolve(strict=True))]),
        ("cuda-full", tests / "timing_dual_csr/cuda_full_trace_controller.py",
         ["--probe", str(args.cuda_full_probe.resolve(strict=True))]),
        ("cuda-incremental", tests / "timing_dual_csr/cuda_targeted_trace_controller.py",
         ["--probe", str(args.cuda_incremental_probe.resolve(strict=True)),
          "--full-probe", str(args.cuda_full_probe.resolve(strict=True)),
          "--strategy", "incremental"]),
    ]
    for side, net, sink in (("q", "q0", "competing_paths:A"), ("d", "d1", "capture:D")):
        trace = root / f"{side}-native-trace.json"
        run(f"{side}-prepare", [
            str(args.oracle.resolve(strict=True)), *source_args, "--top", top,
            "--run-dir", str(root / f"{side}-prepare"), "--create-trace", str(trace),
            "--buffer", "BUF_X1", "--input", "A", "--output-port", "Y",
            "--net", net, "--sink", sink, "--profile", "engineering_smoke", "--seed", "0",
        ])
        native = json.loads(trace.read_text())
        if len(native["batches"]) != 2 or any(not batch["edits"] for batch in native["batches"]):
            raise RuntimeError("native preparation did not execute two real buffer batches")
        program = {
            "header": {
                "schema_version": 2, "selection_mode": "fixed_trace",
                "semantic_workload_kind": "real_design", "profile_id": "engineering_smoke",
                "comparability_status": "non_comparable",
                "comparability_reason": "engineering_rc_no_physical",
            },
            "workload": {
                "kind": "real_design", "artifacts": artifacts, "baseline": native["baseline"],
                "batches": [
                    {"schema_version": "timing-edit-plan/1.0.0", "baseline": native["baseline"],
                     "expected_generation": ordinal, "edits": batch["edits"]}
                    for ordinal, batch in enumerate(native["batches"])
                ],
            },
        }
        replay = root / f"{side}-replay.json"
        replay.write_text(json.dumps(program, indent=2) + "\n")
        for mode, controller, options in controllers:
            run(f"{side}-{mode}", [
                sys.executable, str(controller), *options, "--replay", str(replay),
                "--run-root", str(root / f"{side}-{mode}"),
            ])
            if mode == "cpu-incremental":
                for prefix in (1, 2):
                    for role in ("targeted", "fresh"):
                        receipts = list((root / f"{side}-{mode}").glob(
                            f"run-*/prefix-{prefix}/{role}.json"))
                        if len(receipts) != 1:
                            raise RuntimeError("missing or ambiguous actual prefix receipt")
                        receipt = json.loads(receipts[0].read_text())
                        functional = (receipt["functional"] if role == "fresh"
                                      else receipt["published_observations"][-1]["functional"])
                        require_propagated(functional)
    print(f"propagated Q/D batches passed independent native comparisons: {root}")


if __name__ == "__main__":
    main()
