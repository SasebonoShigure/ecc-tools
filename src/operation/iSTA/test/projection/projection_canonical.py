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
"""Independent strict decoder for Stage 2 native-graph canonical bytes."""

from collections.abc import Callable
from dataclasses import dataclass
from typing import TypeVar

_MAX_VECTOR_ITEMS = 1_000_000
_MAX_BLOB_BYTES = 256 * 1024 * 1024
_PROJECTION_DOMAIN = "native-graph-projection"
_PROJECTION_SCHEMA = "ista-csr-native-graph-projection/1.0.0"
_ORACLE_DOMAIN = "native-graph-oracle"
_ORACLE_SCHEMA = "ista-csr-native-graph-oracle/1.0.0"
_VERSION = "1.0.0"
_NODE_KINDS = frozenset({"port", "instance_pin"})
_ARC_KINDS = frozenset({"cell", "net"})
_DIRECTIONS = frozenset({"none", "input", "output", "inout"})
_SENSES = frozenset({"none_positive_fallback", "positive_unate", "negative_unate", "non_unate"})
_TRANSITIONS = frozenset({"rise", "fall", "none"})
_TABLE_VARIABLES = frozenset(
    {"none", "input_transition", "output_capacitance", "constrained_transition"}
)


class CanonicalDecodeError(RuntimeError):
    """Canonical bytes are malformed, non-canonical, or semantically inconsistent."""


T = TypeVar("T")


class CanonicalReader:
    """Bounds-checked big-endian reader independent from the production encoder."""

    def __init__(self, payload: bytes, where: str) -> None:
        if not isinstance(payload, bytes):
            raise CanonicalDecodeError(f"{where} payload must be bytes")
        if len(payload) > _MAX_BLOB_BYTES:
            raise CanonicalDecodeError(f"{where} payload exceeds the decoder limit")
        self._payload = payload
        self._offset = 0
        self.where = where

    def _take(self, size: int) -> bytes:
        if size < 0 or size > len(self._payload) - self._offset:
            raise CanonicalDecodeError(
                f"{self.where} is truncated at byte {self._offset}; requested {size}"
            )
        result = self._payload[self._offset : self._offset + size]
        self._offset += size
        return result

    def u8(self) -> int:
        return self._take(1)[0]

    def boolean(self) -> bool:
        value = self.u8()
        if value not in (0, 1):
            raise CanonicalDecodeError(f"{self.where} has non-canonical bool {value}")
        return value == 1

    def u32(self) -> int:
        return int.from_bytes(self._take(4), "big", signed=False)

    def i32(self) -> int:
        return int.from_bytes(self._take(4), "big", signed=True)

    def u64(self) -> int:
        return int.from_bytes(self._take(8), "big", signed=False)

    def bytes(self) -> bytes:
        size = self.u64()
        if size > _MAX_BLOB_BYTES:
            raise CanonicalDecodeError(f"{self.where} length {size} exceeds the decoder limit")
        return self._take(size)

    def string(self) -> str:
        raw = self.bytes()
        try:
            value = raw.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise CanonicalDecodeError(f"{self.where} has invalid UTF-8: {exc}") from exc
        if value.encode("utf-8") != raw:
            raise CanonicalDecodeError(f"{self.where} has non-canonical UTF-8")
        return value

    def token(self, allowed: frozenset[str], field: str) -> str:
        value = self.string()
        if value not in allowed:
            raise CanonicalDecodeError(f"{self.where}.{field} has unknown token {value!r}")
        return value

    def vector(self, read_item: Callable[[int], T], field: str) -> tuple[T, ...]:
        count = self.u64()
        if count > _MAX_VECTOR_ITEMS:
            raise CanonicalDecodeError(f"{self.where}.{field} count {count} exceeds limit")
        return tuple(read_item(index) for index in range(count))

    def domain(self, expected_domain: str, expected_version: str = _VERSION) -> None:
        actual = (self.string(), self.string(), self.string())
        expected = ("ista-csr", expected_domain, expected_version)
        if actual != expected:
            raise CanonicalDecodeError(
                f"{self.where} domain/version mismatch: expected {expected!r}, got {actual!r}"
            )

    def finish(self) -> None:
        if self._offset != len(self._payload):
            raise CanonicalDecodeError(
                f"{self.where} has {len(self._payload) - self._offset} trailing bytes"
            )


