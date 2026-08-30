#!/usr/bin/env python3
"""Independent reconstruction of the five payloads and composite identity."""

import hashlib
from dataclasses import dataclass
from enum import IntEnum

import reference_input_model as model
from reference_input_canonical import CanonicalError, CanonicalWriter

_TOKENS: dict[type[IntEnum], tuple[str, ...]] = {
    model.NodeKind: ("port", "instance_pin"),
    model.ArcKind: ("cell", "net"),
    model.Direction: ("none", "input", "output", "inout"),
    model.AnalysisMode: ("max", "min"),
    model.Transition: ("rise", "fall", "none"),
    model.PathSource: ("input", "register"),
    model.StartSeedAvailability: ("materialized", "unsupported_sequential"),
    model.TimingSense: (
        "none_positive_fallback",
        "positive_unate",
        "negative_unate",
        "non_unate",
    ),
    model.TableVariable: (
        "none",
        "input_transition",
        "output_capacitance",
        "constrained_transition",
    ),
    model.TimingCheckKind: (
        "none",
        "setup",
        "hold",
        "recovery",
        "removal",
        "width",
        "period",
    ),
    model.UnsupportedReason: (
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
    ),
}


@dataclass(frozen=True)
class FingerprintPayloads:
    topology: bytes
    clock_constraint: bytes
    library: bytes
    corner: bytes
    parasitic: bytes
    timing_input: bytes


def _token(value: IntEnum) -> str:
    tokens = _TOKENS.get(type(value))
    if tokens is None or not 0 <= value.value < len(tokens):
        raise CanonicalError(f"unknown fingerprint enum {value!r}")
    return tokens[value.value]


def _domain(writer: CanonicalWriter, name: str, version: str = "1.0.0") -> None:
    writer.string("ista-csr")
    writer.string(name)
    writer.string(version)


def _double(writer: CanonicalWriter, value: float) -> None:
    writer.canary_double(value)


def _optional_double(writer: CanonicalWriter, value: float | None) -> None:
    writer.optional(value, lambda item: _double(writer, item))


def _optional_u64(writer: CanonicalWriter, value: int | None) -> None:
    writer.optional(value, writer.u64)


def _optional_matrix(
    writer: CanonicalWriter,
    matrix: tuple[tuple[float | None, float | None], tuple[float | None, float | None]],
) -> None:
    for row in matrix:
        for value in row:
            _optional_double(writer, value)


def _node_key(writer: CanonicalWriter, key: model.NodeKey) -> None:
    writer.string(_token(key.kind))
    writer.string(key.full_name)
    writer.string(key.instance_name)
    writer.string(key.pin_name)


def _table_signature(writer: CanonicalWriter, signature: model.TimingTableSignature) -> None:
    writer.boolean(signature.present)
    writer.string(_token(signature.variable1))
    writer.string(_token(signature.variable2))


def _library_signature(writer: CanonicalWriter, signature: model.LibraryArcSignature) -> None:
    writer.string(signature.library_name)
    writer.i32(signature.arc_index)
    writer.string(_token(signature.sense))
    writer.string(_token(signature.trigger_transition))
    writer.string(_token(signature.check_transition))
    writer.string(signature.sdf_condition)
    for table in (*signature.delay_tables, *signature.slew_tables):
        _table_signature(writer, table)


def _arc_key(writer: CanonicalWriter, key: model.ArcKey) -> None:
    _node_key(writer, key.source)
    _node_key(writer, key.sink)
    writer.string(_token(key.kind))
    writer.string(key.owner_name)
    writer.string(key.library_source_port)
    writer.string(key.library_sink_port)
    writer.vector(key.library_arcs, lambda item: _library_signature(writer, item))
    writer.u64(key.parallel_ordinal)


def _embedded_node_key(key: model.NodeKey) -> bytes:
    writer = CanonicalWriter()
    _domain(writer, "node-key")
    _node_key(writer, key)
    return writer.result()


def _embedded_arc_key(key: model.ArcKey) -> bytes:
    writer = CanonicalWriter()
    _domain(writer, "arc-key")
    _arc_key(writer, key)
    return writer.result()


def _table(writer: CanonicalWriter, table: model.CandidateTimingTable) -> None:
    writer.boolean(table.present)
    if not table.present:
        return
    writer.string(_token(table.variable1))
    writer.string(_token(table.variable2))
    writer.vector(
        table.axes,
        lambda axis: writer.vector(axis, lambda value: _double(writer, value)),
    )
    writer.vector(table.values, lambda value: _double(writer, value))


