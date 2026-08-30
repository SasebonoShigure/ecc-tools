#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# ***************************************************************************************
"""Typed descriptors and strict manifest loading for Step 8."""

import hashlib
import json
import math
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def _bootstrap_sibling_paths():
    test_root = Path(__file__).resolve().parent.parent
    baseline_root = test_root / "baseline"
    reference_root = test_root / "reference_input"
    for root in (baseline_root, reference_root):
        if not root.is_dir() or root.resolve(strict=True) != root:
            raise ImportError(f"Step8 sibling module root is not canonical: {root}")
        if str(root) in sys.path:
            raise ImportError(f"Step8 sibling module root already exists in sys.path: {root}")
        sys.path.insert(0, str(root))
    import baseline_contract as baseline
    from reference_input_bootstrap import load_case_descriptors

    return test_root, baseline_root, reference_root, baseline, load_case_descriptors


(
    _TEST_ROOT,
    _BASELINE_ROOT,
    _REFERENCE_ROOT,
    _baseline,
    load_case_descriptors,
) = _bootstrap_sibling_paths()
BaselineError = _baseline.BaselineError
sha256_bytes = _baseline.sha256_bytes

MANIFEST_SCHEMA = "ista-csr-real-differential-manifest/1.0.0"
MANIFEST_DOMAIN = "ista-csr-real-differential-input-case/1.0.0"
PROBE_SCHEMA = "ista-csr-real-differential-probe/1.0.0"
CONTROLLER_SCHEMA = "ista-csr-real-differential-controller/1.0.0"
CASE_ORDER = ("chain", "diamond", "supported_branches")
LANE_ORDER = ("oracle", "reference")
INPUT_ORDER = ("tech_lef", "cell_lef", "liberty", "verilog", "sdc")
FINGERPRINT_KEYS = (
    "topology_fingerprint",
    "clock_constraint_fingerprint",
    "library_fingerprint",
    "corner_fingerprint",
    "parasitic_fingerprint",
    "timing_input_fingerprint",
)
CHECKPOINTS = (
    "pre_poison",
    "post_poison",
    "post_scatter",
    "post_canary",
    "post_rerun",
)
POISON_FAMILIES = (
    "timing_point_level",
    "timing_point_is_clock_point",
    "timing_point_scalars",
    "timing_point_clock_arrival_maps",
    "timing_point_clock_slew_maps",
    "timing_point_clock_predecessor_maps",
    "timing_point_clock_predecessor_arc_delay_maps",
    "timing_point_clock_predecessor_transition_maps",
    "timing_point_data_slew_map",
    "timing_point_path_state_map",
    "arc_scalars",
    "arc_transition_maps",
    "arc_input_output_delay_map",
    "arc_graph_delay_map",
    "arc_timing_arc_delay_map",
    "arc_timing_arc_fallback_provenance",
    "database_timing_order",
)
NATIVE_BACKED = (
    "chain",
    "diamond",
    "non_unate",
    "parallel",
    "disabled",
    "loop_disabled",
    "multi_root",
    "clock_tag",
    "near_tie",
    "post_scatter_rerun",
)
SYNTHETIC_ONLY = (
    "parasitic_net",
    "sequential_instance",
    "sequential_launch",
    "timing_check",
    "crpr",
    "propagated_clock",
    "clock_tree_fanout",
    "clock_gating_or_divider",
    "conditional_timing_arc",
    "case_analysis",
)
NATIVE_FATAL = ("clock_tree_overlap", "multi_driver")
NOT_CLAIMED = (
    "sequential_native_parity",
    "crpr_native_parity",
    "mutation",
    "generation_gt_zero",
    "cuda",
    "dual_csr",
    "speedup",
)
_MANIFEST_KEYS = {
    "schema",
    "domain",
    "baseline_manifest",
    "baseline_manifest_sha256",
    "tolerances",
    "extended_cases",
    "coverage_claims",
}
_EXTENDED_KEYS = {
    "name",
    "top",
    "expected_clocks",
    "inputs",
    "non_unate_anchor",
    "disabled_anchor",
    "parallel_anchor",
    "loop_disabled_witness",
    "data_roots",
    "clock_tags",
    "near_tie",
}
_ARC_FIELDS = (
    "instance",
    "cell",
    "source_pin",
    "sink_pin",
    "owner_name",
    "library_source_port",
    "library_sink_port",
)
_NODE_FIELDS = ("kind", "full_name", "instance_name", "pin_name")
_NEAR_KEYS = {
    "join_node",
    "row_first_arc",
    "row_second_arc",
    "analysis",
    "path_source",
    "transition",
    "clock_tag",
    "secondary_root",
    "secondary_clock_tag",
    "lower_exclusive",
    "upper_exclusive",
}
_SHA = re.compile(r"[0-9a-f]{64}")
_BASELINE_SHA = "c2cf7aac05719ba7b437b45918cc11ade5fec68f2727135facaa46b7b4f4b47b"
_EXTENDED_PATHS = tuple(
    [
        "../baseline/fixtures/stage0_tech.lef",
        "fixtures/supported_branches_cells.lef",
        "fixtures/supported_branches.lib",
        "fixtures/supported_branches.v",
        "fixtures/supported_branches.sdc",
    ]
)
_EXTENDED_CASE_SHA = "acef79a9df5ce938e5400189935a76fe64c1dad88ae9d9b0a5f89d8bbe25c276"