@dataclass(frozen=True)
class NodeKey:
    kind: str
    full_name: str
    instance_name: str
    pin_name: str


@dataclass(frozen=True)
class TableSignature:
    present: bool
    variable1: str
    variable2: str


@dataclass(frozen=True)
class LibraryArcSignature:
    library_name: str
    arc_index: int
    sense: str
    trigger_transition: str
    check_transition: str
    sdf_condition: str
    delay_tables: tuple[TableSignature, TableSignature]
    slew_tables: tuple[TableSignature, TableSignature]


@dataclass(frozen=True)
class ArcKey:
    source: NodeKey
    sink: NodeKey
    kind: str
    owner_name: str
    library_source_port: str
    library_sink_port: str
    library_arcs: tuple[LibraryArcSignature, ...]
    parallel_ordinal: int


@dataclass(frozen=True)
class ProjectedNode:
    key: NodeKey
    direction: str
    instance_cell: str
    net_name: str
    is_port: bool
    is_startpoint: bool
    is_endpoint: bool
    is_sequential: bool
    is_clock_pin: bool
    is_data_pin: bool
    is_output_pin: bool


@dataclass(frozen=True)
class ProjectedInstance:
    instance_name: str
    cell_name: str
    ordered_pins: tuple[NodeKey, ...]
    is_sequential: bool
    has_timing_checks: bool
    is_clock_gating: bool
    is_macro: bool


@dataclass(frozen=True)
class ProjectedArc:
    key: ArcKey
    native_traversal_rank: int
    is_clock_arc: bool
    is_disabled: bool
    is_loop_disabled: bool


@dataclass(frozen=True)
class AdjacencyRow:
    node: NodeKey
    ordered_arcs: tuple[ArcKey, ...]


@dataclass(frozen=True)
class ProjectionSummary:
    schema: str
    nodes: tuple[ProjectedNode, ...]
    instances: tuple[ProjectedInstance, ...]
    arcs: tuple[ProjectedArc, ...]
    base_out: tuple[AdjacencyRow, ...]
    base_in: tuple[AdjacencyRow, ...]
    ordered_roots: tuple[NodeKey, ...]
    ordered_starts: tuple[NodeKey, ...]
    ordered_ends: tuple[NodeKey, ...]

    @property
    def node_count(self) -> int:
        return len(self.nodes)

    @property
    def arc_count(self) -> int:
        return len(self.arcs)


@dataclass(frozen=True)
class OracleSummary:
    schema: str
    native_timing_order: tuple[NodeKey, ...]
    native_node_levels: tuple[tuple[NodeKey, int], ...]


def _read_node_fields(reader: CanonicalReader) -> NodeKey:
    key = NodeKey(
        kind=reader.token(_NODE_KINDS, "node_kind"),
        full_name=reader.string(),
        instance_name=reader.string(),
        pin_name=reader.string(),
    )
    if not key.full_name:
        raise CanonicalDecodeError(f"{reader.where} node full_name is empty")
    if key.kind == "port":
        if key.instance_name or key.pin_name:
            raise CanonicalDecodeError(f"{reader.where} port key contains instance/pin names")
    elif not key.instance_name or not key.pin_name:
        raise CanonicalDecodeError(f"{reader.where} instance-pin key is incomplete")
    elif key.full_name != f"{key.instance_name}:{key.pin_name}":
        raise CanonicalDecodeError(f"{reader.where} instance-pin full name is inconsistent")
    return key