def _child(writer: CanonicalWriter, child: model.ChildTimingArc) -> None:
    _library_signature(writer, child.signature)
    _double(writer, child.time_unit_scale)
    _double(writer, child.capacitance_unit_scale)
    _double(writer, child.slew_derate)
    for values in (
        child.slew_lower_threshold_pct,
        child.slew_upper_threshold_pct,
        child.input_threshold_pct,
        child.output_threshold_pct,
    ):
        for value in values:
            _double(writer, value)
    for table in (*child.delay_tables, *child.slew_tables, *child.check_tables):
        _table(writer, table)


def _transfer(writer: CanonicalWriter, transfer: model.CellTransfer) -> None:
    _double(writer, transfer.delay)
    _double(writer, transfer.delay_min)
    _double(writer, transfer.delay_max)
    writer.boolean(transfer.is_clock_arc)
    writer.boolean(transfer.is_disabled)
    writer.boolean(transfer.is_clear_or_preset)
    writer.vector(transfer.children, lambda item: _child(writer, item))


def _library_cell(writer: CanonicalWriter, cell: model.CandidateReferencedLibraryCell) -> None:
    writer.string(cell.cell_name)
    writer.boolean(cell.present)
    if not cell.present:
        return
    writer.string(cell.library_name)
    writer.boolean(cell.is_sequential)
    writer.boolean(cell.is_clock_gating)
    writer.boolean(cell.is_macro)
    writer.string(cell.clock_pin_name)
    writer.string(cell.data_pin_name)
    writer.string(cell.output_pin_name)
    writer.u64(len(cell.ports))
    for port in cell.ports:
        writer.string(port.name)
        writer.string(_token(port.direction))
        writer.boolean(port.is_clock)
        writer.boolean(port.is_output)
        _double(writer, port.default_capacitance)
        _optional_matrix(writer, port.transition_capacitance)
    writer.u64(len(cell.cell_arcs))
    for arc in cell.cell_arcs:
        writer.string(arc.source_port)
        writer.string(arc.sink_port)
        _transfer(writer, arc.transfer)
    writer.u64(len(cell.timing_checks))
    for check in cell.timing_checks:
        writer.string(check.clock_port)
        writer.string(check.data_port)
        writer.string(_token(check.kind))
        _double(writer, check.check_time)
        writer.string(_token(check.clock_transition))
        writer.vector(check.children, lambda item: _child(writer, item))


def _parasitics(writer: CanonicalWriter, parasitics: model.CandidateParasitics) -> None:
    writer.string(parasitics.capacitance_unit)
    writer.string(parasitics.resistance_unit)
    writer.u64(len(parasitics.nets))
    for net in parasitics.nets:
        writer.string(net.map_key)
        writer.string(net.embedded_name)
        _double(writer, net.lumped_capacitance)
        writer.u64(len(net.nodes))
        for node in net.nodes:
            writer.string(node.map_key)
            writer.string(node.embedded_name)
            _double(writer, node.capacitance)
            _double(writer, node.x)
            _double(writer, node.y)
        writer.u64(len(net.resistors))
        for resistor in net.resistors:
            writer.string(resistor.source)
            writer.string(resistor.sink)
            _double(writer, resistor.resistance)


def _topology(input_value: model.CandidateInput) -> bytes:
    writer = CanonicalWriter()
    _domain(writer, "reference-topology-payload", input_value.schema_version)
    writer.boolean(not input_value.nodes and not input_value.instances and not input_value.arcs)
    writer.u64(len(input_value.nodes))
    for node in input_value.nodes:
        writer.u64(node.node_id)
        writer.raw_bytes(_embedded_node_key(node.key))
        writer.string(_token(node.direction))
        writer.string(node.instance_cell)
        writer.string(node.net_name)
        writer.boolean(node.is_port)
    writer.u64(len(input_value.node_ids_by_key))
    for entry in input_value.node_ids_by_key:
        writer.raw_bytes(_embedded_node_key(entry.key))
        writer.u64(entry.node_id)
    writer.u64(len(input_value.instances))
    for instance in input_value.instances:
        writer.string(instance.instance_name)
        writer.string(instance.cell_name)
        writer.vector(instance.ordered_pins, writer.u64)
    writer.u64(len(input_value.arcs))
    for arc in input_value.arcs:
        writer.u64(arc.arc_id)
        writer.raw_bytes(_embedded_arc_key(arc.key))
        writer.u64(arc.source)
        writer.u64(arc.sink)
        writer.string(_token(arc.kind))
        writer.u64(arc.native_traversal_rank)
        writer.boolean(arc.is_clock_arc)
        writer.boolean(arc.is_disabled)
        writer.boolean(arc.is_loop_disabled)
    writer.u64(len(input_value.arc_ids_by_key))
    for entry in input_value.arc_ids_by_key:
        writer.raw_bytes(_embedded_arc_key(entry.key))
        writer.u64(entry.arc_id)
    for values in (
        input_value.out_row_offsets,
        input_value.out_arc_ids,
        input_value.in_row_offsets,
        input_value.in_arc_ids,
        input_value.ordered_root_ids,
    ):
        writer.vector(values, writer.u64)
    return writer.result()


