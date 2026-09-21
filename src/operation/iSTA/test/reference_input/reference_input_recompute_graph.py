#!/usr/bin/env python3
"""Independent raw-to-candidate graph, clock, and endpoint recomputation."""

from collections import deque
from dataclasses import dataclass
from typing import TypeVar

import reference_input_model as model
from reference_input_canonical import CanonicalError, CanonicalWriter


@dataclass(frozen=True)
class GraphFacts:
    node_by_name: dict[str, int]
    raw_node_by_name: dict[str, model.RawNode]
    raw_instance_by_name: dict[str, model.RawInstance]
    raw_arc_by_rank: tuple[model.RawArc, ...]
    instance_by_node: tuple[int | None, ...]
    clock_node_by_instance: tuple[int | None, ...]
    output_node_by_instance: tuple[int | None, ...]
    direct_clock_by_node: tuple[int | None, ...]
    clock_source_nodes: tuple[bool, ...]
    clock_reachable_nodes: tuple[bool, ...]
    clock_owner_by_node: tuple[str | None, ...]
    replayed_clock_arcs: tuple[bool, ...]
    timing_check_data_nodes: tuple[bool, ...]
    expected_start_ids: tuple[int, ...]
    expected_end_ids: tuple[int, ...]
    expected_eligibility: tuple[model.ArcEligibility, ...]


def _fail(message: str) -> None:
    raise CanonicalError(f"reference-input graph recomputation: {message}")


def _byte_key(value: str) -> bytes:
    return value.encode("utf-8", errors="strict")


def _token(value: object) -> str:
    tokens: dict[type[object], tuple[str, ...]] = {
        model.NodeKind: ("port", "instance_pin"),
        model.ArcKind: ("cell", "net"),
        model.TimingSense: (
            "none_positive_fallback",
            "positive_unate",
            "negative_unate",
            "non_unate",
        ),
        model.Transition: ("rise", "fall", "none"),
        model.TableVariable: (
            "none",
            "input_transition",
            "output_capacitance",
            "constrained_transition",
        ),
    }
    values = tokens.get(type(value))
    numeric = int(value) if hasattr(value, "__int__") else -1
    if values is None or not 0 <= numeric < len(values):
        _fail(f"unknown key enum {value!r}")
    return values[numeric]


def _write_node_key(writer: CanonicalWriter, key: model.NodeKey) -> None:
    writer.string(_token(key.kind))
    writer.string(key.full_name)
    writer.string(key.instance_name)
    writer.string(key.pin_name)


def _write_table_signature(writer: CanonicalWriter, table: model.TimingTableSignature) -> None:
    writer.boolean(table.present)
    writer.string(_token(table.variable1))
    writer.string(_token(table.variable2))


def _write_library_signature(writer: CanonicalWriter, signature: model.LibraryArcSignature) -> None:
    writer.string(signature.library_name)
    writer.i32(signature.arc_index)
    writer.string(_token(signature.sense))
    writer.string(_token(signature.trigger_transition))
    writer.string(_token(signature.check_transition))
    writer.string(signature.sdf_condition)
    for table in (*signature.delay_tables, *signature.slew_tables):
        _write_table_signature(writer, table)


def _node_key_bytes(key: model.NodeKey) -> bytes:
    writer = CanonicalWriter()
    _write_node_key(writer, key)
    return writer.result()


def _arc_base_bytes(key: model.ArcKey) -> bytes:
    writer = CanonicalWriter()
    _write_node_key(writer, key.source)
    _write_node_key(writer, key.sink)
    writer.string(_token(key.kind))
    writer.string(key.owner_name)
    writer.string(key.library_source_port)
    writer.string(key.library_sink_port)
    writer.vector(key.library_arcs, lambda signature: _write_library_signature(writer, signature))
    return writer.result()


def _arc_key_bytes(key: model.ArcKey) -> bytes:
    writer = CanonicalWriter()
    writer.u64(key.parallel_ordinal)
    return _arc_base_bytes(key) + writer.result()


T = TypeVar("T")


def _unique_by_name(values: tuple[T, ...], name: str) -> dict[str, T]:
    result: dict[str, T] = {}
    for value in values:
        key = getattr(value, name)
        if not isinstance(key, str) or not key or key in result:
            _fail(f"duplicate or empty {name}: {key!r}")
        result[key] = value
    return result


