#!/usr/bin/env python3
"""Independent strict codec for the derived-free native input projection."""
# ruff: noqa: FBT003

from reference_input_canonical import CanonicalError, CanonicalReader, CanonicalWriter
from reference_input_model import (
    ArcKind,
    Direction,
    RawArc,
    RawCaseConstraint,
    RawClockConstraint,
    RawInstance,
    RawLibraryArcChild,
    RawLibraryCellArc,
    RawLibraryCellPayload,
    RawLibraryCheckArc,
    RawLibraryPort,
    RawNet,
    RawNode,
    RawParasiticNet,
    RawParasiticNode,
    RawParasiticResistor,
    RawParasitics,
    RawPortConstraint,
    RawReferencedLibraryCell,
    RawReferenceInput,
    RawSelectedClockArc,
    RawTimingCheck,
    RawTimingTable,
    TableVariable,
    TimingCheckKind,
    TimingSense,
    Transition,
)

RAW_DOMAIN = "ista-csr-reference-input-raw/2.0.0"
EMPTY_RAW_HEX = (
    "0000000000000022697374612d6373722d7265666572656e63652d696e7075742d7261772f322e30"
    "2e300000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000"
)


def _write_optional_double(writer: CanonicalWriter, value: float | None) -> None:
    writer.optional(value, writer.finite_double)


def _write_fixed_pair(writer: CanonicalWriter, values: tuple[float, float]) -> None:
    if len(values) != 2:
        raise CanonicalError("fixed transition pair must contain exactly two values")
    for value in values:
        writer.finite_double(value)


def _read_fixed_pair(reader: CanonicalReader) -> tuple[float, float]:
    return reader.finite_double(), reader.finite_double()


def _write_table(writer: CanonicalWriter, table: RawTimingTable) -> None:
    writer.boolean(table.present)
    if not table.present:
        if (
            table.variable1 is not None
            or table.variable2 is not None
            or table.axis1
            or table.axis2
            or table.values
        ):
            raise CanonicalError("missing raw table carries present-only fields")
        return
    if table.variable1 is None or table.variable2 is None:
        raise CanonicalError("present raw table lacks variable tokens")
    writer.enum(table.variable1)
    writer.enum(table.variable2)
    writer.vector(table.axis1, writer.finite_double)
    writer.vector(table.axis2, writer.finite_double)
    writer.vector(table.values, writer.finite_double)


def _read_table(reader: CanonicalReader) -> RawTimingTable:
    if not reader.boolean():
        return RawTimingTable(False, None, None, (), (), ())
    return RawTimingTable(
        True,
        reader.enum(TableVariable, "table.variable1"),
        reader.enum(TableVariable, "table.variable2"),
        reader.vector(lambda _index: reader.finite_double(), "table.axis1"),
        reader.vector(lambda _index: reader.finite_double(), "table.axis2"),
        reader.vector(lambda _index: reader.finite_double(), "table.values"),
    )


def _write_node(writer: CanonicalWriter, node: RawNode) -> None:
    writer.string(node.map_key)
    writer.string(node.full_name)
    writer.boolean(node.is_port)
    writer.enum(node.direction)
    writer.string(node.instance_name)
    writer.vector(node.outgoing_arc_ranks, writer.u64)
    writer.vector(node.incoming_arc_ranks, writer.u64)


def _read_node(reader: CanonicalReader) -> RawNode:
    return RawNode(
        reader.string(),
        reader.string(),
        reader.boolean(),
        reader.enum(Direction, "node.direction"),
        reader.string(),
        reader.vector(lambda _index: reader.u64(), "node.outgoing_arc_ranks"),
        reader.vector(lambda _index: reader.u64(), "node.incoming_arc_ranks"),
    )


def _write_selected_clock_arc(writer: CanonicalWriter, selected: RawSelectedClockArc) -> None:
    writer.string(selected.source)
    writer.string(selected.sink)
    writer.vector(selected.ordered_child_triggers, writer.enum)