def _read_embedded_node(reader: CanonicalReader, where: str) -> tuple[NodeKey, bytes]:
    payload = reader.bytes()
    nested = CanonicalReader(payload, where)
    nested.domain("node-key")
    result = _read_node_fields(nested)
    nested.finish()
    return result, payload


def _read_table(reader: CanonicalReader) -> TableSignature:
    result = TableSignature(
        present=reader.boolean(),
        variable1=reader.token(_TABLE_VARIABLES, "table.variable1"),
        variable2=reader.token(_TABLE_VARIABLES, "table.variable2"),
    )
    if not result.present and (result.variable1 != "none" or result.variable2 != "none"):
        raise CanonicalDecodeError(f"{reader.where} absent table carries variables")
    if result.present and result.variable1 == "none" and result.variable2 != "none":
        raise CanonicalDecodeError(
            f"{reader.where} table variable2 cannot be present without variable1"
        )
    return result


def _read_library_arc(reader: CanonicalReader) -> LibraryArcSignature:
    result = LibraryArcSignature(
        library_name=reader.string(),
        arc_index=reader.i32(),
        sense=reader.token(_SENSES, "library_arc.sense"),
        trigger_transition=reader.token(_TRANSITIONS, "library_arc.trigger"),
        check_transition=reader.token(_TRANSITIONS, "library_arc.check"),
        sdf_condition=reader.string(),
        delay_tables=(_read_table(reader), _read_table(reader)),
        slew_tables=(_read_table(reader), _read_table(reader)),
    )
    return result


def _read_arc_fields(reader: CanonicalReader) -> ArcKey:
    source = _read_node_fields(reader)
    sink = _read_node_fields(reader)
    kind = reader.token(_ARC_KINDS, "arc_kind")
    owner_name = reader.string()
    library_source_port = reader.string()
    library_sink_port = reader.string()
    library_arcs = reader.vector(lambda _index: _read_library_arc(reader), "library_arcs")
    ordinal = reader.u64()
    if not owner_name:
        raise CanonicalDecodeError(f"{reader.where} arc owner_name is empty")
    if kind == "net" and (library_source_port or library_sink_port or library_arcs):
        raise CanonicalDecodeError(f"{reader.where} net arc contains library identity")
    if kind == "cell" and (not library_source_port or not library_sink_port):
        raise CanonicalDecodeError(f"{reader.where} cell arc lacks library ports")
    return ArcKey(
        source,
        sink,
        kind,
        owner_name,
        library_source_port,
        library_sink_port,
        library_arcs,
        ordinal,
    )


def _read_embedded_arc(reader: CanonicalReader, where: str) -> tuple[ArcKey, bytes]:
    payload = reader.bytes()
    nested = CanonicalReader(payload, where)
    nested.domain("arc-key")
    result = _read_arc_fields(nested)
    nested.finish()
    return result, payload


def _require_unique(values: tuple[T, ...], where: str) -> None:
    if len(values) != len(set(values)):
        raise CanonicalDecodeError(f"{where} contains duplicates")


def _read_projection_node(reader: CanonicalReader, index: int) -> tuple[ProjectedNode, bytes]:
    key, raw_key = _read_embedded_node(reader, f"projection.nodes[{index}].key")
    node = ProjectedNode(
        key=key,
        direction=reader.token(_DIRECTIONS, "node.direction"),
        instance_cell=reader.string(),
        net_name=reader.string(),
        is_port=reader.boolean(),
        is_startpoint=reader.boolean(),
        is_endpoint=reader.boolean(),
        is_sequential=reader.boolean(),
        is_clock_pin=reader.boolean(),
        is_data_pin=reader.boolean(),
        is_output_pin=reader.boolean(),
    )
    if node.is_port != (key.kind == "port"):
        raise CanonicalDecodeError(f"projection.nodes[{index}] has inconsistent port role")
    if node.is_port and (
        node.instance_cell
        or node.is_sequential
        or node.is_clock_pin
        or node.is_data_pin
        or node.is_output_pin
    ):
        raise CanonicalDecodeError(f"projection.nodes[{index}] gives a port instance-only roles")
    return node, raw_key