def _expected_node_key(raw: model.RawNode) -> model.NodeKey:
    if raw.map_key != raw.full_name:
        _fail(f"raw node map key differs from full name: {raw.map_key!r}")
    if raw.is_port:
        if raw.instance_name:
            _fail(f"raw port {raw.full_name!r} has an instance owner")
        return model.NodeKey(model.NodeKind.PORT, raw.full_name, "", "")
    prefix = f"{raw.instance_name}:"
    if not raw.instance_name or not raw.full_name.startswith(prefix):
        _fail(f"raw instance pin {raw.full_name!r} has an invalid owner")
    pin_name = raw.full_name[len(prefix) :]
    if not pin_name:
        _fail(f"raw instance pin {raw.full_name!r} has an empty local name")
    return model.NodeKey(model.NodeKind.INSTANCE_PIN, raw.full_name, raw.instance_name, pin_name)


def _node_net_names(raw: model.RawReferenceInput) -> dict[str, str]:
    result: dict[str, str] = {}
    seen_nets: set[str] = set()
    for net in raw.nets:
        if not net.map_key or net.map_key in seen_nets:
            _fail(f"duplicate or empty raw net key {net.map_key!r}")
        if net.map_key != net.embedded_name:
            _fail(f"raw net {net.map_key!r} embedded identity mismatch")
        seen_nets.add(net.map_key)
        for full_name in (*net.ordered_drivers, *net.ordered_loads):
            previous = result.setdefault(full_name, net.map_key)
            if previous != net.map_key:
                _fail(f"node {full_name!r} belongs to multiple raw nets")
    return result


def _validate_nodes_and_instances(
    candidate: model.CandidateInput, raw: model.RawReferenceInput
) -> tuple[
    dict[str, int],
    dict[str, model.RawNode],
    dict[str, model.RawInstance],
    tuple[int | None, ...],
    tuple[int | None, ...],
    tuple[int | None, ...],
]:
    raw_nodes = _unique_by_name(raw.nodes, "full_name")
    raw_instances = _unique_by_name(raw.instances, "map_key")
    if len(candidate.nodes) != len(raw.nodes):
        _fail("candidate/raw node counts differ")
    node_by_name: dict[str, int] = {}
    net_by_node = _node_net_names(raw)
    expected_keys = sorted((_expected_node_key(item) for item in raw.nodes), key=_node_key_bytes)
    if tuple(node.key for node in candidate.nodes) != tuple(expected_keys):
        _fail("candidate nodes are not the canonical raw NodeKey sequence")
    for index, node in enumerate(candidate.nodes):
        if node.node_id != index or node.key.full_name in node_by_name:
            _fail("candidate node IDs are not unique and dense")
        node_by_name[node.key.full_name] = index
        raw_node = raw_nodes[node.key.full_name]
        instance = raw_instances.get(raw_node.instance_name)
        expected_cell = "" if instance is None else instance.cell_name
        if (
            node.key != _expected_node_key(raw_node)
            or node.direction != raw_node.direction
            or node.is_port != raw_node.is_port
            or node.instance_cell != expected_cell
            or node.net_name != net_by_node.get(raw_node.full_name, "")
        ):
            _fail(f"candidate node {node.key.full_name!r} differs from raw projection")
    expected_lookup = tuple(model.NodeLookup(node.key, node.node_id) for node in candidate.nodes)
    if candidate.node_ids_by_key != expected_lookup:
        _fail("node_ids_by_key differs from canonical dense nodes")

    if tuple(item.map_key for item in raw.instances) != tuple(
        sorted((item.map_key for item in raw.instances), key=_byte_key)
    ):
        _fail("raw instances are not in native byte order")
    if len(candidate.instances) != len(raw.instances):
        _fail("candidate/raw instance counts differ")
    instance_by_node: list[int | None] = [None] * len(candidate.nodes)
    clock_nodes: list[int | None] = []
    output_nodes: list[int | None] = []
    checks_by_instance = {check.instance_name for check in raw.timing_checks}
    for instance_index, (actual, expected) in enumerate(
        zip(candidate.instances, raw.instances, strict=True)
    ):
        pins = tuple(node_by_name.get(name, -1) for name in expected.ordered_pin_names)
        if -1 in pins or len(set(pins)) != len(pins):
            _fail(f"raw instance {expected.map_key!r} has invalid ordered pins")
        fields_match = (
            actual.instance_name == expected.map_key
            and actual.cell_name == expected.cell_name
            and actual.ordered_pins == pins
            and actual.is_sequential == expected.sequential
            and actual.has_timing_checks == (expected.map_key in checks_by_instance)
            and actual.is_clock_gating == expected.gating
            and actual.is_macro == expected.macro
        )
        if not fields_match:
            _fail(f"candidate instance {expected.map_key!r} differs from raw projection")
        for node_id in pins:
            if instance_by_node[node_id] is not None:
                _fail(f"candidate node {node_id} belongs to multiple instances")
            instance_by_node[node_id] = instance_index
        role_names = (
            expected.clock_pin_name,
            expected.data_pin_name,
            expected.output_pin_name,
        )
        if any(
            role_name and role_name not in expected.ordered_pin_names for role_name in role_names
        ):
            _fail(f"raw instance {expected.map_key!r} has a role outside its ordered pins")
        clock_node = node_by_name.get(expected.clock_pin_name) if expected.clock_pin_name else None
        output_node = (
            node_by_name.get(expected.output_pin_name) if expected.output_pin_name else None
        )
        clock_nodes.append(clock_node)
        output_nodes.append(output_node)
        for node_id in pins:
            node = candidate.nodes[node_id]
            raw_pin = raw_nodes[node.key.full_name]
            expected_roles = (
                expected.sequential,
                bool(expected.clock_pin_name and node.key.full_name == expected.clock_pin_name),
                bool(expected.data_pin_name and node.key.full_name == expected.data_pin_name),
                bool(expected.output_pin_name and node.key.full_name == expected.output_pin_name),
            )
            if (
                node.is_sequential,
                node.is_clock_pin,
                node.is_data_pin,
                node.is_output_pin,
            ) != expected_roles or raw_pin.instance_name != expected.map_key:
                _fail(f"candidate role flags differ for {node.key.full_name!r}")
    return (
        node_by_name,
        raw_nodes,
        raw_instances,
        tuple(instance_by_node),
        tuple(clock_nodes),
        tuple(output_nodes),
    )


