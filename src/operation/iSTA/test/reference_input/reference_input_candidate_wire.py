#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# ***************************************************************************************
"""Independent strict codec for the full detached reference-input envelope."""

import reference_input_model as model
from reference_input_canonical import CanonicalError, CanonicalReader, CanonicalWriter

CANDIDATE_DOMAIN = "ista-csr-reference-input-test-wire/2.0.0"
_DoublePair = tuple[float, float]
_OptionalDoublePair = tuple[float | None, float | None]
_DoubleMatrix = tuple[_DoublePair, _DoublePair]
_OptionalDoubleMatrix = tuple[_OptionalDoublePair, _OptionalDoublePair]


def _pair(reader: CanonicalReader) -> _DoublePair:
    return reader.finite_double(), reader.finite_double()


def _optional_double_pair(reader: CanonicalReader) -> _OptionalDoublePair:
    return reader.optional(reader.finite_double), reader.optional(reader.finite_double)


def _optional_matrix(reader: CanonicalReader) -> _OptionalDoubleMatrix:
    return _optional_double_pair(reader), _optional_double_pair(reader)


def _load_matrix(reader: CanonicalReader) -> _DoubleMatrix:
    return _pair(reader), _pair(reader)


def _u64_vector(reader: CanonicalReader, field: str) -> tuple[int, ...]:
    return reader.vector(lambda _index: reader.u64(), field)


def _string_vector(reader: CanonicalReader, field: str) -> tuple[str, ...]:
    return reader.vector(lambda _index: reader.string(), field)