class DifferentialModelError(BaselineError):
    """The Step 8 manifest or descriptor is not canonical."""


@dataclass(frozen=True)
class InputArtifact:
    label: str
    manifest_path: str
    path: Path
    sha256: str


@dataclass(frozen=True)
class ArcSelector:
    instance: str
    cell: str
    source_pin: str
    sink_pin: str
    owner_name: str
    library_source_port: str
    library_sink_port: str

    def values(self) -> tuple[str, ...]:
        return tuple(getattr(self, key) for key in _ARC_FIELDS)


@dataclass(frozen=True)
class NodeSelector:
    kind: str
    full_name: str
    instance_name: str
    pin_name: str

    def values(self) -> tuple[str, ...]:
        return self.kind, self.full_name, self.instance_name, self.pin_name


@dataclass(frozen=True)
class LoopWitness:
    source: str
    sink: str
    owner_name: str
    minimum_count: int


@dataclass(frozen=True)
class NearTie:
    join_node: NodeSelector
    row_first_arc: ArcSelector
    row_second_arc: ArcSelector
    analysis: str
    path_source: str
    transition: str
    clock_tag: str
    secondary_root: NodeSelector
    secondary_clock_tag: str
    lower_exclusive: float
    upper_exclusive: float


@dataclass(frozen=True)
class CaseDescriptor:
    name: str
    top: str
    expected_clocks: tuple[str, ...]
    inputs: tuple[InputArtifact, ...]
    input_case_sha256: str
    descriptor_manifest: Path
    descriptor_manifest_sha256: str
    raw_descriptor: dict[str, Any] | None = None
    non_unate_anchor: ArcSelector | None = None
    disabled_anchor: ArcSelector | None = None
    parallel_anchor: ArcSelector | None = None
    loop_disabled_witness: LoopWitness | None = None
    data_roots: tuple[NodeSelector, ...] = ()
    clock_tags: tuple[str, ...] = ()
    near_tie: NearTie | None = None

    def input(self, label: str) -> InputArtifact:
        matches = tuple(item for item in self.inputs if item.label == label)
        if len(matches) != 1:
            raise DifferentialModelError(f"case {self.name!r} lacks unique input {label!r}")
        return matches[0]

    @property
    def is_extended(self) -> bool:
        return self.raw_descriptor is not None


@dataclass(frozen=True)
class ManifestBundle:
    path: Path
    sha256: str
    baseline_manifest: Path
    cases: tuple[CaseDescriptor, ...]
    absolute_tolerance: float
    relative_tolerance: float
    coverage_claims: dict[str, tuple[str, ...]]


@dataclass(frozen=True)
class LaneSlot:
    role: str
    run_nonce: str
    directory: Path
    temporary_result: Path
    final_result: Path