def _read_instance(reader: CanonicalReader, index: int) -> ProjectedInstance:
    name = reader.string()
    cell = reader.string()
    pins = reader.vector(
        lambda pin_index: _read_embedded_node(
            reader, f"projection.instances[{index}].pins[{pin_index}]"
        )[0],
        "instance.pins",
    )
    if not name or not cell:
        raise CanonicalDecodeError(f"projection.instances[{index}] has empty identity")
    _require_unique(pins, f"projection.instances[{index}].pins")
    return ProjectedInstance(
        name,
        cell,
        pins,
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
    )


def _read_projection_arc(reader: CanonicalReader, index: int) -> tuple[ProjectedArc, bytes]:
    key, raw_key = _read_embedded_arc(reader, f"projection.arcs[{index}].key")
    return (
        ProjectedArc(
            key,
            reader.u64(),
            reader.boolean(),
            reader.boolean(),
            reader.boolean(),
        ),
        raw_key,
    )


def _read_adjacency(reader: CanonicalReader, family: str, index: int) -> AdjacencyRow:
    node = _read_embedded_node(reader, f"projection.{family}[{index}].node")[0]
    arcs = reader.vector(
        lambda arc_index: _read_embedded_arc(
            reader, f"projection.{family}[{index}].arcs[{arc_index}]"
        )[0],
        f"{family}.arcs",
    )
    _require_unique(arcs, f"projection.{family}[{index}].arcs")
    return AdjacencyRow(node, arcs)


def _arc_base(key: ArcKey) -> tuple[object, ...]:
    return (
        key.source,
        key.sink,
        key.kind,
        key.owner_name,
        key.library_source_port,
        key.library_sink_port,
        key.library_arcs,
    )


