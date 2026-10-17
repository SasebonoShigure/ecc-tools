"""Real-process checks for compact input, native loading and final-only reporting."""

import argparse
import copy
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

from trace_process import first_difference, require, write_json

SCHEMA_ROOT = pathlib.Path(__file__).resolve().parents[2] / "benchmark/schema"
sys.path.insert(0, str(SCHEMA_ROOT))
from consumer_trace_samples import validate_consumer_measurement  # noqa: E402


def prepare_input(root: pathlib.Path) -> dict:
    fixture_root = pathlib.Path(__file__).resolve().parents[1] / "timing_edit"
    replay = json.loads((fixture_root / "traces/real_buffer_fanout.json").read_text())["replay"]
    workload = replay["workload"]
    sources = copy.deepcopy(workload["artifacts"])
    files = []
    for key, role in (("tech_lef", "tech_lef"), ("cell_lefs", "cell_lef"),
                      ("liberties", "liberty"), ("verilog", "verilog"), ("sdc", "sdc")):
        entries = sources[key] if isinstance(sources[key], list) else [sources[key]]
        for index, source in enumerate(entries):
            path = (fixture_root / source["path"]).resolve(strict=True)
            source["path"] = str(path)
            logical = f"{index}-{path.name}" if isinstance(sources[key], list) else path.name
            files.append({"index": len(files), "role": role, "logical_name": logical,
                          "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    edits = copy.deepcopy(workload["batches"][0]["edits"])
    first = {"expected_generation": 0, "edits": edits[:2]}
    second = {"expected_generation": 1, "edits": edits[2:]}
    for index, edit in enumerate(second["edits"]):
        edit["creation_token"].update(batch_ordinal=2, accepted_ordinal=index)
    baseline = workload["baseline"]["canonical_pre_state"]
    value = {"schema_version": "ista-scale-compact-replay/2",
             "profile_id": replay["header"]["profile_id"], "artifacts": sources,
             "input_files": files, "batches": [first, second],
             "expected_baseline_instances": len(baseline["instances"]),
             "expected_baseline_nodes": len(baseline["nodes"])}
    write_json(root / "input.json", value)
    return value


def run(probe: pathlib.Path, root: pathlib.Path) -> int:
    build = json.loads(subprocess.check_output([str(probe), "build-info"], text=True))
    if build["debug_checks_enabled"]:
        result = subprocess.run([str(probe), "benchmark"], capture_output=True, text=True)
        require(result.returncode != 0 and "DEBUG_CHECKS=OFF" in result.stderr,
                "ON benchmark did not reject before parsing or loading")
        return 0
    if build["build_config"] != "Release":
        return 77
    root.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(tempfile.mkdtemp(prefix="run-", dir=root))
    original = prepare_input(root)
    affinity = sorted(os.sched_getaffinity(0))

    def child(name, value, backend="cpu", strategy="full", error=None, text=None):
        trace, output = root / f"{name}-input.json", root / f"{name}.json"
        trace.write_text(json.dumps(value) if text is None else text)
        command = [str(probe), "benchmark", "--replay", str(trace), "--prefix", "2",
                   "--backend", backend, "--strategy", strategy, "--threads", "1",
                   "--output", str(output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (root / f"{name}.log").write_text(result.stdout + result.stderr)
        write_json(root / f"{name}-exit.json", {"command": command, "exit": result.returncode})
        if error is not None:
            require(result.returncode != 0 and error in result.stderr and not output.exists(),
                    f"{name} did not reject input as expected: {result.stderr}")
            return None
        require(result.returncode == 0, f"{name} failed: {result.stderr}")
        value = json.loads(output.read_text())
        validate_consumer_measurement(value, f"{backend}/{strategy}", 2, 1, affinity)
        return value

    native = child("native", original)
    repeated = child("native-repeat", original)
    assert native is not None and repeated is not None
    require(first_difference(native["result"], repeated["result"]) is None,
            "native fresh processes disagree")
    if build["cuda_enabled"]:
        cuda = child("cuda", original, "cuda", "incremental")
        assert cuda is not None
        difference = first_difference(native["result"], cuda["result"])
        require(difference is None, f"native/CUDA public results differ: {difference}")
    for name, error in (("hash", "source file identity"), ("role", "source file identity"),
                        ("order", "source file order"), ("generation", "generation"),
                        ("profile", "unknown workload profile"), ("scale", "baseline scale"),
                        ("empty", "at least one batch")):
        value = copy.deepcopy(original)
        if name == "hash":
            value["input_files"][0]["sha256"] = "0" * 64
        elif name == "role":
            value["input_files"][0]["role"] = "sdc"
        elif name == "order":
            value["input_files"][0]["index"] = 1
        elif name == "generation":
            value["batches"][1]["expected_generation"] = 0
        elif name == "profile":
            value["profile_id"] = "unknown"
        elif name == "scale":
            value["expected_baseline_nodes"] += 1
        else:
            value["batches"] = []
        child(name, value, error=error)
    child("duplicate", original, error="duplicate",
          text='{"schema_version":"ista-scale-compact-replay/2",' + json.dumps(original)[1:])
    child("retired-cpu-incremental", original, strategy="incremental",
          error="supports native CPU/full and CUDA/incremental only")
    child("retired-cuda-full", original, backend="cuda",
          error="supports native CPU/full and CUDA/incremental only")
    write_json(root / "verdict.json", {"passed": True, "build": build,
                                      "modes_checked": 2 if build["cuda_enabled"] else 1})
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    parser.add_argument("--run-root", type=pathlib.Path, required=True)
    args = parser.parse_args()
    raise SystemExit(run(args.probe.resolve(strict=True), args.run_root))