def _clock_constraints(input_value: model.CandidateInput) -> bytes:
    writer = CanonicalWriter()
    _domain(writer, "reference-clock-constraint-payload", input_value.schema_version)
    writer.boolean(
        not input_value.clocks
        and not input_value.port_constraints
        and not input_value.case_analysis_constraints
    )
    writer.u64(len(input_value.clocks))
    for clock in input_value.clocks:
        writer.string(clock.map_key)
        writer.string(clock.embedded_clock_name)
        writer.u64(len(clock.ordered_sources))
        for source in clock.ordered_sources:
            writer.string(source.full_name)
        _double(writer, clock.period)
        for edge in clock.edges:
            _double(writer, edge)
        writer.boolean(clock.propagated)
    writer.u64(len(input_value.port_constraints))
    for port in input_value.port_constraints:
        writer.string(port.map_key)
        writer.string(port.embedded_port_name)
        for value in port.input_delay:
            _optional_double(writer, value)
        _optional_double(writer, port.input_transition)
        for value in port.output_delay:
            _optional_double(writer, value)
        _optional_double(writer, port.output_load)
        writer.string(port.clock_name)
    writer.u64(len(input_value.case_analysis_constraints))
    for case in input_value.case_analysis_constraints:
        writer.string(case.raw_name)
        writer.boolean(case.value)
    return writer.result()


def _library(input_value: model.CandidateInput) -> bytes:
    writer = CanonicalWriter()
    _domain(writer, "reference-library-payload", input_value.schema_version)
    writer.boolean(not input_value.referenced_library_cells)
    writer.vector(
        input_value.referenced_library_cells,
        lambda cell: _library_cell(writer, cell),
    )
    return writer.result()


def _corner(input_value: model.CandidateInput) -> bytes:
    writer = CanonicalWriter()
    _domain(writer, "reference-corner-payload", input_value.schema_version)
    context = input_value.analysis_context
    writer.boolean(not context.liberty_corner and not context.temperature and not context.rc_corner)
    writer.string(context.liberty_corner)
    writer.string(context.temperature)
    writer.string(context.rc_corner)
    return writer.result()


def _parasitic(input_value: model.CandidateInput) -> bytes:
    writer = CanonicalWriter()
    _domain(writer, "reference-parasitic-payload", input_value.schema_version)
    writer.boolean(not input_value.parasitics.nets)
    _parasitics(writer, input_value.parasitics)
    return writer.result()


