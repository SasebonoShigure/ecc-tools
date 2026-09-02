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
"""Strict manifest, result-schema, and atomic publication contracts."""

import json
import os
import re
import stat
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_BASELINE_DIRECTORY = Path(__file__).resolve().parents[1] / "baseline"
if str(_BASELINE_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(_BASELINE_DIRECTORY))

from baseline_contract import (  # noqa: E402
    BaselineError,
    load_json,
    require_exact_keys,
    require_list,
    require_mapping,
    validate_manifest,
    validate_repeat_count,
)
from projection_canonical import (  # noqa: E402
    CanonicalDecodeError,
    OracleSummary,
    ProjectionSummary,
    decode_oracle,
    decode_projection,
    validate_acyclic_oracle_shape,
)

PROBE_SCHEMA = "ista-csr-projection-probe/1.0.0"
CONTROLLER_SCHEMA = "ista-csr-projection-controller/1.0.0"
CASE_ORDER = ("chain", "diamond")
PROBE_KEYS = frozenset(
    {
        "schema",
        "run_nonce",
        "case",
        "top",
        "expected_clock",
        "repeat_index",
        "node_count",
        "arc_count",
        "direct_projection",
        "decoded_projection",
        "native_graph_oracle",
        "independent_fifo_oracle",
        "complete",
    }
)
CONTROLLER_KEYS = frozenset({"schema", "requested_repeats", "case_results", "complete"})
CASE_RESULT_KEYS = frozenset({"case", "repeats"})
_COMMON_INPUTS = (
    "fixtures/stage0_tech.lef",
    "fixtures/stage0_cells.lef",
    "fixtures/stage0.lib",
)
_NONCE = re.compile(r"[0-9a-f]{32}")
_LOWER_HEX = re.compile(r"(?:[0-9a-f]{2})+")


class ProjectionContractError(BaselineError):
    """Fresh-process projection contract violation."""


@dataclass(frozen=True)
class CaseDescriptor:
    name: str
    top: str
    verilog: str
    sdc: str
    expected_clock: str
    input_paths: tuple[tuple[str, Path], ...]

    def path(self, relative: str) -> Path:
        matches = tuple(path for name, path in self.input_paths if name == relative)
        if len(matches) != 1:
            raise ProjectionContractError(f"case {self.name!r} lacks unique input {relative!r}")
        return matches[0]

    @property
    def tech_lef(self) -> Path:
        return self.path(_COMMON_INPUTS[0])

    @property
    def cell_lef(self) -> Path:
        return self.path(_COMMON_INPUTS[1])

    @property
    def liberty(self) -> Path:
        return self.path(_COMMON_INPUTS[2])

    @property
    def verilog_path(self) -> Path:
        return self.path(self.verilog)

    @property
    def sdc_path(self) -> Path:
        return self.path(self.sdc)


@dataclass(frozen=True)
class ValidatedProbe:
    record: dict[str, Any]
    projection: ProjectionSummary
    oracle: OracleSummary


def load_case_descriptors(manifest_path: Path) -> tuple[CaseDescriptor, CaseDescriptor]:
    manifest_path = manifest_path.resolve()
    manifest = require_mapping(load_json(manifest_path), "manifest")
    validate_manifest(manifest, manifest_path)
    raw_cases = require_list(manifest.get("cases"), "manifest.cases")
    cases_by_name: dict[str, dict[str, Any]] = {}
    for index, raw_case in enumerate(raw_cases):
        case = require_mapping(raw_case, f"manifest.cases[{index}]")
        name = case.get("name")
        if name in cases_by_name:
            raise ProjectionContractError(f"duplicate projection case {name!r}")
        if isinstance(name, str):
            cases_by_name[name] = case
    if set(cases_by_name) != set(CASE_ORDER):
        raise ProjectionContractError(
            f"projection manifest cases must be exactly {list(CASE_ORDER)!r}; "
            f"got {sorted(cases_by_name)!r}"
        )
    descriptors: list[CaseDescriptor] = []
    for name in CASE_ORDER:
        case = cases_by_name[name]
        expected_inputs = {*_COMMON_INPUTS, case["verilog"], case["sdc"]}
        if len(expected_inputs) != 5:
            raise ProjectionContractError(f"case {name!r} must name exactly five distinct inputs")
        resolved: list[tuple[str, Path]] = []
        for relative in expected_inputs:
            path = (manifest_path.parent / relative).resolve()
            try:
                path.relative_to(manifest_path.parent)
            except ValueError as exc:
                raise ProjectionContractError(
                    f"case input escapes baseline directory: {relative}"
                ) from exc
            if not path.is_file():
                raise ProjectionContractError(f"case input is unavailable: {relative}")
            resolved.append((relative, path))
        descriptors.append(
            CaseDescriptor(
                name=name,
                top=case["top"],
                verilog=case["verilog"],
                sdc=case["sdc"],
                expected_clock=case["expected_clock"],
                input_paths=tuple(resolved),
            )
        )
    return descriptors[0], descriptors[1]


def decode_lower_hex(value: Any, where: str) -> bytes:
    if not isinstance(value, str) or _LOWER_HEX.fullmatch(value) is None:
        raise ProjectionContractError(f"{where} must be non-empty lowercase even-length hex")
    return bytes.fromhex(value)


def _require_positive_count(value: Any, where: str) -> int:
    if type(value) is not int or value <= 0:
        raise ProjectionContractError(f"{where} must be a non-bool positive integer")
    return value