def _validate_arcs(
    candidate: model.CandidateInput,
    raw: model.RawReferenceInput,
    node_by_name: dict[str, int],
) -> tuple[tuple[model.RawArc, ...], tuple[tuple[int, ...], ...]]:
    if len(candidate.arcs) != len(raw.arcs):
        _fail("candidate/raw arc counts differ")
    by_rank: list[model.RawArc | None] = [None] * len(raw.arcs)
    for arc in raw.arcs:
        if arc.native_rank >= len(by_rank) or by_rank[arc.native_rank] is not None:
            _fail(f"raw arc rank is not unique and dense: {arc.native_rank}")
        by_rank[arc.native_rank] = arc
    if any(item is None for item in by_rank):
        _fail("raw arc ranks are not dense")
    ranked = tuple(item for item in by_rank if item is not None)
    if tuple(arc.key for arc in candidate.arcs) != tuple(
        sorted((arc.key for arc in candidate.arcs), key=_arc_key_bytes)
    ):
        _fail("candidate arcs are not in canonical ArcKey order")
    base_ranks: dict[bytes, list[int]] = {}
    for arc in candidate.arcs:
        base_ranks.setdefault(_arc_base_bytes(arc.key), []).append(arc.native_traversal_rank)
    expected_ordinals = {
        rank: ordinal for ranks in base_ranks.values() for ordinal, rank in enumerate(sorted(ranks))
    }
    rank_to_id: list[int | None] = [None] * len(ranked)
    for index, arc in enumerate(candidate.arcs):
        if arc.arc_id != index or arc.native_traversal_rank >= len(ranked):
            _fail("candidate arc IDs/ranks are not dense")
        raw_arc = ranked[arc.native_traversal_rank]
        if rank_to_id[raw_arc.native_rank] is not None:
            _fail("multiple candidate arcs use the same raw rank")
        rank_to_id[raw_arc.native_rank] = index
        if raw_arc.source not in node_by_name or raw_arc.sink not in node_by_name:
            _fail(f"raw arc {raw_arc.native_rank} has an unresolved endpoint")
        if (
            arc.source != node_by_name[raw_arc.source]
            or arc.sink != node_by_name[raw_arc.sink]
            or arc.key.source != candidate.nodes[arc.source].key
            or arc.key.sink != candidate.nodes[arc.sink].key
            or arc.kind != raw_arc.kind
            or arc.key.kind != raw_arc.kind
            or arc.key.owner_name != raw_arc.owner
            or arc.key.library_source_port != raw_arc.library_source_port
            or arc.key.library_sink_port != raw_arc.library_sink_port
            or arc.is_clock_arc != raw_arc.is_clock_arc
            or arc.is_disabled != raw_arc.disabled
            or arc.is_loop_disabled != raw_arc.loop_disabled
            or arc.key.parallel_ordinal != expected_ordinals[raw_arc.native_rank]
            or arc.key.library_arcs != tuple(child.signature for child in arc.transfer.children)
        ):
            _fail(f"candidate arc {index} differs from raw rank {raw_arc.native_rank}")
    if any(item is None for item in rank_to_id):
        _fail("candidate arcs do not cover every raw rank")
    expected_lookup = tuple(model.ArcLookup(arc.key, arc.arc_id) for arc in candidate.arcs)
    if candidate.arc_ids_by_key != expected_lookup:
        _fail("arc_ids_by_key differs from canonical dense arcs")

    outgoing: list[list[int]] = [[] for _ in candidate.nodes]
    incoming: list[list[int]] = [[] for _ in candidate.nodes]
    for rows, field, endpoint in (
        (outgoing, "outgoing_arc_ranks", "source"),
        (incoming, "incoming_arc_ranks", "sink"),
    ):
        seen: set[int] = set()
        for node in raw.nodes:
            for rank in getattr(node, field):
                if rank >= len(ranked) or rank in seen:
                    _fail(f"raw {field} has a duplicate or invalid slot")
                if getattr(ranked[rank], endpoint) != node.full_name:
                    _fail(f"raw {field} endpoint differs from its node")
                seen.add(rank)
                arc_id = rank_to_id[rank]
                assert arc_id is not None
                rows[node_by_name[node.full_name]].append(arc_id)
        if len(seen) != len(ranked):
            _fail(f"raw {field} omits an arc")
    expected_out_offsets, expected_out_ids = _flatten_rows(outgoing)
    expected_in_offsets, expected_in_ids = _flatten_rows(incoming)
    if (
        candidate.out_row_offsets != expected_out_offsets
        or candidate.out_arc_ids != expected_out_ids
        or candidate.in_row_offsets != expected_in_offsets
        or candidate.in_arc_ids != expected_in_ids
    ):
        _fail("candidate CSR rows differ from raw native adjacency")
    return ranked, tuple(tuple(row) for row in outgoing)


