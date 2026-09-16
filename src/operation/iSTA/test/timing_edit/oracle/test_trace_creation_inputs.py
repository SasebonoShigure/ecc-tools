#!/usr/bin/env python3
"""Check that arbitrary cell selection cannot turn a gate into a buffer edit."""

import argparse
import json
import pathlib
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    parser.add_argument("--fixture-dir", type=pathlib.Path, required=True)
    parser.add_argument("--run-root", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = args.run_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    output = root / "rejected-trace.json"
    output.unlink(missing_ok=True)
    fixture = args.fixture_dir.resolve(strict=True)
    command = [
        str(args.probe.resolve(strict=True)),
        "--tech-lef", str(fixture / "tech.lef"),
        "--cell-lef", str(fixture / "cell.lef"),
        "--liberty", str(fixture / "stage4.lib"),
        "--verilog", str(fixture / "design.v"),
        "--sdc", str(fixture / "design.sdc"), "--top", "stage4_buffer_fanout",
        "--run-dir", str(root / "native-run"), "--create-trace", str(output),
        "--net", "y0", "--sink", "u4:A", "--buffer", "OR2_X1",
        "--input", "A", "--output-port", "Y",
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
    (root / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    (root / "stdout.log").write_text(result.stdout)
    (root / "stderr.log").write_text(result.stderr)
    if result.returncode == 0 or output.exists() or "two-port identity buffer" not in result.stderr:
        raise RuntimeError(
            f"OR2 must be rejected before trace publication: exit={result.returncode}, "
            f"output={output.exists()}, stderr={result.stderr}"
        )
    print("non-buffer selection rejected before trace publication")


if __name__ == "__main__":
    main()
