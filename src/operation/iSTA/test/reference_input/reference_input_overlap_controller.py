#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
# http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
#
# See the Mulan PSL v2 for more details.
# ***************************************************************************************
"""Prove that native different-clock-map-key overlap produces no result."""

import argparse
import os
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

_SELF_TEST_ARGUMENT = "--self-test-different-clock-map-key"
_EXPECTED_EXIT_CODE = 86
_EXPECTED_STDERR_TOKEN = b"clock trees overlap at pin 'root': 'diamond_key' and 'z_overlap_key'"
_SUCCESS = "reference-input different-clock-map-key overlap self-test passed"


class OverlapControllerError(RuntimeError):
    """The native overlap no-result contract was not observed exactly."""


def _run_overlap_probe(probe: Path) -> None:
    resolved_probe = probe.resolve()
    if not resolved_probe.is_file():
        raise OverlapControllerError(f"probe is not a regular file: {resolved_probe}")

    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    with tempfile.TemporaryDirectory(prefix="ista-reference-input-overlap-") as directory:
        run_directory = Path(directory)
        try:
            completed = subprocess.run(
                (str(resolved_probe), _SELF_TEST_ARGUMENT),
                cwd=run_directory,
                env=environment,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=False,
                timeout=120.0,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise OverlapControllerError(f"cannot complete native overlap probe: {exc}") from exc
        if completed.returncode != _EXPECTED_EXIT_CODE:
            raise OverlapControllerError(
                f"native overlap probe exited {completed.returncode}, expected "
                f"{_EXPECTED_EXIT_CODE}"
            )
        if completed.stdout:
            raise OverlapControllerError("native overlap probe wrote stdout")
        if _EXPECTED_STDERR_TOKEN not in completed.stderr:
            raise OverlapControllerError("native overlap probe omitted its fixed fatal token")
        if tuple(run_directory.rglob("*")):
            raise OverlapControllerError("native overlap probe left files in its run directory")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        _run_overlap_probe(_parser().parse_args(argv).probe)
    except OverlapControllerError as exc:
        print(f"reference-input overlap controller failed: {exc}", file=sys.stderr)
        return 1
    print(_SUCCESS)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