def _flatten_rows(rows: list[list[int]]) -> tuple[tuple[int, ...], tuple[int, ...]]:
    offsets = [0]
    values: list[int] = []
    for row in rows:
        values.extend(row)
        offsets.append(len(values))
    return tuple(offsets), tuple(values)


def _validate_constraints(
    candidate: model.CandidateInput,
    raw: model.RawReferenceInput,
    node_by_name: dict[str, int],
) -> tuple[tuple[int | None, ...], tuple[bool, ...]]:
    if len(candidate.clocks) != len(raw.clock_constraints):
        _fail("candidate/raw clock counts differ")
    direct: list[int | None] = [None] * len(candidate.nodes)
    sources = [False] * len(candidate.nodes)
    for clock_index, (actual, expected) in enumerate(
        zip(candidate.clocks, raw.clock_constraints, strict=True)
    ):
        expected_sources = tuple(
            model.CandidateRawNodeReference(name, node_by_name.get(name))
            for name in expected.ordered_sources
        )
        if (
            actual.map_key != expected.map_key
            or actual.embedded_clock_name != expected.embedded_name
            or actual.ordered_sources != expected_sources
            or actual.period != expected.period
            or actual.edges != (expected.rise_edge, expected.fall_edge)
            or actual.propagated != expected.propagated
        ):
            _fail(f"candidate clock {clock_index} differs from raw projection")
        for source in actual.ordered_sources:
            if source.resolved_node is not None:
                sources[source.resolved_node] = True
                if direct[source.resolved_node] is None:
                    direct[source.resolved_node] = clock_index
    if len(candidate.port_constraints) != len(raw.port_constraints):
        _fail("candidate/raw port-constraint counts differ")
    for index, (actual, expected) in enumerate(
        zip(candidate.port_constraints, raw.port_constraints, strict=True)
    ):
        if actual != model.CandidatePortConstraint(
            expected.map_key,
            expected.embedded_port_name,
            node_by_name.get(expected.map_key),
            (expected.input_delay_max, expected.input_delay_min),
            expected.input_transition,
            (expected.output_delay_max, expected.output_delay_min),
            expected.output_load,
            expected.clock_name,
        ):
            _fail(f"candidate port constraint {index} differs from raw projection")
    if len(candidate.case_analysis_constraints) != len(raw.case_constraints):
        _fail("candidate/raw case-analysis counts differ")
    for index, (actual, expected) in enumerate(
        zip(candidate.case_analysis_constraints, raw.case_constraints, strict=True)
    ):
        if expected.map_key != expected.raw_name or actual != model.CandidateCaseConstraint(
            expected.raw_name, node_by_name.get(expected.raw_name), expected.value
        ):
            _fail(f"candidate case analysis {index} differs from raw projection")
    return tuple(direct), tuple(sources)