def _validate_projection(
    summary: ProjectionSummary, node_raw: tuple[bytes, ...], arc_raw: tuple[bytes, ...]
) -> None:
    nodes = tuple(node.key for node in summary.nodes)
    arcs = tuple(arc.key for arc in summary.arcs)
    _require_unique(nodes, "projection.nodes")
    _require_unique(arcs, "projection.arcs")
    if any(left >= right for left, right in zip(node_raw, node_raw[1:], strict=False)):
        raise CanonicalDecodeError("projection nodes are not in strict canonical-key order")
    if any(left >= right for left, right in zip(arc_raw, arc_raw[1:], strict=False)):
        raise CanonicalDecodeError("projection arcs are not in strict canonical-key order")
    node_set = set(nodes)
    arc_set = set(arcs)
    nodes_by_key = {node.key: node for node in summary.nodes}
    if any(arc.source not in node_set or arc.sink not in node_set for arc in arcs):
        raise CanonicalDecodeError("projection arc refers to an unknown node")
    ranks = tuple(arc.native_traversal_rank for arc in summary.arcs)
    if set(ranks) != set(range(len(arcs))):
        raise CanonicalDecodeError("projection native traversal ranks are not a permutation")
    next_ordinal: dict[tuple[object, ...], int] = {}
    for projected_arc in sorted(summary.arcs, key=lambda arc: arc.native_traversal_rank):
        base = _arc_base(projected_arc.key)
        expected = next_ordinal.get(base, 0)
        if projected_arc.key.parallel_ordinal != expected:
            raise CanonicalDecodeError(
                "projection parallel ordinal disagrees with native traversal order"
            )
        next_ordinal[base] = expected + 1
    for arc in arcs:
        source = nodes_by_key[arc.source]
        sink = nodes_by_key[arc.sink]
        if arc.kind == "cell" and (
            arc.source.kind != "instance_pin"
            or arc.sink.kind != "instance_pin"
            or arc.source.instance_name != arc.sink.instance_name
            or arc.owner_name != arc.source.instance_name
            or arc.library_source_port != arc.source.pin_name
            or arc.library_sink_port != arc.sink.pin_name
        ):
            raise CanonicalDecodeError("projection cell arc endpoint identity is inconsistent")
        if arc.kind == "net" and (
            not source.net_name
            or source.net_name != sink.net_name
            or arc.owner_name != source.net_name
        ):
            raise CanonicalDecodeError("projection net arc endpoint identity is inconsistent")
    instance_names = tuple(instance.instance_name for instance in summary.instances)
    _require_unique(instance_names, "projection.instances")
    if any(
        left.encode("utf-8") >= right.encode("utf-8")
        for left, right in zip(instance_names, instance_names[1:], strict=False)
    ):
        raise CanonicalDecodeError("projection instances are not in native map-name order")
    seen_instance_pins: list[NodeKey] = []
    for instance in summary.instances:
        clock_roles = data_roles = output_roles = 0
        for pin in instance.ordered_pins:
            if (
                pin not in node_set
                or pin.kind != "instance_pin"
                or pin.instance_name != instance.instance_name
            ):
                raise CanonicalDecodeError("projection instance pin is unknown or owned elsewhere")
            seen_instance_pins.append(pin)
            node = nodes_by_key[pin]
            if (
                node.instance_cell != instance.cell_name
                or node.is_sequential != instance.is_sequential
            ):
                raise CanonicalDecodeError(
                    "projection instance pin cell/sequential role disagrees with its instance"
                )
            clock_roles += int(node.is_clock_pin)
            data_roles += int(node.is_data_pin)
            output_roles += int(node.is_output_pin)
        if max(clock_roles, data_roles, output_roles) > 1:
            raise CanonicalDecodeError("projection instance pin roles are not unique")
    expected_instance_pins = tuple(node for node in nodes if node.kind == "instance_pin")
    if set(seen_instance_pins) != set(expected_instance_pins) or len(seen_instance_pins) != len(
        expected_instance_pins
    ):
        raise CanonicalDecodeError(
            "projection instance rows do not cover instance pins exactly once"
        )
    for rows, family, endpoint in (
        (summary.base_out, "base_out", "source"),
        (summary.base_in, "base_in", "sink"),
    ):
        if tuple(row.node for row in rows) != nodes:
            raise CanonicalDecodeError(f"projection {family} rows do not match canonical nodes")
        flattened = tuple(arc for row in rows for arc in row.ordered_arcs)
        if set(flattened) != arc_set or len(flattened) != len(arcs):
            raise CanonicalDecodeError(f"projection {family} does not cover arcs exactly once")
        for row in rows:
            if any(getattr(arc, endpoint) != row.node for arc in row.ordered_arcs):
                raise CanonicalDecodeError(f"projection {family} contains an arc in the wrong row")
    for values, field in (
        (summary.ordered_roots, "ordered_roots"),
        (summary.ordered_starts, "ordered_starts"),
        (summary.ordered_ends, "ordered_ends"),
    ):
        _require_unique(values, f"projection.{field}")
        if not set(values).issubset(node_set):
            raise CanonicalDecodeError(f"projection.{field} refers to an unknown node")
    if set(summary.ordered_starts) != {node.key for node in summary.nodes if node.is_startpoint}:
        raise CanonicalDecodeError("projection ordered_starts disagrees with node roles")
    if set(summary.ordered_ends) != {node.key for node in summary.nodes if node.is_endpoint}:
        raise CanonicalDecodeError("projection ordered_ends disagrees with node roles")
    active_indegree = {node: 0 for node in nodes}
    for projected_arc in summary.arcs:
        if not projected_arc.is_disabled and not projected_arc.is_loop_disabled:
            active_indegree[projected_arc.key.sink] += 1
    expected_roots = tuple(
        sorted(
            (node for node, degree in active_indegree.items() if degree == 0),
            key=lambda node: node.full_name.encode("utf-8"),
        )
    )
    if summary.ordered_roots != expected_roots:
        raise CanonicalDecodeError("projection roots disagree with active indegree-zero map order")