@dataclass(frozen=True)
class PairSlot:
    ordinal: int
    descriptor: CaseDescriptor
    repeat_index: int
    directory: Path
    oracle: LaneSlot
    reference: LaneSlot

    def lanes(self) -> tuple[LaneSlot, LaneSlot]:
        return self.oracle, self.reference


@dataclass(frozen=True)
class ControllerRun:
    directory: Path
    summary_path: Path
    summary: dict[str, Any]


class _Writer:
    def __init__(self) -> None:
        self.data = bytearray()

    def u64(self, value: int) -> None:
        if type(value) is not int or not 0 <= value < 1 << 64:
            raise DifferentialModelError(f"non-canonical U64: {value!r}")
        self.data.extend(value.to_bytes(8, "big"))

    def string(self, value: str) -> None:
        if not isinstance(value, str):
            raise DifferentialModelError("canonical value must be a string")
        encoded = value.encode("utf-8", errors="strict")
        self.u64(len(encoded))
        self.data.extend(encoded)

    def vector(self, values: tuple[str, ...]) -> None:
        self.u64(len(values))
        for value in values:
            self.string(value)


def _mapping(value: Any, where: str) -> dict[str, Any]:
    try:
        return _baseline.require_mapping(value, where)
    except BaselineError as exc:
        raise DifferentialModelError(str(exc)) from exc


def _items(value: Any, where: str) -> list[Any]:
    try:
        return _baseline.require_list(value, where)
    except BaselineError as exc:
        raise DifferentialModelError(str(exc)) from exc


def _exact(value: dict[str, Any], keys: set[str], where: str) -> None:
    try:
        _baseline.require_exact_keys(value, keys, where)
    except BaselineError as exc:
        raise DifferentialModelError(str(exc)) from exc


def _text(value: Any, where: str, *, empty: bool = False) -> str:
    if not isinstance(value, str) or (not empty and not value):
        raise DifferentialModelError(f"{where} must be a string")
    value.encode("utf-8", errors="strict")
    return value


def _sha(value: Any, where: str) -> str:
    if not isinstance(value, str) or _SHA.fullmatch(value) is None:
        raise DifferentialModelError(f"{where} must be a lowercase SHA-256")
    return value