def validate_probe_record(
    raw_record: Any,
    descriptor: CaseDescriptor,
    expected_repeat: int,
    expected_nonce: str,
) -> ValidatedProbe:
    record = require_mapping(raw_record, "probe result")
    require_exact_keys(record, set(PROBE_KEYS), "probe result")
    if record["schema"] != PROBE_SCHEMA:
        raise ProjectionContractError(f"probe schema mismatch: {record['schema']!r}")
    if not isinstance(expected_nonce, str) or _NONCE.fullmatch(expected_nonce) is None:
        raise ProjectionContractError("controller expected nonce is malformed")
    expected = {
        "run_nonce": expected_nonce,
        "case": descriptor.name,
        "top": descriptor.top,
        "expected_clock": descriptor.expected_clock,
        "repeat_index": expected_repeat,
    }
    for field, expected_value in expected.items():
        if type(record[field]) is not type(expected_value) or record[field] != expected_value:
            raise ProjectionContractError(
                f"probe {field} mismatch: expected {expected_value!r}, got {record[field]!r}"
            )
    node_count = _require_positive_count(record["node_count"], "probe.node_count")
    arc_count = _require_positive_count(record["arc_count"], "probe.arc_count")
    if record["complete"] is not True:
        raise ProjectionContractError("probe complete must be exactly true")
    direct = decode_lower_hex(record["direct_projection"], "probe.direct_projection")
    decoded = decode_lower_hex(record["decoded_projection"], "probe.decoded_projection")
    native_oracle = decode_lower_hex(record["native_graph_oracle"], "probe.native_graph_oracle")
    fifo_oracle = decode_lower_hex(
        record["independent_fifo_oracle"], "probe.independent_fifo_oracle"
    )
    if direct != decoded:
        raise ProjectionContractError("direct and decoded projection bytes differ")
    if native_oracle != fifo_oracle:
        raise ProjectionContractError("native and independent FIFO oracle bytes differ")
    try:
        projection = decode_projection(direct)
        oracle = decode_oracle(native_oracle)
        validate_acyclic_oracle_shape(projection, oracle)
    except CanonicalDecodeError as exc:
        raise ProjectionContractError(f"strict canonical decode failed: {exc}") from exc
    if projection.node_count != node_count or projection.arc_count != arc_count:
        raise ProjectionContractError(
            "probe node_count/arc_count disagree with the independently decoded projection"
        )
    return ValidatedProbe(record, projection, oracle)


def validate_controller_result(
    raw_result: Any, descriptors: tuple[CaseDescriptor, CaseDescriptor]
) -> dict[str, Any]:
    result = require_mapping(raw_result, "controller result")
    require_exact_keys(result, set(CONTROLLER_KEYS), "controller result")
    if result["schema"] != CONTROLLER_SCHEMA:
        raise ProjectionContractError(f"controller schema mismatch: {result['schema']!r}")
    repeats = validate_repeat_count(result["requested_repeats"])
    if result["complete"] is not True:
        raise ProjectionContractError("controller complete must be exactly true")
    case_results = require_list(result["case_results"], "controller.case_results")
    if len(case_results) != len(CASE_ORDER):
        raise ProjectionContractError("controller must contain exactly chain and diamond")
    all_nonces: set[str] = set()
    first_projection_by_case: dict[str, str] = {}
    for case_index, descriptor in enumerate(descriptors):
        case_result = require_mapping(case_results[case_index], f"case_results[{case_index}]")
        require_exact_keys(case_result, set(CASE_RESULT_KEYS), f"case_results[{case_index}]")
        if case_result["case"] != descriptor.name:
            raise ProjectionContractError("controller case_results order/case is invalid")
        records = require_list(case_result["repeats"], f"case {descriptor.name}.repeats")
        if len(records) != repeats:
            raise ProjectionContractError(f"case {descriptor.name!r} repeat count is incomplete")
        projection_payloads: set[str] = set()
        oracle_payloads: set[str] = set()
        for expected_repeat, raw_record in enumerate(records):
            record = require_mapping(raw_record, f"case {descriptor.name} repeat")
            nonce = record.get("run_nonce")
            if not isinstance(nonce, str) or _NONCE.fullmatch(nonce) is None:
                raise ProjectionContractError("controller contains malformed run nonce")
            if nonce in all_nonces:
                raise ProjectionContractError("controller run nonce is reused")
            all_nonces.add(nonce)
            validated = validate_probe_record(record, descriptor, expected_repeat, nonce)
            projection_payloads.add(validated.record["direct_projection"])
            oracle_payloads.add(validated.record["native_graph_oracle"])
        if len(projection_payloads) != 1 or len(oracle_payloads) != 1:
            raise ProjectionContractError(
                f"case {descriptor.name!r} projection/oracle changed across fresh processes"
            )
        first_projection_by_case[descriptor.name] = next(iter(projection_payloads))
    if first_projection_by_case[CASE_ORDER[0]] == first_projection_by_case[CASE_ORDER[1]]:
        raise ProjectionContractError("chain and diamond projection payloads are equal")
    return result


def require_regular_owned_file(path: Path, where: str) -> os.stat_result:
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise ProjectionContractError(f"{where} is missing: {path}") from exc
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        raise ProjectionContractError(f"{where} must be one regular, non-linked file")
    if hasattr(os, "geteuid") and metadata.st_uid != os.geteuid():
        raise ProjectionContractError(f"{where} is not owned by the current user")
    return metadata


def write_json_exclusive(path: Path, value: Any) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            descriptor = -1
            json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def publish_atomic(temporary: Path, final: Path, where: str) -> None:
    if temporary.parent != final.parent:
        raise ProjectionContractError(f"{where} temp and final must share one directory")
    require_regular_owned_file(temporary, f"{where} temporary")
    if final.exists() or final.is_symlink():
        raise ProjectionContractError(f"{where} final already exists: {final}")
    os.replace(temporary, final)
    require_regular_owned_file(final, f"{where} final")