def _identity(reader: CanonicalReader) -> model.SnapshotIdentity:
    return model.SnapshotIdentity(
        reader.string(),
        reader.u64(),
        reader.u64(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.string(),
    )


def _node_key(reader: CanonicalReader) -> model.NodeKey:
    return model.NodeKey(
        reader.enum(model.NodeKind, "node_key.kind"),
        reader.string(),
        reader.string(),
        reader.string(),
    )


def _table_signature(reader: CanonicalReader) -> model.TimingTableSignature:
    return model.TimingTableSignature(
        reader.boolean(),
        reader.enum(model.TableVariable, "table_signature.variable1"),
        reader.enum(model.TableVariable, "table_signature.variable2"),
    )


def _library_signature(reader: CanonicalReader) -> model.LibraryArcSignature:
    return model.LibraryArcSignature(
        reader.string(),
        reader.i32(),
        reader.enum(model.TimingSense, "library_signature.sense"),
        reader.enum(model.Transition, "library_signature.trigger_transition"),
        reader.enum(model.Transition, "library_signature.check_transition"),
        reader.string(),
        (_table_signature(reader), _table_signature(reader)),
        (_table_signature(reader), _table_signature(reader)),
    )


def _arc_key(reader: CanonicalReader) -> model.ArcKey:
    return model.ArcKey(
        _node_key(reader),
        _node_key(reader),
        reader.enum(model.ArcKind, "arc_key.kind"),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.vector(lambda _index: _library_signature(reader), "arc_key.library_arcs"),
        reader.u64(),
    )


def _timing_table(reader: CanonicalReader) -> model.CandidateTimingTable:
    return model.CandidateTimingTable(
        reader.boolean(),
        reader.enum(model.TableVariable, "timing_table.variable1"),
        reader.enum(model.TableVariable, "timing_table.variable2"),
        reader.vector(
            lambda _index: reader.vector(
                lambda _axis_index: reader.finite_double(), "timing_table.axis"
            ),
            "timing_table.axes",
        ),
        reader.vector(lambda _index: reader.finite_double(), "timing_table.values"),
    )


def _child(reader: CanonicalReader) -> model.ChildTimingArc:
    return model.ChildTimingArc(
        _library_signature(reader),
        reader.finite_double(),
        reader.finite_double(),
        reader.finite_double(),
        _pair(reader),
        _pair(reader),
        _pair(reader),
        _pair(reader),
        (_timing_table(reader), _timing_table(reader)),
        (_timing_table(reader), _timing_table(reader)),
        (_timing_table(reader), _timing_table(reader)),
    )


def _transfer(reader: CanonicalReader) -> model.CellTransfer:
    return model.CellTransfer(
        reader.finite_double(),
        reader.finite_double(),
        reader.finite_double(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.vector(lambda _index: _child(reader), "transfer.children"),
    )


def _node(reader: CanonicalReader) -> model.CandidateNode:
    return model.CandidateNode(
        reader.u64(),
        _node_key(reader),
        reader.enum(model.Direction, "node.direction"),
        reader.string(),
        reader.string(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
    )


def _node_lookup(reader: CanonicalReader) -> model.NodeLookup:
    return model.NodeLookup(_node_key(reader), reader.u64())


def _instance(reader: CanonicalReader) -> model.CandidateInstance:
    return model.CandidateInstance(
        reader.string(),
        reader.string(),
        _u64_vector(reader, "instance.ordered_pins"),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
    )


def _eligibility(reader: CanonicalReader) -> model.ArcEligibility:
    return model.ArcEligibility(
        reader.boolean(), reader.boolean(), reader.boolean(), reader.boolean()
    )


def _arc(reader: CanonicalReader) -> model.CandidateArc:
    arc_id = reader.u64()
    key = _arc_key(reader)
    source, sink = reader.u64(), reader.u64()
    kind = reader.enum(model.ArcKind, "arc.kind")
    rank = reader.u64()
    ordinal = reader.optional(reader.u64)
    return model.CandidateArc(
        arc_id, key, source, sink, kind, rank,
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        _eligibility(reader),
        _transfer(reader),
        reader.optional(lambda: _load_matrix(reader)),
        ordinal,
    )


def _arc_lookup(reader: CanonicalReader) -> model.ArcLookup:
    return model.ArcLookup(_arc_key(reader), reader.u64())


def _raw_node_reference(reader: CanonicalReader) -> model.CandidateRawNodeReference:
    return model.CandidateRawNodeReference(reader.string(), reader.optional(reader.u64))


def _clock(reader: CanonicalReader) -> model.CandidateClockConstraint:
    return model.CandidateClockConstraint(
        reader.string(),
        reader.string(),
        reader.vector(lambda _index: _raw_node_reference(reader), "clock.sources"),
        reader.finite_double(),
        _pair(reader),
        reader.boolean(),
    )


def _port(reader: CanonicalReader) -> model.CandidatePortConstraint:
    return model.CandidatePortConstraint(
        reader.string(),
        reader.string(),
        reader.optional(reader.u64),
        _optional_double_pair(reader),
        reader.optional(reader.finite_double),
        _optional_double_pair(reader),
        reader.optional(reader.finite_double),
        reader.string(),
    )


def _case(reader: CanonicalReader) -> model.CandidateCaseConstraint:
    return model.CandidateCaseConstraint(
        reader.string(), reader.optional(reader.u64), reader.boolean()
    )


def _check_endpoint(reader: CanonicalReader) -> model.CandidateTimingCheckEndpoint:
    return model.CandidateTimingCheckEndpoint(
        reader.string(),
        reader.u64(),
        reader.enum(model.TimingCheckKind, "check_endpoint.kind"),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.optional(reader.u64),
        reader.optional(reader.u64),
    )


def _library_port(reader: CanonicalReader) -> model.CandidateLibraryPort:
    return model.CandidateLibraryPort(
        reader.string(),
        reader.enum(model.Direction, "library_port.direction"),
        reader.boolean(),
        reader.boolean(),
        reader.finite_double(),
        _optional_matrix(reader),
    )


def _library_cell_arc(reader: CanonicalReader) -> model.CandidateLibraryCellArc:
    return model.CandidateLibraryCellArc(reader.string(), reader.string(), _transfer(reader))


def _library_check(reader: CanonicalReader) -> model.CandidateLibraryTimingCheck:
    return model.CandidateLibraryTimingCheck(
        reader.string(),
        reader.string(),
        reader.enum(model.TimingCheckKind, "library_check.kind"),
        reader.finite_double(),
        reader.enum(model.Transition, "library_check.clock_transition"),
        reader.vector(lambda _index: _child(reader), "library_check.children"),
    )


def _library_cell(reader: CanonicalReader) -> model.CandidateReferencedLibraryCell:
    return model.CandidateReferencedLibraryCell(
        reader.string(),
        reader.boolean(),
        reader.string(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.finite_double(),
        (reader.finite_double(), reader.finite_double()),
        (reader.finite_double(), reader.finite_double()),
        (reader.finite_double(), reader.finite_double()),
        reader.vector(lambda _index: _library_port(reader), "library_cell.ports"),
        reader.vector(lambda _index: _library_cell_arc(reader), "library_cell.cell_arcs"),
        reader.vector(lambda _index: _library_check(reader), "library_cell.timing_checks"),
    )


def _parasitic_node(reader: CanonicalReader) -> model.RawParasiticNode:
    return model.RawParasiticNode(
        reader.string(),
        reader.string(),
        reader.finite_double(),
        reader.finite_double(),
        reader.finite_double(),
    )


def _parasitic_resistor(reader: CanonicalReader) -> model.RawParasiticResistor:
    return model.RawParasiticResistor(reader.string(), reader.string(), reader.finite_double())


def _parasitic_net(reader: CanonicalReader) -> model.CandidateParasiticNet:
    return model.CandidateParasiticNet(
        reader.string(),
        reader.string(),
        reader.finite_double(),
        reader.vector(lambda _index: _parasitic_node(reader), "parasitic_net.nodes"),
        reader.vector(lambda _index: _parasitic_resistor(reader), "parasitic_net.resistors"),
    )


def _parasitics(reader: CanonicalReader) -> model.CandidateParasitics:
    capacitance, resistance = reader.string(), reader.string()
    thresholds = model.InputPortRcThresholds(
        reader.finite_double(),
        (reader.finite_double(), reader.finite_double()),
        (reader.finite_double(), reader.finite_double()),
    )
    nets = reader.vector(lambda _index: _parasitic_net(reader), "parasitics.nets")
    return model.CandidateParasitics(capacitance, resistance, nets, thresholds)


def _start_seed(reader: CanonicalReader) -> model.CandidateStartSeed:
    return model.CandidateStartSeed(
        reader.u64(),
        reader.u64(),
        reader.enum(model.StartSeedAvailability, "start_seed.availability"),
        reader.optional(lambda: reader.enum(model.PathSource, "start_seed.path_source")),
        reader.string(),
        _optional_matrix(reader),
        _optional_matrix(reader),
        reader.optional(reader.u64),
        reader.optional(lambda: reader.enum(model.Transition, "start_seed.crpr_transition")),
    )


def _endpoint(reader: CanonicalReader) -> model.CandidateEndpointRequired:
    return model.CandidateEndpointRequired(
        reader.u64(), reader.string(), reader.finite_double(), _optional_double_pair(reader)
    )


def _pin_cap(reader: CanonicalReader) -> model.CandidatePinCapacitance:
    return model.CandidatePinCapacitance(
        reader.u64(), reader.finite_double(), _optional_matrix(reader)
    )


class _FullCandidateEncoder:
    def __init__(self) -> None:
        self.w = CanonicalWriter()

    def _strings(self, *values: str) -> None:
        for value in values:
            self.w.string(value)

    def _finite(self, *values: float) -> None:
        for value in values:
            self.w.finite_double(value)

    def _booleans(self, *values: bool) -> None:
        for value in values:
            self.w.boolean(value)

    def _pair(self, values: _DoublePair) -> None:
        for value in values:
            self.w.finite_double(value)

    def _optional_pair(self, values: _OptionalDoublePair) -> None:
        for value in values:
            self.w.optional(value, self.w.finite_double)

    def _optional_matrix(self, values: _OptionalDoubleMatrix) -> None:
        for row in values:
            self._optional_pair(row)

    def _matrix(self, values: _DoubleMatrix) -> None:
        for row in values:
            self._pair(row)

    def _node_key(self, key: model.NodeKey) -> None:
        self.w.enum(key.kind)
        self._strings(key.full_name, key.instance_name, key.pin_name)

    def _library_signature(self, signature: model.LibraryArcSignature) -> None:
        self.w.string(signature.library_name)
        self.w.i32(signature.arc_index)
        self.w.enum(signature.sense)
        self.w.enum(signature.trigger_transition)
        self.w.enum(signature.check_transition)
        self.w.string(signature.sdf_condition)
        for tables in (signature.delay_tables, signature.slew_tables):
            for table in tables:
                self.w.boolean(table.present)
                self.w.enum(table.variable1)
                self.w.enum(table.variable2)

    def _arc_key(self, key: model.ArcKey) -> None:
        self._node_key(key.source)
        self._node_key(key.sink)
        self.w.enum(key.kind)
        self._strings(key.owner_name, key.library_source_port, key.library_sink_port)
        self.w.vector(key.library_arcs, self._library_signature)
        self.w.u64(key.parallel_ordinal)

    def _timing_table(self, table: model.CandidateTimingTable) -> None:
        self.w.boolean(table.present)
        self.w.enum(table.variable1)
        self.w.enum(table.variable2)
        self.w.vector(table.axes, lambda axis: self.w.vector(axis, self.w.finite_double))
        self.w.vector(table.values, self.w.finite_double)

    def _child(self, child: model.ChildTimingArc) -> None:
        self._library_signature(child.signature)
        self._finite(child.time_unit_scale, child.capacitance_unit_scale, child.slew_derate)
        for pair in (
            child.slew_lower_threshold_pct,
            child.slew_upper_threshold_pct,
            child.input_threshold_pct,
            child.output_threshold_pct,
        ):
            self._pair(pair)
        for tables in (child.delay_tables, child.slew_tables, child.check_tables):
            for table in tables:
                self._timing_table(table)

    def _transfer(self, transfer: model.CellTransfer) -> None:
        self._finite(transfer.delay, transfer.delay_min, transfer.delay_max)
        self._booleans(transfer.is_clock_arc, transfer.is_disabled, transfer.is_clear_or_preset)
        self.w.vector(transfer.children, self._child)

    def _node(self, node: model.CandidateNode) -> None:
        self.w.u64(node.node_id)
        self._node_key(node.key)
        self.w.enum(node.direction)
        self._strings(node.instance_cell, node.net_name)
        self._booleans(
            node.is_port,
            node.is_startpoint,
            node.is_endpoint,
            node.is_sequential,
            node.is_clock_pin,
            node.is_data_pin,
            node.is_output_pin,
        )

    def _instance(self, instance: model.CandidateInstance) -> None:
        self._strings(instance.instance_name, instance.cell_name)
        self.w.vector(instance.ordered_pins, self.w.u64)
        self._booleans(
            instance.is_sequential,
            instance.has_timing_checks,
            instance.is_clock_gating,
            instance.is_macro,
        )

    def _arc(self, arc: model.CandidateArc) -> None:
        self.w.u64(arc.arc_id)
        self._arc_key(arc.key)
        self.w.u64(arc.source)
        self.w.u64(arc.sink)
        self.w.enum(arc.kind)
        self.w.u64(arc.native_traversal_rank)
        self.w.optional(arc.native_cell_arc_ordinal, self.w.u64)
        self._booleans(arc.is_clock_arc, arc.is_disabled, arc.is_loop_disabled)
        self._booleans(
            arc.eligibility.levelization,
            arc.eligibility.data_arrival,
            arc.eligibility.data_slew,
            arc.eligibility.clock,
        )
        self._transfer(arc.transfer)
        self.w.optional(arc.output_load, self._matrix)

    def _raw_node_reference(self, reference: model.CandidateRawNodeReference) -> None:
        self.w.string(reference.full_name)
        self.w.optional(reference.resolved_node, self.w.u64)

    def _clock(self, clock: model.CandidateClockConstraint) -> None:
        self._strings(clock.map_key, clock.embedded_clock_name)
        self.w.vector(clock.ordered_sources, self._raw_node_reference)
        self.w.finite_double(clock.period)
        self._pair(clock.edges)
        self.w.boolean(clock.propagated)

    def _port(self, port: model.CandidatePortConstraint) -> None:
        self._strings(port.map_key, port.embedded_port_name)
        self.w.optional(port.resolved_node, self.w.u64)
        self._optional_pair(port.input_delay)
        self.w.optional(port.input_transition, self.w.finite_double)
        self._optional_pair(port.output_delay)
        self.w.optional(port.output_load, self.w.finite_double)
        self.w.string(port.clock_name)

    def _case(self, constraint: model.CandidateCaseConstraint) -> None:
        self.w.string(constraint.raw_name)
        self.w.optional(constraint.resolved_node, self.w.u64)
        self.w.boolean(constraint.value)

    def _check_endpoint(self, endpoint: model.CandidateTimingCheckEndpoint) -> None:
        self.w.string(endpoint.instance_name)
        self.w.u64(endpoint.referenced_check_ordinal)
        self.w.enum(endpoint.check_kind)
        self._strings(endpoint.clock_pin_raw_name, endpoint.data_pin_raw_name)
        self._strings(endpoint.clock_pin_full_name, endpoint.data_pin_full_name)
        self.w.optional(endpoint.resolved_clock_node, self.w.u64)
        self.w.optional(endpoint.resolved_data_node, self.w.u64)

    def _library_port(self, port: model.CandidateLibraryPort) -> None:
        self.w.string(port.name)
        self.w.enum(port.direction)
        self._booleans(port.is_clock, port.is_output)
        self.w.finite_double(port.default_capacitance)
        self._optional_matrix(port.transition_capacitance)

    def _library_cell_arc(self, arc: model.CandidateLibraryCellArc) -> None:
        self._strings(arc.source_port, arc.sink_port)
        self._transfer(arc.transfer)

    def _library_check(self, check: model.CandidateLibraryTimingCheck) -> None:
        self._strings(check.clock_port, check.data_port)
        self.w.enum(check.kind)
        self.w.finite_double(check.check_time)
        self.w.enum(check.clock_transition)
        self.w.vector(check.children, self._child)

    def _library_cell(self, cell: model.CandidateReferencedLibraryCell) -> None:
        self.w.string(cell.cell_name)
        self.w.boolean(cell.present)
        self.w.string(cell.library_name)
        self._booleans(cell.is_sequential, cell.is_clock_gating, cell.is_macro)
        self._strings(cell.clock_pin_name, cell.data_pin_name, cell.output_pin_name)
        self._finite(cell.slew_derate, *cell.slew_lower_threshold_pct,
                     *cell.slew_upper_threshold_pct, *cell.input_threshold_pct)
        self.w.vector(cell.ports, self._library_port)
        self.w.vector(cell.cell_arcs, self._library_cell_arc)
        self.w.vector(cell.timing_checks, self._library_check)

    def _parasitic_node(self, node: model.RawParasiticNode) -> None:
        self._strings(node.map_key, node.embedded_name)
        self._finite(node.capacitance, node.x, node.y)

    def _parasitic_resistor(self, resistor: model.RawParasiticResistor) -> None:
        self._strings(resistor.source, resistor.sink)
        self.w.finite_double(resistor.resistance)

    def _parasitic_net(self, net: model.CandidateParasiticNet) -> None:
        self._strings(net.map_key, net.embedded_name)
        self.w.finite_double(net.lumped_capacitance)
        self.w.vector(net.nodes, self._parasitic_node)
        self.w.vector(net.resistors, self._parasitic_resistor)

    def _start_seed(self, seed: model.CandidateStartSeed) -> None:
        self.w.u64(seed.seed_node)
        self.w.u64(seed.path_start_node)
        self.w.enum(seed.availability)
        self.w.optional(seed.path_source, self.w.enum)
        self.w.string(seed.clock_tag)
        self._optional_matrix(seed.arrival)
        self._optional_matrix(seed.slew)
        self.w.optional(seed.crpr_node, self.w.u64)
        self.w.optional(seed.crpr_transition, self.w.enum)

    def _endpoint(self, endpoint: model.CandidateEndpointRequired) -> None:
        self.w.u64(endpoint.node)
        self.w.string(endpoint.clock_name)
        self.w.finite_double(endpoint.clock_period)
        self._optional_pair(endpoint.output_delay)

    def _pin_cap(self, capacitance: model.CandidatePinCapacitance) -> None:
        self.w.u64(capacitance.node)
        self.w.finite_double(capacitance.default_capacitance)
        self._optional_matrix(capacitance.transition_capacitance)

    def encode(self, candidate: model.CandidateInput) -> bytes:
        self.w.string(CANDIDATE_DOMAIN)
        self.w.string(candidate.schema_version)
        identity = candidate.source_identity
        self.w.string(identity.fingerprint_schema_version)
        self.w.u64(identity.session_token)
        self.w.u64(identity.graph_generation)
        self._strings(
            identity.topology_fingerprint,
            identity.clock_constraint_fingerprint,
            identity.library_fingerprint,
            identity.corner_fingerprint,
            identity.parasitic_fingerprint,
            identity.timing_input_fingerprint,
        )
        self.w.string(candidate.analysis_context.liberty_corner)
        self.w.string(candidate.analysis_context.temperature)
        self.w.string(candidate.analysis_context.rc_corner)
        for value in (
            *candidate.analysis_order,
            *candidate.path_source_order,
            *candidate.transition_order,
        ):
            self.w.enum(value)
        self.w.vector(candidate.nodes, self._node)
        self.w.vector(
            candidate.node_ids_by_key,
            lambda entry: (self._node_key(entry.key), self.w.u64(entry.node_id)),
        )
        self.w.vector(candidate.instances, self._instance)
        self.w.vector(candidate.arcs, self._arc)
        self.w.vector(
            candidate.arc_ids_by_key,
            lambda entry: (self._arc_key(entry.key), self.w.u64(entry.arc_id)),
        )
        for values in (
            candidate.out_row_offsets,
            candidate.out_arc_ids,
            candidate.in_row_offsets,
            candidate.in_arc_ids,
            candidate.ordered_root_ids,
            candidate.ordered_start_ids,
            candidate.ordered_end_ids,
        ):
            self.w.vector(values, self.w.u64)
        self.w.vector(candidate.clock_tags, self.w.string)
        self.w.vector(candidate.clocks, self._clock)
        self.w.vector(
            candidate.clock_owner_by_node,
            lambda owner: self.w.optional(owner, self.w.string),
        )
        self.w.vector(candidate.port_constraints, self._port)
        self.w.vector(candidate.case_analysis_constraints, self._case)
        self.w.vector(candidate.timing_check_endpoints, self._check_endpoint)
        self.w.vector(candidate.referenced_library_cells, self._library_cell)
        self.w.string(candidate.parasitics.capacitance_unit)
        self.w.string(candidate.parasitics.resistance_unit)
        thresholds = candidate.parasitics.input_port_thresholds
        self._finite(thresholds.slew_derate, *thresholds.lower, *thresholds.upper)
        self.w.vector(candidate.parasitics.nets, self._parasitic_net)
        self.w.vector(candidate.start_seeds, self._start_seed)
        self.w.vector(candidate.endpoint_required, self._endpoint)
        self.w.vector(candidate.cell_pin_capacitances, self._pin_cap)
        self.w.vector(candidate.unsupported_reasons, self.w.enum)
        return self.w.result()


def encode_full_candidate_wire(candidate: model.CandidateInput) -> bytes:
    if not isinstance(candidate, model.CandidateInput):
        raise CanonicalError("full candidate encoder requires CandidateInput")
    return _FullCandidateEncoder().encode(candidate)


def decode_candidate_wire(payload: bytes) -> model.CandidateInput:
    reader = CanonicalReader(payload, "reference input candidate wire")
    if reader.string() != CANDIDATE_DOMAIN:
        raise CanonicalError("reference input candidate wire domain/version mismatch")
    schema_version = reader.string()
    identity = _identity(reader)
    context = model.AnalysisContext(reader.string(), reader.string(), reader.string())
    analysis_order = (
        reader.enum(model.AnalysisMode, "analysis_order[0]"),
        reader.enum(model.AnalysisMode, "analysis_order[1]"),
    )
    path_order = (
        reader.enum(model.PathSource, "path_source_order[0]"),
        reader.enum(model.PathSource, "path_source_order[1]"),
    )
    transition_order = (
        reader.enum(model.Transition, "transition_order[0]"),
        reader.enum(model.Transition, "transition_order[1]"),
    )
    result = model.CandidateInput(
        schema_version,
        identity,
        context,
        analysis_order,
        path_order,
        transition_order,
        reader.vector(lambda _index: _node(reader), "nodes"),
        reader.vector(lambda _index: _node_lookup(reader), "node_ids_by_key"),
        reader.vector(lambda _index: _instance(reader), "instances"),
        reader.vector(lambda _index: _arc(reader), "arcs"),
        reader.vector(lambda _index: _arc_lookup(reader), "arc_ids_by_key"),
        _u64_vector(reader, "out_row_offsets"),
        _u64_vector(reader, "out_arc_ids"),
        _u64_vector(reader, "in_row_offsets"),
        _u64_vector(reader, "in_arc_ids"),
        _u64_vector(reader, "ordered_root_ids"),
        _u64_vector(reader, "ordered_start_ids"),
        _u64_vector(reader, "ordered_end_ids"),
        _string_vector(reader, "clock_tags"),
        reader.vector(lambda _index: _clock(reader), "clocks"),
        reader.vector(lambda _index: reader.optional(reader.string), "clock_owner_by_node"),
        reader.vector(lambda _index: _port(reader), "port_constraints"),
        reader.vector(lambda _index: _case(reader), "case_analysis_constraints"),
        reader.vector(lambda _index: _check_endpoint(reader), "timing_check_endpoints"),
        reader.vector(lambda _index: _library_cell(reader), "referenced_library_cells"),
        _parasitics(reader),
        reader.vector(lambda _index: _start_seed(reader), "start_seeds"),
        reader.vector(lambda _index: _endpoint(reader), "endpoint_required"),
        reader.vector(lambda _index: _pin_cap(reader), "cell_pin_capacitances"),
        reader.vector(
            lambda _index: reader.enum(model.UnsupportedReason, "unsupported_reason"),
            "unsupported_reasons",
        ),
    )
    reader.finish()
    return result
