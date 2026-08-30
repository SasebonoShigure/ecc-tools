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
"""Independent handwritten wire fixtures for projection-controller self-tests."""
# ruff: noqa: FBT001, FBT003

from collections.abc import Sequence


class WireWriter:
    """Small known-answer encoder that is independent from the production encoder."""

    def __init__(self) -> None:
        self.data = bytearray()

    def u8(self, value: int) -> None:
        self.data.append(value)

    def boolean(self, value: bool) -> None:
        self.u8(1 if value else 0)

    def u64(self, value: int) -> None:
        self.data.extend(value.to_bytes(8, "big", signed=False))

    def i32(self, value: int) -> None:
        self.data.extend(value.to_bytes(4, "big", signed=True))

    def string(self, value: str) -> None:
        encoded = value.encode("utf-8")
        self.u64(len(encoded))
        self.data.extend(encoded)

    def bytes(self, value: bytes) -> None:
        self.u64(len(value))
        self.data.extend(value)

    def domain(self, name: str) -> None:
        self.string("ista-csr")
        self.string(name)
        self.string("1.0.0")

    def result(self) -> bytes:
        return bytes(self.data)


def _node_fields(writer: WireWriter, name: str) -> None:
    writer.string("port")
    writer.string(name)
    writer.string("")
    writer.string("")


def _node_key(name: str) -> bytes:
    writer = WireWriter()
    writer.domain("node-key")
    _node_fields(writer, name)
    return writer.result()


def _arc_key(source: str, sink: str, owner: str, ordinal: int) -> bytes:
    writer = WireWriter()
    writer.domain("arc-key")
    _node_fields(writer, source)
    _node_fields(writer, sink)
    writer.string("net")
    writer.string(owner)
    writer.string("")
    writer.string("")
    writer.u64(0)
    writer.u64(ordinal)
    return writer.result()


def _write_embedded_nodes(writer: WireWriter, names: Sequence[str]) -> None:
    writer.u64(len(names))
    for name in names:
        writer.bytes(_node_key(name))


def _shape(case_name: str) -> tuple[tuple[str, ...], tuple[tuple[str, str], ...]]:
    if case_name == "chain":
        return ("a", "b", "c"), (("a", "b"), ("a", "b"), ("b", "c"))
    if case_name == "diamond":
        return ("a", "b", "c", "d"), (
            ("a", "b"),
            ("a", "c"),
            ("b", "d"),
            ("c", "d"),
        )
    raise ValueError(f"unknown handwritten fixture {case_name!r}")


def oracle_bytes(timing_order: Sequence[str], level_rows: Sequence[tuple[str, int]]) -> bytes:
    oracle = WireWriter()
    oracle.domain("native-graph-oracle")
    oracle.string("ista-csr-native-graph-oracle/1.0.0")
    _write_embedded_nodes(oracle, timing_order)
    oracle.u64(len(level_rows))
    for name, level in level_rows:
        oracle.bytes(_node_key(name))
        oracle.i32(level)
    return oracle.result()


def _ranked_edges(
    edge_order: Sequence[tuple[str, str]], mutation: str | None
) -> tuple[tuple[str, str, int, int], ...]:
    next_ordinal: dict[tuple[str, str], int] = {}
    ranked = []
    for rank, (source, sink) in enumerate(edge_order):
        base = source, sink
        ordinal = next_ordinal.get(base, 0)
        next_ordinal[base] = ordinal + 1
        ranked.append((source, sink, rank, ordinal))
    if mutation == "swap_parallel_ordinals":
        positions = [index for index, edge in enumerate(ranked) if edge[:2] == ("a", "b")]
        if len(positions) != 2:
            raise ValueError("parallel-ordinal mutation requires exactly two a-to-b arcs")
        first, second = positions
        source, sink, rank, ordinal = ranked[first]
        source2, sink2, rank2, ordinal2 = ranked[second]
        ranked[first] = source, sink, rank, ordinal2
        ranked[second] = source2, sink2, rank2, ordinal
    return tuple(ranked)


def fixture_bytes(case_name: str, mutation: str | None = None) -> tuple[bytes, bytes, int, int]:
    if mutation not in {None, "swap_parallel_ordinals", "drop_roots"}:
        raise ValueError(f"unknown handwritten fixture mutation {mutation!r}")
    timing_order, edge_order = _shape(case_name)
    nodes = tuple(sorted(timing_order, key=_node_key))
    net_name = f"net_{case_name}"
    ranked_edges = _ranked_edges(edge_order, mutation)
    arcs = tuple(
        sorted(ranked_edges, key=lambda edge: _arc_key(edge[0], edge[1], net_name, edge[3]))
    )
    incoming = {name: [] for name in nodes}
    outgoing = {name: [] for name in nodes}
    for edge in ranked_edges:
        source, sink, _rank, _ordinal = edge
        outgoing[source].append(edge)
        incoming[sink].append(edge)
    roots = tuple(sorted((name for name in nodes if not incoming[name]), key=str.encode))
    if mutation == "drop_roots":
        roots = ()
    ends = tuple(name for name in timing_order if not outgoing[name])

    projection = WireWriter()
    projection.domain("native-graph-projection")
    projection.string("ista-csr-native-graph-projection/1.0.0")
    projection.u64(len(nodes))
    for name in nodes:
        projection.bytes(_node_key(name))
        projection.string("input" if name in roots else "output" if name in ends else "inout")
        projection.string("")
        projection.string(net_name)
        projection.boolean(True)
        projection.boolean(name in roots)
        projection.boolean(name in ends)
        projection.boolean(False)
        projection.boolean(False)
        projection.boolean(False)
        projection.boolean(False)
    projection.u64(0)
    projection.u64(len(arcs))
    for source, sink, rank, ordinal in arcs:
        projection.bytes(_arc_key(source, sink, net_name, ordinal))
        projection.u64(rank)
        projection.boolean(False)
        projection.boolean(False)
        projection.boolean(False)
    for rows in (outgoing, incoming):
        projection.u64(len(nodes))
        for name in nodes:
            projection.bytes(_node_key(name))
            row = tuple(sorted(rows[name], key=lambda edge: edge[2]))
            projection.u64(len(row))
            for source, sink, _rank, ordinal in row:
                projection.bytes(_arc_key(source, sink, net_name, ordinal))
    _write_embedded_nodes(projection, roots)
    _write_embedded_nodes(projection, roots)
    _write_embedded_nodes(projection, ends)

    levels: dict[str, int] = {}
    for name in timing_order:
        levels[name] = 1 + max(
            (levels[source] for source, sink in edge_order if sink == name), default=0
        )
    oracle = oracle_bytes(timing_order, tuple((name, levels[name]) for name in nodes))
    return projection.result(), oracle, len(nodes), len(arcs)


def _write_table(writer: WireWriter, present: bool, variable1: str, variable2: str) -> None:
    writer.boolean(present)
    writer.string(variable1)
    writer.string(variable2)


def valid_library_arc_bytes() -> bytes:
    writer = WireWriter()
    writer.string("")
    writer.i32(-7)
    writer.string("positive_unate")
    writer.string("rise")
    writer.string("fall")
    writer.string("")
    for _index in range(4):
        _write_table(writer, True, "none", "none")
    return writer.result()


def invalid_second_variable_table_bytes() -> bytes:
    writer = WireWriter()
    _write_table(writer, True, "none", "output_capacitance")
    return writer.result()
