#!/usr/bin/env python3
"""Exercise bare replay through the existing controller and reject ambiguous input."""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=pathlib.Path, required=True)
    parser.add_argument("--controller", type=pathlib.Path, required=True)
    parser.add_argument("--trace", type=pathlib.Path, required=True)
    parser.add_argument("--run-root", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.run_root.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(tempfile.mkdtemp(prefix="replay-input-", dir=args.run_root.resolve()))
    trace = args.trace.resolve(strict=True)
    program = root / "replay.json"
    program.write_text(json.dumps(json.loads(trace.read_text())["replay"]) + "\n")
    probe = str(args.probe.resolve(strict=True))
    command = [
        sys.executable, str(args.controller.resolve(strict=True)), "--probe", probe,
        "--replay", str(program), "--run-root", str(root / "controller"),
    ]
    (root / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (root / "stdout.log").open("w") as out, (root / "stderr.log").open("w") as err:
        subprocess.run(command, stdout=out, stderr=err, check=True, timeout=300)

    for label, inputs in (
        ("bare-as-artifact", ["--trace", str(program)]),
        ("both-inputs", ["--trace", str(trace), "--replay", str(program)]),
    ):
        output = root / f"{label}.json"
        result = subprocess.run(
            [probe, "targeted", *inputs, "--prefix", "1", "--output", str(output)],
            capture_output=True, text=True, check=False, timeout=60,
        )
        (root / f"{label}.stdout.log").write_text(result.stdout)
        (root / f"{label}.stderr.log").write_text(result.stderr)
        if result.returncode == 0 or output.exists():
            raise RuntimeError(f"{label} was accepted or published a result")
    print("bare replay completed independent comparison; malformed input was rejected")


if __name__ == "__main__":
    main()