def _read_selected_clock_arc(reader: CanonicalReader) -> RawSelectedClockArc:
    return RawSelectedClockArc(
        reader.string(),
        reader.string(),
        reader.vector(
            lambda _index: reader.enum(Transition, "selected_clock_arc.trigger"),
            "selected_clock_arc.ordered_child_triggers",
        ),
    )


def _write_instance(writer: CanonicalWriter, instance: RawInstance) -> None:
    writer.string(instance.map_key)
    writer.string(instance.cell_name)
    writer.boolean(instance.sequential)
    writer.boolean(instance.gating)
    writer.boolean(instance.macro)
    writer.string(instance.clock_pin_name)
    writer.string(instance.data_pin_name)
    writer.string(instance.output_pin_name)
    writer.vector(instance.ordered_pin_names, writer.string)
    writer.optional(
        instance.selected_clock_arc, lambda value: _write_selected_clock_arc(writer, value)
    )


def _read_instance(reader: CanonicalReader) -> RawInstance:
    return RawInstance(
        reader.string(),
        reader.string(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.vector(lambda _index: reader.string(), "instance.ordered_pin_names"),
        reader.optional(lambda: _read_selected_clock_arc(reader)),
    )


def _write_arc(writer: CanonicalWriter, arc: RawArc) -> None:
    writer.u64(arc.native_rank)
    writer.string(arc.source)
    writer.string(arc.sink)
    writer.enum(arc.kind)
    writer.string(arc.owner)
    writer.string(arc.library_source_port)
    writer.string(arc.library_sink_port)
    writer.boolean(arc.is_clock_arc)
    writer.boolean(arc.disabled)
    writer.boolean(arc.loop_disabled)


def _read_arc(reader: CanonicalReader) -> RawArc:
    return RawArc(
        reader.u64(),
        reader.string(),
        reader.string(),
        reader.enum(ArcKind, "arc.kind"),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
    )


def _write_net(writer: CanonicalWriter, net: RawNet) -> None:
    writer.string(net.map_key)
    writer.string(net.embedded_name)
    writer.vector(net.ordered_drivers, writer.string)
    writer.vector(net.ordered_loads, writer.string)


def _read_net(reader: CanonicalReader) -> RawNet:
    return RawNet(
        reader.string(),
        reader.string(),
        reader.vector(lambda _index: reader.string(), "net.ordered_drivers"),
        reader.vector(lambda _index: reader.string(), "net.ordered_loads"),
    )


def _write_clock(writer: CanonicalWriter, clock: RawClockConstraint) -> None:
    writer.string(clock.map_key)
    writer.string(clock.embedded_name)
    writer.vector(clock.ordered_sources, writer.string)
    writer.finite_double(clock.period)
    writer.finite_double(clock.rise_edge)
    writer.finite_double(clock.fall_edge)
    writer.boolean(clock.propagated)


def _read_clock(reader: CanonicalReader) -> RawClockConstraint:
    return RawClockConstraint(
        reader.string(),
        reader.string(),
        reader.vector(lambda _index: reader.string(), "clock.ordered_sources"),
        reader.finite_double(),
        reader.finite_double(),
        reader.finite_double(),
        reader.boolean(),
    )


def _write_port(writer: CanonicalWriter, port: RawPortConstraint) -> None:
    writer.string(port.map_key)
    writer.string(port.embedded_port_name)
    writer.string(port.clock_name)
    for value in (
        port.input_delay_max,
        port.input_delay_min,
        port.input_transition,
        port.output_delay_max,
        port.output_delay_min,
        port.output_load,
    ):
        _write_optional_double(writer, value)


def _read_port(reader: CanonicalReader) -> RawPortConstraint:
    return RawPortConstraint(
        reader.string(),
        reader.string(),
        reader.string(),
        reader.optional(reader.finite_double),
        reader.optional(reader.finite_double),
        reader.optional(reader.finite_double),
        reader.optional(reader.finite_double),
        reader.optional(reader.finite_double),
        reader.optional(reader.finite_double),
    )


def _write_case(writer: CanonicalWriter, case: RawCaseConstraint) -> None:
    writer.string(case.map_key)
    writer.string(case.raw_name)
    writer.boolean(case.value)


def _read_case(reader: CanonicalReader) -> RawCaseConstraint:
    return RawCaseConstraint(reader.string(), reader.string(), reader.boolean())


def _write_check(writer: CanonicalWriter, check: RawTimingCheck) -> None:
    writer.string(check.instance_name)
    writer.string(check.cell_name)
    writer.u64(check.ordinal)
    writer.enum(check.kind)
    writer.string(check.raw_clock_name)
    writer.string(check.full_clock_name)
    writer.string(check.raw_data_name)
    writer.string(check.full_data_name)


def _read_check(reader: CanonicalReader) -> RawTimingCheck:
    return RawTimingCheck(
        reader.string(),
        reader.string(),
        reader.u64(),
        reader.enum(TimingCheckKind, "timing_check.kind"),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.string(),
    )


def _write_library_port(writer: CanonicalWriter, port: RawLibraryPort) -> None:
    writer.string(port.name)
    writer.enum(port.direction)
    writer.boolean(port.clock)
    writer.boolean(port.output)
    writer.finite_double(port.default_capacitance)
    if len(port.transition_capacitance) != 2 or any(
        len(row) != 2 for row in port.transition_capacitance
    ):
        raise CanonicalError("library port transition caps must be a 2x2 matrix")
    for row in port.transition_capacitance:
        for value in row:
            _write_optional_double(writer, value)


def _read_library_port(reader: CanonicalReader) -> RawLibraryPort:
    return RawLibraryPort(
        reader.string(),
        reader.enum(Direction, "library_port.direction"),
        reader.boolean(),
        reader.boolean(),
        reader.finite_double(),
        (
            (reader.optional(reader.finite_double), reader.optional(reader.finite_double)),
            (reader.optional(reader.finite_double), reader.optional(reader.finite_double)),
        ),
    )


def _write_library_child(writer: CanonicalWriter, child: RawLibraryArcChild) -> None:
    writer.string(child.library_name)
    writer.i32(child.index)
    writer.enum(child.sense)
    writer.enum(child.trigger_transition)
    writer.enum(child.check_transition)
    writer.string(child.sdf_condition)
    writer.finite_double(child.time_scale)
    writer.finite_double(child.capacitance_scale)
    writer.finite_double(child.slew_derate)
    for pair in (
        child.slew_lower_thresholds,
        child.slew_upper_thresholds,
        child.input_thresholds,
        child.output_thresholds,
    ):
        _write_fixed_pair(writer, pair)
    for table in (*child.delay_tables, *child.slew_tables, *child.check_tables):
        _write_table(writer, table)


def _read_library_child(reader: CanonicalReader) -> RawLibraryArcChild:
    return RawLibraryArcChild(
        reader.string(),
        reader.i32(),
        reader.enum(TimingSense, "library_child.sense"),
        reader.enum(Transition, "library_child.trigger_transition"),
        reader.enum(Transition, "library_child.check_transition"),
        reader.string(),
        reader.finite_double(),
        reader.finite_double(),
        reader.finite_double(),
        _read_fixed_pair(reader),
        _read_fixed_pair(reader),
        _read_fixed_pair(reader),
        _read_fixed_pair(reader),
        (_read_table(reader), _read_table(reader)),
        (_read_table(reader), _read_table(reader)),
        (_read_table(reader), _read_table(reader)),
    )


def _write_library_arc(writer: CanonicalWriter, arc: RawLibraryCellArc) -> None:
    writer.string(arc.source_port)
    writer.string(arc.sink_port)
    writer.finite_double(arc.constant_delay)
    writer.finite_double(arc.delay_min)
    writer.finite_double(arc.delay_max)
    writer.boolean(arc.is_clock_arc)
    writer.boolean(arc.disabled)
    writer.boolean(arc.is_clear_or_preset)
    writer.vector(arc.ordered_children, lambda value: _write_library_child(writer, value))


def _read_library_arc(reader: CanonicalReader) -> RawLibraryCellArc:
    return RawLibraryCellArc(
        reader.string(),
        reader.string(),
        reader.finite_double(),
        reader.finite_double(),
        reader.finite_double(),
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.vector(lambda _index: _read_library_child(reader), "library_arc.children"),
    )


def _write_library_check(writer: CanonicalWriter, check: RawLibraryCheckArc) -> None:
    writer.string(check.raw_clock_port)
    writer.string(check.raw_data_port)
    writer.enum(check.kind)
    writer.finite_double(check.check_time)
    writer.enum(check.clock_transition)
    writer.vector(check.ordered_children, lambda value: _write_library_child(writer, value))


def _read_library_check(reader: CanonicalReader) -> RawLibraryCheckArc:
    return RawLibraryCheckArc(
        reader.string(),
        reader.string(),
        reader.enum(TimingCheckKind, "library_check.kind"),
        reader.finite_double(),
        reader.enum(Transition, "library_check.clock_transition"),
        reader.vector(lambda _index: _read_library_child(reader), "library_check.children"),
    )


def _write_library_payload(writer: CanonicalWriter, payload: RawLibraryCellPayload) -> None:
    writer.boolean(payload.sequential)
    writer.boolean(payload.gating)
    writer.boolean(payload.macro)
    writer.string(payload.clock_pin_name)
    writer.string(payload.data_pin_name)
    writer.string(payload.output_pin_name)
    writer.string(payload.capacitance_unit)
    writer.string(payload.time_unit)
    writer.finite_double(payload.slew_derate)
    for pair in (
        payload.slew_lower_thresholds,
        payload.slew_upper_thresholds,
        payload.input_thresholds,
        payload.output_thresholds,
    ):
        _write_fixed_pair(writer, pair)
    writer.vector(payload.ports, lambda value: _write_library_port(writer, value))
    writer.vector(payload.cell_arcs, lambda value: _write_library_arc(writer, value))
    writer.vector(payload.check_arcs, lambda value: _write_library_check(writer, value))


def _read_library_payload(reader: CanonicalReader) -> RawLibraryCellPayload:
    return RawLibraryCellPayload(
        reader.boolean(),
        reader.boolean(),
        reader.boolean(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.finite_double(),
        _read_fixed_pair(reader),
        _read_fixed_pair(reader),
        _read_fixed_pair(reader),
        _read_fixed_pair(reader),
        reader.vector(lambda _index: _read_library_port(reader), "library.ports"),
        reader.vector(lambda _index: _read_library_arc(reader), "library.cell_arcs"),
        reader.vector(lambda _index: _read_library_check(reader), "library.check_arcs"),
    )


def _write_library_cell(writer: CanonicalWriter, cell: RawReferencedLibraryCell) -> None:
    writer.string(cell.library_name)
    writer.string(cell.cell_name)
    writer.optional(cell.payload, lambda value: _write_library_payload(writer, value))


def _read_library_cell(reader: CanonicalReader) -> RawReferencedLibraryCell:
    return RawReferencedLibraryCell(
        reader.string(), reader.string(), reader.optional(lambda: _read_library_payload(reader))
    )


def _write_parasitic_node(writer: CanonicalWriter, node: RawParasiticNode) -> None:
    writer.string(node.map_key)
    writer.string(node.embedded_name)
    writer.finite_double(node.capacitance)
    writer.finite_double(node.x)
    writer.finite_double(node.y)


def _read_parasitic_node(reader: CanonicalReader) -> RawParasiticNode:
    return RawParasiticNode(
        reader.string(),
        reader.string(),
        reader.finite_double(),
        reader.finite_double(),
        reader.finite_double(),
    )


def _write_resistor(writer: CanonicalWriter, resistor: RawParasiticResistor) -> None:
    writer.string(resistor.source)
    writer.string(resistor.sink)
    writer.finite_double(resistor.resistance)


def _read_resistor(reader: CanonicalReader) -> RawParasiticResistor:
    return RawParasiticResistor(reader.string(), reader.string(), reader.finite_double())


def _write_parasitic_net(writer: CanonicalWriter, net: RawParasiticNet) -> None:
    writer.string(net.map_key)
    writer.string(net.embedded_name)
    writer.finite_double(net.total_capacitance)
    writer.vector(net.nodes, lambda value: _write_parasitic_node(writer, value))
    writer.vector(net.resistors, lambda value: _write_resistor(writer, value))


def _read_parasitic_net(reader: CanonicalReader) -> RawParasiticNet:
    return RawParasiticNet(
        reader.string(),
        reader.string(),
        reader.finite_double(),
        reader.vector(lambda _index: _read_parasitic_node(reader), "parasitic_net.nodes"),
        reader.vector(lambda _index: _read_resistor(reader), "parasitic_net.resistors"),
    )


def _write_parasitics(writer: CanonicalWriter, parasitics: RawParasitics) -> None:
    writer.string(parasitics.source_path)
    writer.string(parasitics.capacitance_unit)
    writer.string(parasitics.resistance_unit)
    writer.vector(parasitics.nets, lambda value: _write_parasitic_net(writer, value))


def _read_parasitics(reader: CanonicalReader) -> RawParasitics:
    return RawParasitics(
        reader.string(),
        reader.string(),
        reader.string(),
        reader.vector(lambda _index: _read_parasitic_net(reader), "parasitics.nets"),
    )


def encode_raw_wire(raw: RawReferenceInput) -> bytes:
    writer = CanonicalWriter()
    writer.string(RAW_DOMAIN)
    writers = (
        (raw.nodes, _write_node),
        (raw.instances, _write_instance),
        (raw.arcs, _write_arc),
        (raw.nets, _write_net),
        (raw.clock_constraints, _write_clock),
        (raw.port_constraints, _write_port),
        (raw.case_constraints, _write_case),
        (raw.timing_checks, _write_check),
        (raw.referenced_library_cells, _write_library_cell),
    )
    for values, write_item in writers:
        writer.vector(values, lambda value, function=write_item: function(writer, value))
    writer.optional(raw.parasitics, lambda value: _write_parasitics(writer, value))
    return writer.result()


def decode_raw_wire(payload: bytes) -> RawReferenceInput:
    reader = CanonicalReader(payload, "reference input raw wire")
    if reader.string() != RAW_DOMAIN:
        raise CanonicalError("reference input raw wire domain/version mismatch")
    result = RawReferenceInput(
        reader.vector(lambda _index: _read_node(reader), "nodes"),
        reader.vector(lambda _index: _read_instance(reader), "instances"),
        reader.vector(lambda _index: _read_arc(reader), "arcs"),
        reader.vector(lambda _index: _read_net(reader), "nets"),
        reader.vector(lambda _index: _read_clock(reader), "clock_constraints"),
        reader.vector(lambda _index: _read_port(reader), "port_constraints"),
        reader.vector(lambda _index: _read_case(reader), "case_constraints"),
        reader.vector(lambda _index: _read_check(reader), "timing_checks"),
        reader.vector(lambda _index: _read_library_cell(reader), "referenced_library_cells"),
        reader.optional(lambda: _read_parasitics(reader)),
    )
    reader.finish()
    return result


def empty_raw_input() -> RawReferenceInput:
    return RawReferenceInput((), (), (), (), (), (), (), (), (), None)
