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
"""Strict manifest, result-schema, fingerprint, and atomic publication contracts."""

import hashlib
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
    sha256_file,
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
        "input_case_sha256",
        "node_count",
        "arc_count",
        "direct_projection",
        "decoded_projection",
        "native_graph_oracle",
        "independent_fifo_oracle",
        "projection_sha256",
        "oracle_sha256",
        "database_before_sha256",
        "database_after_sha256",
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
_SHA256 = re.compile(r"[0-9a-f]{64}")
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
    input_hashes: tuple[tuple[str, str], ...]
    input_paths: tuple[tuple[str, Path], ...]
    input_case_sha256: str

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


class CanonicalWriter:
    """Small independent writer used only for the input-case descriptor fingerprint."""

    def __init__(self) -> None:
        self._bytes = bytearray()

    def append_u64(self, value: int) -> None:
        if type(value) is not int or value < 0 or value >= 1 << 64:
            raise ProjectionContractError(f"non-canonical U64 value: {value!r}")
        self._bytes.extend(value.to_bytes(8, "big", signed=False))

    def append_string(self, value: str) -> None:
        if not isinstance(value, str):
            raise ProjectionContractError("canonical string value must be a string")
        encoded = value.encode("utf-8", errors="strict")
        self.append_u64(len(encoded))
        self._bytes.extend(encoded)

    def append_domain(self, domain: str, version: str = "1.0.0") -> None:
        self.append_string("ista-csr")
        self.append_string(domain)
        self.append_string(version)

    def bytes(self) -> bytes:
        return bytes(self._bytes)


def _canonical_string_bytes(value: str) -> bytes:
    writer = CanonicalWriter()
    writer.append_string(value)
    return writer.bytes()


def compute_case_fingerprint(
    *,
    name: str,
    top: str,
    verilog: str,
    sdc: str,
    expected_clock: str,
    input_hashes: tuple[tuple[str, str], ...],
) -> str:
    if len(input_hashes) != 5 or len({path for path, _digest in input_hashes}) != 5:
        raise ProjectionContractError("input case fingerprint requires exactly five unique inputs")
    writer = CanonicalWriter()
    writer.append_domain("ista-csr-projection-input-case")
    for value in (name, top, verilog, sdc, expected_clock):
        if not value:
            raise ProjectionContractError("input case descriptor strings must be non-empty")
        writer.append_string(value)
    for path, digest in sorted(input_hashes, key=lambda item: _canonical_string_bytes(item[0])):
        if _SHA256.fullmatch(digest) is None:
            raise ProjectionContractError(f"invalid recomputed SHA-256 for {path!r}")
        writer.append_string(path)
        writer.append_string(digest)
    return hashlib.sha256(writer.bytes()).hexdigest()


def load_case_descriptors(manifest_path: Path) -> tuple[CaseDescriptor, CaseDescriptor]:
    manifest_path = manifest_path.resolve()
    manifest = require_mapping(load_json(manifest_path), "manifest")
    validate_manifest(manifest, manifest_path, allow_unpinned_topology=False)
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
        declared = require_mapping(case["input_sha256"], f"case {name}.input_sha256")
        expected_inputs = {*_COMMON_INPUTS, case["verilog"], case["sdc"]}
        if len(expected_inputs) != 5 or set(declared) != expected_inputs:
            raise ProjectionContractError(f"case {name!r} must name exactly five distinct inputs")
        recomputed: list[tuple[str, str]] = []
        resolved: list[tuple[str, Path]] = []
        for relative in expected_inputs:
            path = (manifest_path.parent / relative).resolve()
            try:
                path.relative_to(manifest_path.parent)
            except ValueError as exc:
                raise ProjectionContractError(
                    f"case input escapes baseline directory: {relative}"
                ) from exc
            digest = sha256_file(path)
            if digest != declared[relative]:
                raise ProjectionContractError(
                    f"case {name!r} input hash changed for {relative!r}: "
                    f"expected {declared[relative]}, got {digest}"
                )
            recomputed.append((relative, digest))
            resolved.append((relative, path))
        input_hashes = tuple(sorted(recomputed, key=lambda item: _canonical_string_bytes(item[0])))
        fingerprint = compute_case_fingerprint(
            name=name,
            top=case["top"],
            verilog=case["verilog"],
            sdc=case["sdc"],
            expected_clock=case["expected_clock"],
            input_hashes=input_hashes,
        )
        descriptors.append(
            CaseDescriptor(
                name=name,
                top=case["top"],
                verilog=case["verilog"],
                sdc=case["sdc"],
                expected_clock=case["expected_clock"],
                input_hashes=input_hashes,
                input_paths=tuple(resolved),
                input_case_sha256=fingerprint,
            )
        )
    if descriptors[0].input_case_sha256 == descriptors[1].input_case_sha256:
        raise ProjectionContractError("chain and diamond input-case fingerprints must differ")
    return descriptors[0], descriptors[1]


def decode_lower_hex(value: Any, where: str) -> bytes:
    if not isinstance(value, str) or _LOWER_HEX.fullmatch(value) is None:
        raise ProjectionContractError(f"{where} must be non-empty lowercase even-length hex")
    return bytes.fromhex(value)


def _require_sha(value: Any, where: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise ProjectionContractError(f"{where} must be a lowercase SHA-256")
    return value


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
        "input_case_sha256": descriptor.input_case_sha256,
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
    projection_sha = _require_sha(record["projection_sha256"], "probe.projection_sha256")
    oracle_sha = _require_sha(record["oracle_sha256"], "probe.oracle_sha256")
    before_sha = _require_sha(record["database_before_sha256"], "probe.database_before_sha256")
    after_sha = _require_sha(record["database_after_sha256"], "probe.database_after_sha256")
    _require_sha(record["input_case_sha256"], "probe.input_case_sha256")
    actual_projection_sha = hashlib.sha256(direct).hexdigest()
    actual_oracle_sha = hashlib.sha256(native_oracle).hexdigest()
    if projection_sha != actual_projection_sha:
        raise ProjectionContractError("probe projection SHA-256 does not match bytes")
    if oracle_sha != actual_oracle_sha:
        raise ProjectionContractError("probe oracle SHA-256 does not match bytes")
    if before_sha != projection_sha or after_sha != projection_sha:
        raise ProjectionContractError("database before/after SHA-256 must equal projection SHA-256")
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
        projection_hashes: set[str] = set()
        oracle_hashes: set[str] = set()
        for expected_repeat, raw_record in enumerate(records):
            record = require_mapping(raw_record, f"case {descriptor.name} repeat")
            nonce = record.get("run_nonce")
            if not isinstance(nonce, str) or _NONCE.fullmatch(nonce) is None:
                raise ProjectionContractError("controller contains malformed run nonce")
            if nonce in all_nonces:
                raise ProjectionContractError("controller run nonce is reused")
            all_nonces.add(nonce)
            validated = validate_probe_record(record, descriptor, expected_repeat, nonce)
            projection_hashes.add(validated.record["projection_sha256"])
            oracle_hashes.add(validated.record["oracle_sha256"])
        if len(projection_hashes) != 1 or len(oracle_hashes) != 1:
            raise ProjectionContractError(
                f"case {descriptor.name!r} projection/oracle changed across fresh processes"
            )
        first_projection_by_case[descriptor.name] = next(iter(projection_hashes))
    if descriptors[0].input_case_sha256 == descriptors[1].input_case_sha256:
        raise ProjectionContractError("chain and diamond case fingerprints are equal")
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