def _number(value: Any, where: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise DifferentialModelError(f"{where} must be numeric")
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise DifferentialModelError(f"{where} must be finite and positive")
    return result


def _arc(value: Any, where: str) -> ArcSelector:
    record = _mapping(value, where)
    _exact(record, set(_ARC_FIELDS), where)
    return ArcSelector(*(_text(record[key], f"{where}.{key}") for key in _ARC_FIELDS))


def _node(value: Any, where: str) -> NodeSelector:
    record = _mapping(value, where)
    _exact(record, set(_NODE_FIELDS), where)
    kind = _text(record["kind"], f"{where}.kind")
    full = _text(record["full_name"], f"{where}.full_name")
    instance = _text(record["instance_name"], f"{where}.instance_name", empty=True)
    pin = _text(record["pin_name"], f"{where}.pin_name", empty=True)
    if kind == "port" and (instance or pin):
        raise DifferentialModelError(f"{where} port has instance fields")
    if kind == "instance_pin" and (not instance or not pin or full != f"{instance}:{pin}"):
        raise DifferentialModelError(f"{where} instance pin is non-canonical")
    if kind not in {"port", "instance_pin"}:
        raise DifferentialModelError(f"{where}.kind is unsupported")
    return NodeSelector(kind, full, instance, pin)


def _near(value: Any) -> NearTie:
    record = _mapping(value, "extended.near_tie")
    _exact(record, _NEAR_KEYS, "extended.near_tie")
    lower = _number(record["lower_exclusive"], "near_tie.lower_exclusive")
    upper = _number(record["upper_exclusive"], "near_tie.upper_exclusive")
    if (
        struct.pack(">d", lower).hex() != "3e112e0be826d695"
        or struct.pack(">d", upper).hex() != "3eb0c6f7a0b5ed8d"
        or lower >= upper
    ):
        raise DifferentialModelError("near-tie binary64 bounds changed")
    return NearTie(
        _node(record["join_node"], "near_tie.join_node"),
        _arc(record["row_first_arc"], "near_tie.row_first_arc"),
        _arc(record["row_second_arc"], "near_tie.row_second_arc"),
        _text(record["analysis"], "near_tie.analysis"),
        _text(record["path_source"], "near_tie.path_source"),
        _text(record["transition"], "near_tie.transition"),
        _text(record["clock_tag"], "near_tie.clock_tag"),
        _node(record["secondary_root"], "near_tie.secondary_root"),
        _text(record["secondary_clock_tag"], "near_tie.secondary_clock_tag"),
        lower,
        upper,
    )


def _input_path(base: Path, label: str, relative: str, baseline_tech: Path) -> Path:
    if label == "tech_lef":
        if relative != "../baseline/fixtures/stage0_tech.lef":
            raise DifferentialModelError("extended tech_lef path bytes changed")
        resolved = (base / relative).resolve()
        if resolved != baseline_tech or resolved.is_symlink():
            raise DifferentialModelError("extended tech_lef is not the frozen baseline")
        return resolved
    raw_parts = relative.split("/")
    if not raw_parts or any(part in {"", ".", ".."} for part in raw_parts):
        raise DifferentialModelError(f"extended {label} path is non-canonical")
    candidate = Path(*raw_parts)
    if candidate.is_absolute():
        raise DifferentialModelError(f"extended {label} path is non-canonical")
    cursor = base
    for part in candidate.parts:
        cursor /= part
        if cursor.is_symlink():
            raise DifferentialModelError(f"extended {label} traverses a symlink")
    resolved = cursor.resolve()
    try:
        resolved.relative_to(base.resolve())
    except ValueError as exc:
        raise DifferentialModelError(f"extended {label} escapes manifest tree") from exc
    return resolved


def _fingerprint(descriptor: CaseDescriptor) -> str:
    near = descriptor.near_tie
    loop = descriptor.loop_disabled_witness
    anchors = (
        descriptor.non_unate_anchor,
        descriptor.disabled_anchor,
        descriptor.parallel_anchor,
    )
    if near is None or loop is None or any(anchor is None for anchor in anchors):
        raise DifferentialModelError("extended fingerprint inputs are incomplete")
    writer = _Writer()
    for value in ("ista-csr", "real-differential-input-case", "1.0.0"):
        writer.string(value)
    writer.string(descriptor.name)
    writer.string(descriptor.top)
    writer.vector(descriptor.expected_clocks)
    for artifact in descriptor.inputs:
        for value in (artifact.label, artifact.manifest_path, artifact.sha256):
            writer.string(value)
    for anchor in anchors:
        assert anchor is not None
        for value in anchor.values():
            writer.string(value)
    for value in (loop.source, loop.sink, loop.owner_name):
        writer.string(value)
    writer.u64(loop.minimum_count)
    writer.u64(len(descriptor.data_roots))
    for root in descriptor.data_roots:
        for value in root.values():
            writer.string(value)
    writer.vector(descriptor.clock_tags)
    for value in near.join_node.values():
        writer.string(value)
    for arc in (near.row_first_arc, near.row_second_arc):
        for value in arc.values():
            writer.string(value)
    for value in (
        near.analysis,
        near.path_source,
        near.transition,
        near.clock_tag,
        *near.secondary_root.values(),
        near.secondary_clock_tag,
        struct.pack(">d", near.lower_exclusive).hex(),
        struct.pack(">d", near.upper_exclusive).hex(),
    ):
        writer.string(value)
    return hashlib.sha256(writer.data).hexdigest()


def canonical_clock_json(clocks: tuple[str, ...]) -> str:
    return json.dumps(list(clocks), ensure_ascii=False, separators=(",", ":"), allow_nan=False)


def _extended(
    manifest: dict[str, Any],
    manifest_path: Path,
    manifest_sha: str,
    baseline_tech: Path,
) -> CaseDescriptor:
    cases = _items(manifest["extended_cases"], "manifest.extended_cases")
    if len(cases) != 1:
        raise DifferentialModelError("extended_cases must contain exactly one case")
    raw = _mapping(cases[0], "manifest.extended_cases[0]")
    _exact(raw, _EXTENDED_KEYS, "manifest.extended_cases[0]")
    if (raw["name"], raw["top"]) != ("supported_branches", "supported_branches"):
        raise DifferentialModelError("extended name/top changed")
    clocks = tuple(
        _text(item, "extended.expected_clocks[]")
        for item in _items(raw["expected_clocks"], "extended.expected_clocks")
    )
    if clocks != ("vclk_a", "vclk_b"):
        raise DifferentialModelError("extended clocks must be two byte-sorted strings")
    input_map = _mapping(raw["inputs"], "extended.inputs")
    _exact(input_map, set(INPUT_ORDER), "extended.inputs")
    artifacts = []
    for label in INPUT_ORDER:
        item = _mapping(input_map[label], f"extended.inputs.{label}")
        _exact(item, {"path", "sha256"}, f"extended.inputs.{label}")
        relative = _text(item["path"], f"extended.inputs.{label}.path")
        declared = _sha(item["sha256"], f"extended.inputs.{label}.sha256")
        path = _input_path(manifest_path.parent, label, relative, baseline_tech)
        if not path.is_file() or path.is_symlink() or _baseline.sha256_file(path) != declared:
            raise DifferentialModelError(f"extended {label} file/SHA differs")
        artifacts.append(InputArtifact(label, relative, path, declared))
    if tuple(item.manifest_path for item in artifacts) != _EXTENDED_PATHS:
        raise DifferentialModelError("extended input path bytes changed")
    anchors = tuple(
        _arc(raw[key], f"extended.{key}")
        for key in ("non_unate_anchor", "disabled_anchor", "parallel_anchor")
    )
    frozen_anchors = (
        ("u_nonunate", "NONUNATE2_X1", "A", "Y", "u_nonunate", "A", "Y"),
        ("u_disabled", "DISABLE_X1", "A", "Y", "u_disabled", "A", "Y"),
        ("u_parallel", "PARALLEL_X1", "A", "Y", "u_parallel", "A", "Y"),
    )
    if tuple(anchor.values() for anchor in anchors) != frozen_anchors:
        raise DifferentialModelError("extended branch anchor selectors changed")
    loop_raw = _mapping(raw["loop_disabled_witness"], "extended.loop")
    _exact(loop_raw, {"source", "sink", "owner_name", "minimum_count"}, "extended.loop")
    minimum = loop_raw["minimum_count"]
    if type(minimum) is not int or minimum <= 0:
        raise DifferentialModelError("loop minimum_count must be positive")
    loop = LoopWitness(
        _text(loop_raw["source"], "extended.loop.source"),
        _text(loop_raw["sink"], "extended.loop.sink"),
        _text(loop_raw["owner_name"], "extended.loop.owner"),
        minimum,
    )
    if loop != LoopWitness("u_loop_b:Y", "u_loop_a:A", "loop_b", 1):
        raise DifferentialModelError("loop-disabled witness changed")
    roots = tuple(
        _node(item, "extended.data_roots[]")
        for item in _items(raw["data_roots"], "extended.data_roots")
    )
    tags = tuple(
        _text(item, "extended.clock_tags[]")
        for item in _items(raw["clock_tags"], "extended.clock_tags")
    )
    near = _near(raw["near_tie"])
    if (
        len(roots) != 2
        or len(set(roots)) != 2
        or tags != clocks
        or near.analysis != "max"
        or near.path_source != "input"
        or near.transition != "rise"
        or near.clock_tag != tags[0]
        or near.secondary_root not in roots
        or near.secondary_root != roots[1]
        or near.secondary_clock_tag != tags[1]
    ):
        raise DifferentialModelError("extended root/tag contract differs")
    draft = CaseDescriptor(
        raw["name"],
        raw["top"],
        clocks,
        tuple(artifacts),
        "",
        manifest_path,
        manifest_sha,
        raw,
        anchors[0],
        anchors[1],
        anchors[2],
        loop,
        roots,
        tags,
        near,
    )
    fingerprint = _fingerprint(draft)
    if fingerprint != _EXTENDED_CASE_SHA:
        raise DifferentialModelError("extended canonical input-case SHA changed")
    return CaseDescriptor(**{**draft.__dict__, "input_case_sha256": fingerprint})


def _baseline_cases(path: Path) -> tuple[CaseDescriptor, CaseDescriptor]:
    result = []
    manifest_sha = _baseline.sha256_file(path)
    for source in load_case_descriptors(path):
        paths = {
            "tech_lef": source.tech_lef,
            "cell_lef": source.cell_lef,
            "liberty": source.liberty,
            "verilog": source.verilog_path,
            "sdc": source.sdc_path,
        }
        source_paths = {resolved: relative for relative, resolved in source.input_paths}
        hashes = dict(source.input_hashes)
        inputs = tuple(
            InputArtifact(
                label,
                source_paths[paths[label]],
                paths[label],
                hashes[source_paths[paths[label]]],
            )
            for label in INPUT_ORDER
        )
        if source.expected_clock != "vclk":
            raise DifferentialModelError("baseline expected clock changed")
        result.append(
            CaseDescriptor(
                source.name,
                source.top,
                ("vclk",),
                inputs,
                source.input_case_sha256,
                path,
                manifest_sha,
            )
        )
    return result[0], result[1]


def load_manifest(manifest_path: Path, baseline_path: Path) -> ManifestBundle:
    manifest_path = manifest_path.resolve()
    baseline_path = baseline_path.resolve()
    manifest = _mapping(_baseline.load_json(manifest_path), "manifest")
    _exact(manifest, _MANIFEST_KEYS, "manifest")
    if manifest["schema"] != MANIFEST_SCHEMA or manifest["domain"] != MANIFEST_DOMAIN:
        raise DifferentialModelError("manifest schema/domain changed")
    if manifest["baseline_manifest"] != "../baseline/manifest.json":
        raise DifferentialModelError("baseline manifest path bytes changed")
    baseline_sha = _sha(manifest["baseline_manifest_sha256"], "manifest.baseline_manifest_sha256")
    if (
        baseline_sha != _BASELINE_SHA
        or (manifest_path.parent / manifest["baseline_manifest"]).resolve() != baseline_path
        or _baseline.sha256_file(baseline_path) != baseline_sha
    ):
        raise DifferentialModelError("frozen baseline manifest path/SHA changed")
    tolerances = _mapping(manifest["tolerances"], "manifest.tolerances")
    _exact(tolerances, {"absolute", "relative"}, "manifest.tolerances")
    absolute = _number(tolerances["absolute"], "manifest.tolerances.absolute")
    relative = _number(tolerances["relative"], "manifest.tolerances.relative")
    if absolute != 1e-9 or relative != 1e-9:
        raise DifferentialModelError("manifest tolerances changed")
    claims = _mapping(manifest["coverage_claims"], "manifest.coverage_claims")
    expected = {
        "native_backed": NATIVE_BACKED,
        "synthetic_only": SYNTHETIC_ONLY,
        "native_fatal": NATIVE_FATAL,
        "not_claimed": NOT_CLAIMED,
    }
    _exact(claims, set(expected), "manifest.coverage_claims")
    normalized = {}
    for key, frozen in expected.items():
        actual = tuple(
            _text(item, f"coverage.{key}[]") for item in _items(claims[key], f"coverage.{key}")
        )
        if actual != frozen:
            raise DifferentialModelError(f"coverage claim {key} changed")
        normalized[key] = actual
    baseline = _baseline_cases(baseline_path)
    extended = _extended(
        manifest,
        manifest_path,
        _baseline.sha256_file(manifest_path),
        baseline[0].input("tech_lef").path,
    )
    cases = (*baseline, extended)
    if tuple(item.name for item in cases) != CASE_ORDER:
        raise DifferentialModelError("case order changed")
    return ManifestBundle(
        manifest_path,
        _baseline.sha256_file(manifest_path),
        baseline_path,
        cases,
        absolute,
        relative,
        normalized,
    )
