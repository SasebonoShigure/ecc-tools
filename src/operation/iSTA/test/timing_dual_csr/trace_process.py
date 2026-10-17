"""JSON comparison and isolated child execution for STA acceptance tools."""

import json
import math
import pathlib
import subprocess
import sys
import time
from typing import Any, Literal

ABS_TOLERANCE = 1.0e-9
REL_TOLERANCE = 1.0e-9


class ControllerError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:  # noqa: FBT001
    if not condition:
        raise ControllerError(message)


def require_exact_keys(value: object, keys: set[str], label: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(set(value) == keys, f"{label} fields differ: {sorted(value)}")
    return value


def read_json(path: pathlib.Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(value, dict), f"{path} must contain a JSON object")
    return value


def write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def numeric_equal(left: object, right: object) -> bool:
    if isinstance(left, bool) or isinstance(right, bool):
        return False
    if not isinstance(left, (int, float)) or not isinstance(right, (int, float)):
        return False
    if isinstance(left, int) and isinstance(right, int):
        return left == right
    return math.isclose(
        float(left),
        float(right),
        rel_tol=REL_TOLERANCE,
        abs_tol=ABS_TOLERANCE,
    )


def first_difference(left: object, right: object, path: str = "$") -> str | None:
    if type(left) is not type(right):
        if numeric_equal(left, right):
            return None
        return f"{path}: type {type(left).__name__} != {type(right).__name__}"
    if isinstance(left, dict):
        if set(left) != set(right):
            return f"{path}: keys {sorted(left)} != {sorted(right)}"
        for key in sorted(left):
            difference = first_difference(left[key], right[key], f"{path}.{key}")
            if difference:
                return difference
        return None
    if isinstance(left, list):
        if len(left) != len(right):
            return f"{path}: length {len(left)} != {len(right)}"
        for index, (lhs, rhs) in enumerate(zip(left, right, strict=True)):
            difference = first_difference(lhs, rhs, f"{path}[{index}]")
            if difference:
                return difference
        return None
    if isinstance(left, float) and numeric_equal(left, right):
        return None
    if left != right:
        return f"{path}: {left!r} != {right!r}"
    return None


def run_child(
    probe: pathlib.Path,
    role: str,
    trace: pathlib.Path,
    prefix: int,
    output: pathlib.Path,
    timeout: int,
    *,
    input_flag: Literal["--trace", "--replay"] = "--trace",
    extra_args: tuple[str, ...] = (),
    cpu_affinity: list[int] | None = None,
) -> tuple[dict[str, Any], float]:
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(probe),
        role,
        input_flag,
        str(trace),
        "--prefix",
        str(prefix),
        "--output",
        str(output),
        *extra_args,
    ]
    if cpu_affinity is not None:
        command = [
            sys.executable,
            "-c",
            "import json,os,sys; os.sched_setaffinity(0,json.loads(sys.argv[1])); "
            "os.execv(sys.argv[2],sys.argv[2:])",
            json.dumps(cpu_affinity),
            *command,
        ]
    write_json(output.with_suffix(".command.json"), command)
    started = time.perf_counter()
    stdout_path, stderr_path = output.with_suffix(".stdout.txt"), output.with_suffix(".stderr.txt")
    with (
        stdout_path.open("w", encoding="utf-8") as stdout,
        stderr_path.open("w", encoding="utf-8") as stderr,
    ):
        try:
            completed = subprocess.run(
                command,
                cwd=trace.parent.parent,
                check=False,
                stdout=stdout,
                stderr=stderr,
                text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            write_json(output.with_suffix(".status.json"), {"exit_code": None, "timeout": True})
            raise
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    write_json(
        output.with_suffix(".status.json"),
        {
            "exit_code": completed.returncode,
            "timeout": False,
            "wall_ms": elapsed_ms,
        },
    )
    require(
        completed.returncode == 0,
        f"{' '.join(command)} failed ({completed.returncode}): "
        f"{stderr_path.read_text(encoding='utf-8')[-4000:].strip()}",
    )
    return read_json(output), elapsed_ms