def _timing_check_data_nodes(
    candidate: model.CandidateInput,
    raw: model.RawReferenceInput,
    node_by_name: dict[str, int],
) -> tuple[bool, ...]:
    if len(candidate.timing_check_endpoints) != len(raw.timing_checks):
        _fail("candidate/raw timing-check counts differ")
    result = [False] * len(candidate.nodes)
    for index, (actual, expected) in enumerate(
        zip(candidate.timing_check_endpoints, raw.timing_checks, strict=True)
    ):
        projected = model.CandidateTimingCheckEndpoint(
            expected.instance_name,
            expected.ordinal,
            expected.kind,
            expected.raw_clock_name,
            expected.raw_data_name,
            expected.full_clock_name,
            expected.full_data_name,
            node_by_name.get(expected.full_clock_name),
            node_by_name.get(expected.full_data_name),
        )
        if actual != projected:
            _fail(f"candidate timing-check endpoint {index} differs from raw projection")
        for resolved in (actual.resolved_clock_node, actual.resolved_data_node):
            if resolved is not None:
                node = candidate.nodes[resolved]
                if node.is_port or node.key.instance_name != actual.instance_name:
                    _fail(f"timing-check endpoint {index} resolves outside its instance")
        if actual.resolved_data_node is not None:
            result[actual.resolved_data_node] = True
    return tuple(result)


def _visit_clock_node(
    node_id: int,
    clock_key: str,
    visited: list[bool],
    ready: deque[int],
    reachable: list[bool],
    owners: list[str | None],
) -> None:
    if owners[node_id] is not None and owners[node_id] != clock_key:
        _fail(f"different clock map keys overlap at node {node_id}")
    reachable[node_id] = True
    if owners[node_id] is None:
        owners[node_id] = clock_key
    if not visited[node_id]:
        visited[node_id] = True
        ready.append(node_id)


def _replay_clocks(
    candidate: model.CandidateInput,
    outgoing: tuple[tuple[int, ...], ...],
    instance_by_node: tuple[int | None, ...],
) -> tuple[tuple[bool, ...], tuple[str | None, ...], tuple[bool, ...]]:
    reachable = [False] * len(candidate.nodes)
    owners: list[str | None] = [None] * len(candidate.nodes)
    replayed = [False] * len(candidate.arcs)
    for clock in candidate.clocks:
        visited = [False] * len(candidate.nodes)
        ready: deque[int] = deque()

        for source in clock.ordered_sources:
            if source.resolved_node is not None:
                _visit_clock_node(
                    source.resolved_node, clock.map_key, visited, ready, reachable, owners
                )
        while ready:
            node_id = ready.popleft()
            owner = instance_by_node[node_id]
            node = candidate.nodes[node_id]
            if (
                owner is not None
                and candidate.instances[owner].is_sequential
                and not candidate.instances[owner].is_clock_gating
                and node.is_clock_pin
            ):
                continue
            for arc_id in outgoing[node_id]:
                arc = candidate.arcs[arc_id]
                if arc.is_disabled or arc.is_loop_disabled:
                    continue
                replayed[arc_id] = True
                _visit_clock_node(arc.sink, clock.map_key, visited, ready, reachable, owners)
    return tuple(reachable), tuple(owners), tuple(replayed)


