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
"""Compare five compiled C++ contract tables with the normative manifest."""

import argparse
import subprocess
import sys
from pathlib import Path

from json_schema_contract import ValidationError, load_json

MANIFEST_PATH = Path(__file__).with_name("csr_snapshot_contract_manifest_v1.json")
_EXECUTABLES = ("identity", "fifo", "storage", "observation", "transition")
_ACTIONS = {
    "-",
    "read",
    "reference_measurement",
    "scatter_rejection",
    "scatter_success",
    "invalidation",
    "release",
}
_VALIDITY = {"valid", "invalid"}
_COLUMN_COUNT = 10


def _manifest_descriptor(case: dict[str, object]) -> tuple[str, ...]:
    recipe = case["recipe"]
    expected = case["expected"]
    return (
        case["case_id"],
        case["baseline"],
        recipe["op"],
        recipe["target"],
        recipe["operand"],
        case["action_token"],
        expected["validity"],
        expected["field"],
        expected["category"],
        expected["message"],
    )


def _require_ascii_field(field: object, context: str) -> str:
    if type(field) is not str or not field:
        raise AssertionError(f"{context}: descriptor fields must be nonempty strings")
    try:
        field.encode("ascii")
    except UnicodeEncodeError as error:
        raise AssertionError(f"{context}: descriptor fields must be ASCII") from error
    if any(character in field for character in "\t\r\n"):
        raise AssertionError(f"{context}: descriptor fields must not contain TAB/CR/LF")
    return field


def _validate_descriptor(row: tuple[str, ...], context: str) -> None:
    if len(row) != _COLUMN_COUNT:
        raise AssertionError(f"{context}: expected exactly 10 TSV columns, got {len(row)}")
    for field in row:
        _require_ascii_field(field, context)
    if row[5] not in _ACTIONS:
        raise AssertionError(f"{context}: unknown action token {row[5]!r}")
    if row[6] not in _VALIDITY:
        raise AssertionError(f"{context}: unknown validity token {row[6]!r}")
    if row[6] == "valid" and row[7:] != ("-", "-", "-"):
        raise AssertionError(f"{context}: valid case expected columns must all be '-'")
    if row[6] == "invalid" and (row[7] == "-" or row[9] == "-"):
        raise AssertionError(f"{context}: invalid case field/message must not be '-'")


def _run_compiled(name: str, path: Path) -> bytes:
    completed = subprocess.run(
        [path, "--list-case-contracts"],
        check=False,
        capture_output=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"{name}: executable returned {completed.returncode}")
    if completed.stderr:
        raise AssertionError(f"{name}: executable wrote to stderr")
    return completed.stdout


def _parse_compiled(name: str, raw_output: bytes) -> list[tuple[str, ...]]:
    try:
        output = raw_output.decode("ascii")
    except UnicodeDecodeError as error:
        raise AssertionError(f"{name}: descriptor output must be ASCII") from error
    if "\r" in output:
        raise AssertionError(f"{name}: descriptor output must not contain CR")
    rows: list[tuple[str, ...]] = []
    for line_number, line in enumerate(output.splitlines(), 1):
        if not line:
            raise AssertionError(f"{name}:{line_number}: blank descriptor line")
        row = tuple(line.split("\t"))
        _validate_descriptor(row, f"{name}:{line_number}")
        rows.append(row)
    if not rows:
        raise AssertionError(f"{name}: executable returned no descriptors")
    return rows


def _parse_executables(values: list[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        name, separator, raw_path = value.partition("=")
        if not separator or name not in _EXECUTABLES or not raw_path:
            raise AssertionError(f"invalid --executable value {value!r}")
        if name in result:
            raise AssertionError(f"duplicate executable key {name!r}")
        result[name] = Path(raw_path)
    missing = set(_EXECUTABLES) - set(result)
    extra = set(result) - set(_EXECUTABLES)
    if missing or extra:
        raise AssertionError(
            f"executable keys differ: missing={sorted(missing)}, extra={sorted(extra)}"
        )
    return result


def validate_compiled_contracts(manifest_path: Path, executable_args: list[str]) -> int:
    manifest = load_json(manifest_path)
    if not isinstance(manifest, dict) or manifest.get("schema_version") != "1.0.0":
        raise AssertionError("invalid snapshot contract manifest")
    executables = _parse_executables(executable_args)
    expected: dict[str, list[tuple[str, ...]]] = {name: [] for name in _EXECUTABLES}
    expected_ids: set[str] = set()
    for index, case in enumerate(manifest["typed_cpp_cases"]):
        name = case["executable"]
        if name not in expected:
            raise AssertionError(f"manifest case {index}: unknown executable {name!r}")
        row = _manifest_descriptor(case)
        _validate_descriptor(row, f"manifest:{index}")
        if row[0] in expected_ids:
            raise AssertionError(f"manifest duplicate case id {row[0]!r}")
        expected_ids.add(row[0])
        expected[name].append(row)
    raw_outputs = {name: _run_compiled(name, executables[name]) for name in _EXECUTABLES}
    actual: dict[str, list[tuple[str, ...]]] = {}
    actual_ids: set[str] = set()
    for name in _EXECUTABLES:
        rows = _parse_compiled(name, raw_outputs[name])
        for row in rows:
            if row[0] in actual_ids:
                raise AssertionError(f"compiled duplicate case id {row[0]!r}")
            actual_ids.add(row[0])
        actual[name] = rows
    for name in _EXECUTABLES:
        if sorted(actual[name]) != sorted(expected[name]):
            missing = sorted(set(expected[name]) - set(actual[name]))
            extra = sorted(set(actual[name]) - set(expected[name]))
            raise AssertionError(f"{name}: descriptor mismatch; missing={missing}, extra={extra}")
    return len(actual_ids)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    parser.add_argument("--executable", action="append", default=[], metavar="NAME=PATH")
    args = parser.parse_args(argv)
    try:
        checks = validate_compiled_contracts(args.manifest, args.executable)
        print(f"compiled-contract parity: {checks} descriptors matched")
    except (AssertionError, OSError, TypeError, KeyError, ValidationError) as error:
        print(f"compiled-contract parity failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