def _timing_input(input_value: model.CandidateInput) -> bytes:
    writer = CanonicalWriter()
    _domain(writer, "reference-timing-input-payload", input_value.schema_version)
    writer.boolean(
        not input_value.nodes
        and not input_value.arcs
        and not input_value.start_seeds
        and not input_value.endpoint_required
    )
    for values in (
        input_value.analysis_order,
        input_value.path_source_order,
        input_value.transition_order,
    ):
        for value in values:
            writer.string(_token(value))
    writer.u64(len(input_value.nodes))
    for node in input_value.nodes:
        writer.u64(node.node_id)
        for value in (
            node.is_startpoint,
            node.is_endpoint,
            node.is_sequential,
            node.is_clock_pin,
            node.is_data_pin,
            node.is_output_pin,
        ):
            writer.boolean(value)
    writer.u64(len(input_value.instances))
    for instance in input_value.instances:
        writer.string(instance.instance_name)
        writer.boolean(instance.is_sequential)
        writer.boolean(instance.has_timing_checks)
        writer.boolean(instance.is_clock_gating)
        writer.boolean(instance.is_macro)
    writer.u64(len(input_value.arcs))
    for arc in input_value.arcs:
        writer.u64(arc.arc_id)
        writer.boolean(arc.eligibility.levelization)
        writer.boolean(arc.eligibility.data_arrival)
        writer.boolean(arc.eligibility.data_slew)
        writer.boolean(arc.eligibility.clock)
        _transfer(writer, arc.transfer)
        writer.boolean(arc.output_load is not None)
        if arc.output_load is not None:
            for row in arc.output_load:
                for value in row:
                    _double(writer, value)
    writer.u64(len(input_value.clocks))
    for clock in input_value.clocks:
        writer.u64(len(clock.ordered_sources))
        for source in clock.ordered_sources:
            _optional_u64(writer, source.resolved_node)
    writer.u64(len(input_value.clock_owner_by_node))
    for owner in input_value.clock_owner_by_node:
        writer.optional(owner, writer.string)
    writer.u64(len(input_value.port_constraints))
    for port in input_value.port_constraints:
        _optional_u64(writer, port.resolved_node)
    writer.u64(len(input_value.case_analysis_constraints))
    for case in input_value.case_analysis_constraints:
        _optional_u64(writer, case.resolved_node)
    writer.u64(len(input_value.timing_check_endpoints))
    for check in input_value.timing_check_endpoints:
        writer.string(check.instance_name)
        writer.u64(check.referenced_check_ordinal)
        writer.string(_token(check.check_kind))
        writer.string(check.clock_pin_raw_name)
        writer.string(check.data_pin_raw_name)
        writer.string(check.clock_pin_full_name)
        writer.string(check.data_pin_full_name)
        _optional_u64(writer, check.resolved_clock_node)
        _optional_u64(writer, check.resolved_data_node)
    for values in (input_value.ordered_start_ids, input_value.ordered_end_ids):
        writer.vector(values, writer.u64)
    writer.vector(input_value.clock_tags, writer.string)
    writer.u64(len(input_value.start_seeds))
    for seed in input_value.start_seeds:
        writer.u64(seed.seed_node)
        writer.u64(seed.path_start_node)
        writer.string(_token(seed.availability))
        writer.optional(seed.path_source, lambda value: writer.string(_token(value)))
        writer.string(seed.clock_tag)
        _optional_matrix(writer, seed.arrival)
        _optional_matrix(writer, seed.slew)
        _optional_u64(writer, seed.crpr_node)
        writer.optional(seed.crpr_transition, lambda value: writer.string(_token(value)))
    writer.u64(len(input_value.endpoint_required))
    for endpoint in input_value.endpoint_required:
        writer.u64(endpoint.node)
        writer.string(endpoint.clock_name)
        _double(writer, endpoint.clock_period)
        for value in endpoint.output_delay:
            _optional_double(writer, value)
    writer.u64(len(input_value.cell_pin_capacitances))
    for capacitance in input_value.cell_pin_capacitances:
        writer.u64(capacitance.node)
        _double(writer, capacitance.default_capacitance)
        _optional_matrix(writer, capacitance.transition_capacitance)
    writer.vector(
        input_value.unsupported_reasons,
        lambda reason: writer.string(_token(reason)),
    )
    return writer.result()


def build_fingerprint_payloads(input_value: model.CandidateInput) -> FingerprintPayloads:
    return FingerprintPayloads(
        _topology(input_value),
        _clock_constraints(input_value),
        _library(input_value),
        _corner(input_value),
        _parasitic(input_value),
        _timing_input(input_value),
    )


def _component(domain: str, payload: bytes) -> str:
    writer = CanonicalWriter()
    _domain(writer, domain)
    writer.raw_bytes(payload)
    return hashlib.sha256(writer.result()).hexdigest()


def recompute_identity(
    input_value: model.CandidateInput, session_token: int, generation: int
) -> model.SnapshotIdentity:
    if session_token <= 0:
        raise CanonicalError("reference-input session token must be nonzero")
    payloads = build_fingerprint_payloads(input_value)
    topology = _component("topology-fingerprint", payloads.topology)
    clock = _component("clock-constraint-fingerprint", payloads.clock_constraint)
    library = _component("library-fingerprint", payloads.library)
    corner = _component("corner-fingerprint", payloads.corner)
    parasitic = _component("parasitic-fingerprint", payloads.parasitic)
    composite = CanonicalWriter()
    _domain(composite, "timing-input-composite")
    for value in (topology, clock, library, corner, parasitic):
        composite.string(value)
    composite.raw_bytes(payloads.timing_input)
    return model.SnapshotIdentity(
        input_value.schema_version,
        session_token,
        generation,
        topology,
        clock,
        library,
        corner,
        parasitic,
        hashlib.sha256(composite.result()).hexdigest(),
    )