def _recompute_roles(
    candidate: model.CandidateInput,
    raw: model.RawReferenceInput,
    instance_by_node: tuple[int | None, ...],
    source_nodes: tuple[bool, ...],
    check_data_nodes: tuple[bool, ...],
    reachable: tuple[bool, ...],
    replayed: tuple[bool, ...],
) -> tuple[tuple[int, ...], tuple[int, ...], tuple[model.ArcEligibility, ...]]:
    incoming = [False] * len(candidate.nodes)
    outgoing = [False] * len(candidate.nodes)
    active_indegree = [0] * len(candidate.nodes)
    for arc in candidate.arcs:
        incoming[arc.sink] = True
        outgoing[arc.source] = True
        if not arc.is_disabled and not arc.is_loop_disabled:
            active_indegree[arc.sink] += 1
    by_name = {node.key.full_name: node.node_id for node in candidate.nodes}
    expected_roots = tuple(
        by_name[node.full_name]
        for node in sorted(raw.nodes, key=lambda item: _byte_key(item.full_name))
        if active_indegree[by_name[node.full_name]] == 0
    )
    if candidate.ordered_root_ids != expected_roots:
        _fail("ordered_root_ids differs from raw active-indegree roots")

    start_flags: list[bool] = []
    end_flags: list[bool] = []
    eligibility: list[model.ArcEligibility] = []
    for node in candidate.nodes:
        input_port = node.is_port and node.direction in (
            model.Direction.INPUT,
            model.Direction.INOUT,
        )
        output_port = node.is_port and node.direction in (
            model.Direction.OUTPUT,
            model.Direction.INOUT,
        )
        owner = instance_by_node[node.node_id]
        sequential_clock = (
            owner is not None
            and candidate.instances[owner].is_sequential
            and not candidate.instances[owner].is_clock_gating
            and node.is_clock_pin
        )
        if sequential_clock:
            is_start = True
        elif node.is_clock_pin:
            is_start = False
        elif source_nodes[node.node_id]:
            is_start = input_port
        else:
            is_start = not incoming[node.node_id] or input_port
        is_end = (
            not node.is_clock_pin
            and not source_nodes[node.node_id]
            and (not outgoing[node.node_id] or output_port or check_data_nodes[node.node_id])
        )
        start_flags.append(is_start)
        end_flags.append(is_end)
        if node.is_startpoint != is_start or node.is_endpoint != is_end:
            _fail(f"candidate start/end flags differ for {node.key.full_name!r}")
    full_name_order = sorted(
        range(len(candidate.nodes)),
        key=lambda node_id: _byte_key(candidate.nodes[node_id].key.full_name),
    )
    starts = tuple(node_id for node_id in full_name_order if start_flags[node_id])
    ends = tuple(node_id for node_id in full_name_order if end_flags[node_id])
    if candidate.ordered_start_ids != starts or candidate.ordered_end_ids != ends:
        _fail("ordered start/end vectors differ from independently derived flags")
    for arc_id, arc in enumerate(candidate.arcs):
        active = not arc.is_disabled and not arc.is_loop_disabled
        sink = candidate.nodes[arc.sink]
        sequential_clock_sink = (
            arc.kind == model.ArcKind.NET and sink.is_sequential and sink.is_clock_pin
        )
        expected = model.ArcEligibility(
            active,
            active and not sequential_clock_sink,
            active and not (sequential_clock_sink and reachable[arc.sink]),
            replayed[arc_id],
        )
        eligibility.append(expected)
        if arc.eligibility != expected:
            _fail(f"candidate eligibility differs for arc {arc_id}")
    return starts, ends, tuple(eligibility)


def recompute_graph(candidate: model.CandidateInput, raw: model.RawReferenceInput) -> GraphFacts:
    """Validate structural projection and independently derive graph semantics."""
    if candidate.analysis_order != (model.AnalysisMode.MAX, model.AnalysisMode.MIN):
        _fail("analysis_order is not [max,min]")
    if candidate.path_source_order != (model.PathSource.INPUT, model.PathSource.REGISTER):
        _fail("path_source_order is not [input,register]")
    if candidate.transition_order != (model.Transition.RISE, model.Transition.FALL):
        _fail("transition_order is not [rise,fall]")
    (
        node_by_name,
        raw_nodes,
        raw_instances,
        instance_by_node,
        clock_nodes,
        output_nodes,
    ) = _validate_nodes_and_instances(candidate, raw)
    raw_arcs, outgoing = _validate_arcs(candidate, raw, node_by_name)
    direct, sources = _validate_constraints(candidate, raw, node_by_name)
    check_data = _timing_check_data_nodes(candidate, raw, node_by_name)
    reachable, owners, replayed = _replay_clocks(candidate, outgoing, instance_by_node)
    if candidate.clock_owner_by_node != owners:
        _fail("dense clock_owner_by_node differs from replayed map-key owners")
    starts, ends, eligibility = _recompute_roles(
        candidate,
        raw,
        instance_by_node,
        sources,
        check_data,
        reachable,
        replayed,
    )
    return GraphFacts(
        node_by_name,
        raw_nodes,
        raw_instances,
        raw_arcs,
        instance_by_node,
        clock_nodes,
        output_nodes,
        direct,
        sources,
        reachable,
        owners,
        replayed,
        check_data,
        starts,
        ends,
        eligibility,
    )