def decode_projection(payload: bytes) -> ProjectionSummary:
    reader = CanonicalReader(payload, "projection")
    reader.domain(_PROJECTION_DOMAIN)
    schema = reader.string()
    if schema != _PROJECTION_SCHEMA:
        raise CanonicalDecodeError(f"projection schema mismatch: {schema!r}")
    raw_nodes = reader.vector(lambda index: _read_projection_node(reader, index), "nodes")
    instances = reader.vector(lambda index: _read_instance(reader, index), "instances")
    raw_arcs = reader.vector(lambda index: _read_projection_arc(reader, index), "arcs")
    base_out = reader.vector(lambda index: _read_adjacency(reader, "base_out", index), "base_out")
    base_in = reader.vector(lambda index: _read_adjacency(reader, "base_in", index), "base_in")
    roots = reader.vector(
        lambda index: _read_embedded_node(reader, f"projection.ordered_roots[{index}]")[0],
        "ordered_roots",
    )
    starts = reader.vector(
        lambda index: _read_embedded_node(reader, f"projection.ordered_starts[{index}]")[0],
        "ordered_starts",
    )
    ends = reader.vector(
        lambda index: _read_embedded_node(reader, f"projection.ordered_ends[{index}]")[0],
        "ordered_ends",
    )
    reader.finish()
    result = ProjectionSummary(
        schema,
        tuple(item[0] for item in raw_nodes),
        instances,
        tuple(item[0] for item in raw_arcs),
        base_out,
        base_in,
        roots,
        starts,
        ends,
    )
    _validate_projection(
        result,
        tuple(item[1] for item in raw_nodes),
        tuple(item[1] for item in raw_arcs),
    )
    return result


def decode_oracle(payload: bytes) -> OracleSummary:
    reader = CanonicalReader(payload, "native_graph_oracle")
    reader.domain(_ORACLE_DOMAIN)
    schema = reader.string()
    if schema != _ORACLE_SCHEMA:
        raise CanonicalDecodeError(f"native graph oracle schema mismatch: {schema!r}")
    order = reader.vector(
        lambda index: _read_embedded_node(reader, f"oracle.native_timing_order[{index}]")[0],
        "native_timing_order",
    )
    raw_levels = reader.vector(
        lambda index: (
            *_read_embedded_node(reader, f"oracle.native_node_levels[{index}].key"),
            reader.i32(),
        ),
        "native_node_levels",
    )
    reader.finish()
    _require_unique(order, "oracle.native_timing_order")
    _require_unique(tuple(item[0] for item in raw_levels), "oracle.native_node_levels")
    raw_keys = tuple(item[1] for item in raw_levels)
    if any(left >= right for left, right in zip(raw_keys, raw_keys[1:], strict=False)):
        raise CanonicalDecodeError("oracle level rows are not in strict canonical-key order")
    levels = tuple((key, level) for key, _raw, level in raw_levels)
    return OracleSummary(schema, order, levels)


def validate_oracle_shape(projection: ProjectionSummary, oracle: OracleSummary) -> None:
    canonical_nodes = tuple(node.key for node in projection.nodes)
    nodes = set(canonical_nodes)
    if not set(oracle.native_timing_order).issubset(nodes):
        raise CanonicalDecodeError("oracle timing order refers to an unknown projection node")
    level_nodes = tuple(node for node, _level in oracle.native_node_levels)
    if level_nodes != canonical_nodes:
        raise CanonicalDecodeError(
            "oracle level rows do not exactly match canonical projection node order"
        )


def validate_acyclic_oracle_shape(projection: ProjectionSummary, oracle: OracleSummary) -> None:
    validate_oracle_shape(projection, oracle)
    nodes = {node.key for node in projection.nodes}
    if set(oracle.native_timing_order) != nodes or len(oracle.native_timing_order) != len(nodes):
        raise CanonicalDecodeError("acyclic oracle timing order does not cover all nodes")
    if any(level < 1 for _node, level in oracle.native_node_levels):
        raise CanonicalDecodeError("acyclic oracle levels must be one-based and positive")
