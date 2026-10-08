#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# ***************************************************************************************
"""Independent Liberty, transfer, capacitance, load, and parasitic reconstruction."""

import math
from dataclasses import dataclass
from typing import NoReturn

import reference_input_model as model
import reference_input_recompute_graph as graph_oracle
from reference_input_canonical import CanonicalError


@dataclass(frozen=True)
class TransferFacts:
    raw_cells: dict[str, model.RawReferencedLibraryCell]
    capacitances: tuple[model.CandidatePinCapacitance, ...]
    loads: tuple[tuple[tuple[float, float], tuple[float, float]] | None, ...]


def _fail(message: str) -> NoReturn:
    raise CanonicalError(f"reference-input transfer recomputation: {message}")


def _byte_key(value: str) -> bytes:
    return value.encode("utf-8", errors="strict")


def _finite(value: float, field: str) -> None:
    if type(value) is not float or not math.isfinite(value):
        _fail(f"{field} is not finite")


def _nonnegative(value: float, field: str) -> None:
    _finite(value, field)
    if value < 0.0:
        _fail(f"{field} is negative")


def _thresholds(
    lower: tuple[float, float],
    upper: tuple[float, float],
    input_threshold: tuple[float, float],
    output_threshold: tuple[float, float],
    field: str,
) -> None:
    if any(len(values) != 2 for values in (lower, upper, input_threshold, output_threshold)):
        _fail(f"{field} does not contain four transition pairs")
    for transition in range(2):
        values = (
            lower[transition],
            upper[transition],
            input_threshold[transition],
            output_threshold[transition],
        )
        for value in values:
            _finite(value, f"{field}[{transition}]")
        if not 0.0 <= values[0] < values[1] <= 1.0:
            _fail(f"{field} slew thresholds are invalid")
        if not 0.0 <= values[2] <= 1.0 or not 0.0 <= values[3] <= 1.0:
            _fail(f"{field} input/output threshold is invalid")


def _table(raw: model.RawTimingTable, field: str) -> model.CandidateTimingTable:
    if not raw.present:
        if (
            raw.variable1 is not None
            or raw.variable2 is not None
            or raw.axis1
            or raw.axis2
            or raw.values
        ):
            _fail(f"{field} missing table retains present-only data")
        return model.CandidateTimingTable(
            raw.present, model.TableVariable.NONE, model.TableVariable.NONE, (), ()
        )
    if raw.variable1 is None or raw.variable2 is None:
        _fail(f"{field} present table lacks variable tokens")
    if raw.variable1 == model.TableVariable.NONE:
        if raw.variable2 != model.TableVariable.NONE:
            _fail(f"{field} has variable2 without variable1")
        dimension = 0
    else:
        dimension = 1 if raw.variable2 == model.TableVariable.NONE else 2
    if dimension < 2 and raw.axis2:
        _fail(f"{field} has an unused second axis")
    axes = () if dimension == 0 else (raw.axis1,) if dimension == 1 else (raw.axis1, raw.axis2)
    for axis_index, axis in enumerate(axes):
        for value in axis:
            _finite(value, f"{field}.axis[{axis_index}]")
        if any(lhs >= rhs for lhs, rhs in zip(axis, axis[1:], strict=False)):
            _fail(f"{field}.axis[{axis_index}] is not strictly increasing")
    for value in raw.values:
        _finite(value, f"{field}.values")
    if raw.values:
        if dimension == 0 and len(raw.values) != 1:
            _fail(f"{field} constant table has more than one value")
        if dimension != 0:
            product = 1
            for axis in axes:
                if not axis:
                    _fail(f"{field} populated table has an empty axis")
                product *= len(axis)
            if len(raw.values) != product:
                _fail(f"{field} value count differs from its axis product")
    return model.CandidateTimingTable(raw.present, raw.variable1, raw.variable2, axes, raw.values)


def _signature(table: model.RawTimingTable, field: str) -> model.TimingTableSignature:
    translated = _table(table, field)
    return model.TimingTableSignature(
        translated.present, translated.variable1, translated.variable2
    )


def _child(raw: model.RawLibraryArcChild, field: str) -> model.ChildTimingArc:
    for name, value in (
        ("time_scale", raw.time_scale),
        ("capacitance_scale", raw.capacitance_scale),
        ("slew_derate", raw.slew_derate),
    ):
        _finite(value, f"{field}.{name}")
        if value <= 0.0:
            _fail(f"{field}.{name} is not positive")
    _thresholds(
        raw.slew_lower_thresholds,
        raw.slew_upper_thresholds,
        raw.input_thresholds,
        raw.output_thresholds,
        f"{field}.thresholds",
    )
    families = tuple(
        tuple(_table(table, f"{field}.{family}[{index}]") for index, table in enumerate(tables))
        for family, tables in (
            ("delay", raw.delay_tables),
            ("slew", raw.slew_tables),
            ("check", raw.check_tables),
        )
    )
    if any(len(tables) != 2 for tables in families):
        _fail(f"{field} does not contain two transition tables per family")
    signature = model.LibraryArcSignature(
        raw.library_name,
        raw.index,
        raw.sense,
        raw.trigger_transition,
        raw.check_transition,
        raw.sdf_condition,
        tuple(
            _signature(table, f"{field}.delay_signature[{index}]")
            for index, table in enumerate(raw.delay_tables)
        ),
        tuple(
            _signature(table, f"{field}.slew_signature[{index}]")
            for index, table in enumerate(raw.slew_tables)
        ),
    )
    return model.ChildTimingArc(
        signature,
        raw.time_scale,
        raw.capacitance_scale,
        raw.slew_derate,
        raw.slew_lower_thresholds,
        raw.slew_upper_thresholds,
        raw.input_thresholds,
        raw.output_thresholds,
        families[0],
        families[1],
        families[2],
    )


def _transfer(raw: model.RawLibraryCellArc, field: str) -> model.CellTransfer:
    for name, value in (
        ("delay", raw.constant_delay),
        ("delay_min", raw.delay_min),
        ("delay_max", raw.delay_max),
    ):
        _finite(value, f"{field}.{name}")
    children = tuple(
        _child(child, f"{field}.children[{index}]")
        for index, child in enumerate(raw.ordered_children)
    )
    indices = [child.signature.arc_index for child in children]
    if len(indices) != len(set(indices)):
        _fail(f"{field} child arc indices are not unique")
    return model.CellTransfer(
        raw.constant_delay,
        raw.delay_min,
        raw.delay_max,
        raw.is_clock_arc,
        raw.disabled,
        raw.is_clear_or_preset,
        children,
    )


def _library_port(raw: model.RawLibraryPort, field: str) -> model.CandidateLibraryPort:
    _nonnegative(raw.default_capacitance, f"{field}.default_capacitance")
    if len(raw.transition_capacitance) != 2 or any(
        len(row) != 2 for row in raw.transition_capacitance
    ):
        _fail(f"{field}.transition_capacitance is not 2x2")
    for row in raw.transition_capacitance:
        for value in row:
            if value is not None:
                _nonnegative(value, f"{field}.transition_capacitance")
    output_direction = raw.direction in (model.Direction.OUTPUT, model.Direction.INOUT)
    if raw.output != output_direction:
        _fail(f"{field}.output differs from its direction")
    return model.CandidateLibraryPort(
        raw.name,
        raw.direction,
        raw.clock,
        raw.output,
        raw.default_capacitance,
        ((raw.transition_capacitance[0][0], raw.transition_capacitance[0][1]),
         (raw.transition_capacitance[1][0], raw.transition_capacitance[1][1])),
    )


def _library_cell(
    raw: model.RawReferencedLibraryCell, field: str
) -> model.CandidateReferencedLibraryCell:
    payload = raw.payload
    if payload is None:
        if raw.library_name:
            _fail(f"{field} missing cell retains a library name")
        return model.CandidateReferencedLibraryCell(
            cell_name=raw.cell_name,
            present=False,
            library_name="",
            is_sequential=False,
            is_clock_gating=False,
            is_macro=False,
            clock_pin_name="",
            data_pin_name="",
            output_pin_name="",
            slew_derate=1.0,
            slew_lower_threshold_pct=(0.3, 0.3),
            slew_upper_threshold_pct=(0.7, 0.7),
            input_threshold_pct=(0.5, 0.5),
            ports=(),
            cell_arcs=(),
            timing_checks=(),
        )
    if not raw.library_name or not payload.capacitance_unit or not payload.time_unit:
        _fail(f"{field} present cell lacks library/unit identity")
    _finite(payload.slew_derate, f"{field}.slew_derate")
    # Native load-threshold adjustment normalizes percentages and skips
    # degenerate spans/derates. This differs from a driver's table model.
    def load_thresholds(values: tuple[float, float]) -> tuple[float, float]:
        normalized = tuple(value * 0.01 if value > 1.0 else value for value in values)
        for value in normalized:
            _finite(value, f"{field}.load_threshold")
            if not 0.0 <= value <= 1.0:
                _fail(f"{field}.load_threshold is outside [0, 1]")
        return normalized[0], normalized[1]
    lower = load_thresholds(payload.slew_lower_thresholds)
    upper = load_thresholds(payload.slew_upper_thresholds)
    inputs = load_thresholds(payload.input_thresholds)
    if tuple(port.name for port in payload.ports) != tuple(
        sorted((port.name for port in payload.ports), key=_byte_key)
    ) or len({port.name for port in payload.ports}) != len(payload.ports):
        _fail(f"{field}.ports are not unique native byte order")
    ports = tuple(
        _library_port(port, f"{field}.ports[{index}]") for index, port in enumerate(payload.ports)
    )
    port_names = {port.name for port in payload.ports}
    if any(not arc.source_port or not arc.sink_port for arc in payload.cell_arcs):
        _fail(f"{field}.cell_arcs contain an empty port")
    if any(
        arc.source_port not in port_names or arc.sink_port not in port_names
        for arc in payload.cell_arcs
    ):
        _fail(f"{field}.cell_arcs contain an unresolved port")
    cell_arcs = tuple(
        model.CandidateLibraryCellArc(
            arc.source_port,
            arc.sink_port,
            _transfer(arc, f"{field}.cell_arcs[{index}]"),
        )
        for index, arc in enumerate(payload.cell_arcs)
    )
    checks: list[model.CandidateLibraryTimingCheck] = []
    for index, check in enumerate(payload.check_arcs):
        _finite(check.check_time, f"{field}.checks[{index}].check_time")
        if check.clock_transition == model.Transition.NONE:
            _fail(f"{field}.checks[{index}] has no clock transition")
        checks.append(
            model.CandidateLibraryTimingCheck(
                check.raw_clock_port,
                check.raw_data_port,
                check.kind,
                check.check_time,
                check.clock_transition,
                tuple(
                    _child(child, f"{field}.checks[{index}].children[{child_index}]")
                    for child_index, child in enumerate(check.ordered_children)
                ),
            )
        )
    representative = next(
        (check for check in payload.check_arcs if check.kind == model.TimingCheckKind.SETUP),
        payload.check_arcs[0] if payload.check_arcs else None,
    )
    expected_clock = "" if representative is None else representative.raw_clock_port
    expected_data = "" if representative is None else representative.raw_data_port
    clock_arc = next((arc for arc in payload.cell_arcs if arc.is_clock_arc), None)
    if clock_arc is not None:
        expected_output = clock_arc.sink_port
    else:
        output_port = next((port for port in payload.ports if port.output and not port.clock), None)
        expected_output = "" if output_port is None else output_port.name
    if (
        payload.clock_pin_name,
        payload.data_pin_name,
        payload.output_pin_name,
    ) != (expected_clock, expected_data, expected_output):
        _fail(f"{field} timing roles differ from native first-role derivation")
    return model.CandidateReferencedLibraryCell(
        cell_name=raw.cell_name,
        present=True,
        library_name=raw.library_name,
        is_sequential=payload.sequential,
        is_clock_gating=payload.gating,
        is_macro=payload.macro,
        clock_pin_name=payload.clock_pin_name,
        data_pin_name=payload.data_pin_name,
        output_pin_name=payload.output_pin_name,
        slew_derate=payload.slew_derate,
        slew_lower_threshold_pct=lower,
        slew_upper_threshold_pct=upper,
        input_threshold_pct=inputs,
        ports=ports,
        cell_arcs=cell_arcs,
        timing_checks=tuple(checks),
    )


def _validate_library(
    candidate: model.CandidateInput, raw: model.RawReferenceInput
) -> dict[str, model.RawReferencedLibraryCell]:
    expected_names = tuple(
        sorted({instance.cell_name for instance in raw.instances}, key=_byte_key)
    )
    if tuple(cell.cell_name for cell in raw.referenced_library_cells) != expected_names:
        _fail("raw referenced cells are not the exact used-cell byte-order closure")
    expected = tuple(
        _library_cell(cell, f"referenced_library_cells[{index}]")
        for index, cell in enumerate(raw.referenced_library_cells)
    )
    if candidate.referenced_library_cells != expected:
        _fail("candidate referenced Liberty closure differs from raw projection")
    cells = {cell.cell_name: cell for cell in raw.referenced_library_cells}
    instances = {instance.map_key: instance for instance in raw.instances}
    for index, check in enumerate(raw.timing_checks):
        cell = cells.get(check.cell_name)
        instance = instances.get(check.instance_name)
        if cell is None or instance is None or instance.cell_name != check.cell_name:
            _fail(f"raw timing check {index} has an invalid cell/instance binding")
        if cell.payload is None or check.ordinal >= len(cell.payload.check_arcs):
            _fail(f"raw timing check {index} has an invalid closure ordinal")
        closure = cell.payload.check_arcs[check.ordinal]
        if (
            check.kind != closure.kind
            or check.raw_clock_name != closure.raw_clock_port
            or check.raw_data_name != closure.raw_data_port
            or check.full_clock_name != f"{check.instance_name}:{check.raw_clock_name}"
            or check.full_data_name != f"{check.instance_name}:{check.raw_data_name}"
        ):
            _fail(f"raw timing check {index} differs from its closure ordinal")
    raw_nodes = {node.full_name: node for node in raw.nodes}
    for instance in raw.instances:
        cell = cells[instance.cell_name]
        payload = cell.payload
        expected = (
            (
                False,
                False,
                False,
                "",
                "",
                "",
            )
            if payload is None
            else (
                payload.sequential,
                payload.gating,
                payload.macro,
                (f"{instance.map_key}:{payload.clock_pin_name}" if payload.clock_pin_name else ""),
                f"{instance.map_key}:{payload.data_pin_name}" if payload.data_pin_name else "",
                (
                    f"{instance.map_key}:{payload.output_pin_name}"
                    if payload.output_pin_name
                    else ""
                ),
            )
        )
        actual = (
            instance.sequential,
            instance.gating,
            instance.macro,
            instance.clock_pin_name,
            instance.data_pin_name,
            instance.output_pin_name,
        )
        if actual != expected:
            _fail(f"raw instance {instance.map_key!r} differs from its library cell")
        if payload is not None:
            ports = {port.name: port for port in payload.ports}
            for full_name in instance.ordered_pin_names:
                node = raw_nodes[full_name]
                local = full_name.removeprefix(f"{instance.map_key}:")
                if local in ports and node.direction != ports[local].direction:
                    _fail(f"raw pin {full_name!r} direction differs from its library port")
    return cells


def _validate_fallbacks(
    candidate: model.CandidateInput,
    raw_cells: dict[str, model.RawReferencedLibraryCell],
) -> None:
    fallback_instances = {
        instance.instance_name: instance
        for instance in candidate.instances
        if raw_cells[instance.cell_name].payload is None
        or not raw_cells[instance.cell_name].payload.cell_arcs
    }
    expected = {
        (instance.instance_name, source, sink)
        for instance in fallback_instances.values()
        for source in instance.ordered_pins
        for sink in instance.ordered_pins
        if source != sink
        and candidate.nodes[source].direction in (model.Direction.INPUT, model.Direction.INOUT)
        and candidate.nodes[sink].direction in (model.Direction.OUTPUT, model.Direction.INOUT)
    }
    actual: set[tuple[str, int, int]] = set()
    for arc in candidate.arcs:
        owner = candidate.nodes[arc.source].key.instance_name
        if arc.kind != model.ArcKind.CELL or owner not in fallback_instances:
            continue
        valid = (
            owner == candidate.nodes[arc.sink].key.instance_name
            and candidate.nodes[arc.source].direction
            in (model.Direction.INPUT, model.Direction.INOUT)
            and candidate.nodes[arc.sink].direction
            in (model.Direction.OUTPUT, model.Direction.INOUT)
            and arc.transfer
            == model.CellTransfer(
                delay=1.0,
                delay_min=1.0,
                delay_max=1.0,
                is_clock_arc=arc.is_clock_arc,
                is_disabled=arc.is_disabled,
                is_clear_or_preset=False,
                children=(),
            )
        )
        if not valid or (owner, arc.source, arc.sink) in actual:
            _fail("fallback cell arc is not one unique input-like/output-like unit transfer")
        actual.add((owner, arc.source, arc.sink))
    if actual != expected:
        _fail("fallback cell arcs differ from the exact per-instance Cartesian product")


def _graph_transfers(
    candidate: model.CandidateInput,
    raw_cells: dict[str, model.RawReferencedLibraryCell],
    facts: graph_oracle.GraphFacts,
) -> None:
    _validate_fallbacks(candidate, raw_cells)
    result: list[model.CellTransfer | None] = [None] * len(candidate.arcs)
    arc_id_by_rank = {arc.native_traversal_rank: arc.arc_id for arc in candidate.arcs}
    for raw_arc in facts.raw_arc_by_rank:
        arc_id = arc_id_by_rank[raw_arc.native_rank]
        if raw_arc.kind == model.ArcKind.NET:
            expected = model.CellTransfer(
                delay=0.0,
                delay_min=0.0,
                delay_max=0.0,
                is_clock_arc=False,
                is_disabled=False,
                is_clear_or_preset=False,
                children=(),
            )
        else:
            instance = facts.raw_instance_by_name.get(raw_arc.owner)
            if instance is None:
                _fail(f"raw cell arc {raw_arc.native_rank} has no owning instance")
            cell = raw_cells.get(instance.cell_name)
            payload = None if cell is None else cell.payload
            if payload is None or not payload.cell_arcs:
                expected = model.CellTransfer(
                    delay=1.0,
                    delay_min=1.0,
                    delay_max=1.0,
                    is_clock_arc=raw_arc.is_clock_arc,
                    is_disabled=raw_arc.disabled,
                    is_clear_or_preset=False,
                    children=(),
                )
            else:
                ordinal = raw_arc.native_cell_arc_ordinal
                if ordinal is None or not 0 <= ordinal < len(payload.cell_arcs):
                    _fail(f"raw cell arc {raw_arc.native_rank} lacks a valid Liberty ordinal")
                closure = payload.cell_arcs[ordinal]
                if (closure.source_port != raw_arc.library_source_port
                        or closure.sink_port != raw_arc.library_sink_port):
                    _fail(f"raw cell arc {raw_arc.native_rank} ordinal has wrong endpoints")
                expected = _transfer(closure, f"graph_transfer_by_rank[{raw_arc.native_rank}]")
                if (tuple(child.signature for child in expected.children)
                        != candidate.arcs[arc_id].key.library_arcs):
                    _fail(
                        f"candidate graph transfer {raw_arc.native_rank} "
                        "signature differs from its ordinal"
                    )
                if candidate.arcs[arc_id].transfer != expected:
                    _fail(
                        f"candidate graph transfer {raw_arc.native_rank} is not its matched closure"
                    )
        if candidate.arcs[arc_id].native_cell_arc_ordinal != raw_arc.native_cell_arc_ordinal:
            _fail(f"candidate cell arc {raw_arc.native_rank} identity differs from raw")
        if raw_arc.kind == model.ArcKind.NET and raw_arc.native_cell_arc_ordinal is not None:
            _fail("raw net arc has a Liberty identity")
        result[arc_id] = expected
    if any(item is None for item in result):
        _fail("raw graph transfers do not cover every candidate arc")
    if tuple(arc.transfer for arc in candidate.arcs) != tuple(result):
        _fail("candidate graph transfers differ from raw Liberty projection")


def index_port_constraints(
    candidate: model.CandidateInput,
) -> dict[int, model.CandidatePortConstraint]:
    result: dict[int, model.CandidatePortConstraint] = {}
    for constraint in candidate.port_constraints:
        if constraint.resolved_node is not None:
            if constraint.resolved_node in result:
                _fail("multiple port constraints resolve to one node")
            result[constraint.resolved_node] = constraint
    return result


def _capacitances(
    candidate: model.CandidateInput,
    facts: graph_oracle.GraphFacts,
    raw_cells: dict[str, model.RawReferencedLibraryCell],
) -> tuple[model.CandidatePinCapacitance, ...]:
    result: list[model.CandidatePinCapacitance] = []
    order = sorted(
        (node.node_id for node in candidate.nodes if not node.is_port),
        key=lambda node_id: _byte_key(candidate.nodes[node_id].key.full_name),
    )
    for node_id in order:
        node = candidate.nodes[node_id]
        instance = facts.raw_instance_by_name[node.key.instance_name]
        cell = raw_cells[instance.cell_name]
        port = (
            None
            if cell.payload is None
            else next(
                (item for item in cell.payload.ports if item.name == node.key.pin_name),
                None,
            )
        )
        result.append(
            model.CandidatePinCapacitance(
                node_id,
                0.0 if port is None else port.default_capacitance,
                ((None, None), (None, None)) if port is None else port.transition_capacitance,
            )
        )
    expected = tuple(result)
    if candidate.cell_pin_capacitances != expected:
        _fail("candidate cell-pin capacitances differ from raw Liberty ports")
    return expected


def _loads(
    candidate: model.CandidateInput,
    raw: model.RawReferenceInput,
    facts: graph_oracle.GraphFacts,
    capacitances: tuple[model.CandidatePinCapacitance, ...],
) -> tuple[tuple[tuple[float, float], tuple[float, float]] | None, ...]:
    constraints = index_port_constraints(candidate)
    caps = {item.node: item for item in capacitances}
    net_loads: dict[str, tuple[tuple[float, float], tuple[float, float]]] = {}
    rc_nets = {net.map_key for net in raw.parasitics.nets} if raw.parasitics else set()
    for net in raw.nets:
        if net.map_key in rc_nets:
            continue
        matrix = [[0.0, 0.0], [0.0, 0.0]]
        for name in net.ordered_loads:
            node_id = facts.node_by_name.get(name)
            if node_id is None:
                _fail(f"raw net {net.map_key!r} has unresolved load {name!r}")
            node = candidate.nodes[node_id]
            if node.is_port:
                if node.direction not in (model.Direction.OUTPUT, model.Direction.INOUT):
                    continue
                constraint = constraints.get(node_id)
                value = (
                    0.0
                    if constraint is None or constraint.output_load is None
                    else constraint.output_load
                )
                for row in matrix:
                    for transition in range(2):
                        row[transition] += value
            else:
                cap = caps.get(node_id)
                if cap is None:
                    _fail(f"raw load {name!r} lacks a pin capacitance")
                for analysis in range(2):
                    for transition in range(2):
                        transition_cap = cap.transition_capacitance[analysis][transition]
                        matrix[analysis][transition] += (
                            cap.default_capacitance if transition_cap is None else transition_cap
                        )
        for row in matrix:
            for value in row:
                _nonnegative(value, f"raw net {net.map_key!r} load")
        net_loads[net.map_key] = (
            (matrix[0][0], matrix[0][1]),
            (matrix[1][0], matrix[1][1]),
        )
    zero = ((0.0, 0.0), (0.0, 0.0))
    expected = tuple(
        None if candidate.nodes[arc.sink].net_name in rc_nets
        else net_loads.get(candidate.nodes[arc.sink].net_name, zero)
        for arc in candidate.arcs
    )
    if tuple(arc.output_load for arc in candidate.arcs) != expected:
        _fail("candidate output loads differ from raw ordered net loads")
    return expected


def _parasitics(candidate: model.CandidateInput, raw: model.RawReferenceInput) -> None:
    source = raw.parasitics
    if source is None or not source.nets:
        expected = model.CandidateParasitics("", "", ())
    else:
        if not source.capacitance_unit or not source.resistance_unit:
            _fail("nonempty raw parasitics lack unit strings")
        if tuple(net.map_key for net in source.nets) != tuple(
            sorted((net.map_key for net in source.nets), key=_byte_key)
        ):
            _fail("raw parasitic nets are not in native byte order")
        nets: list[model.CandidateParasiticNet] = []
        for net_index, net in enumerate(source.nets):
            _nonnegative(net.total_capacitance, f"parasitic.net[{net_index}].total")
            if tuple(node.map_key for node in net.nodes) != tuple(
                sorted((node.map_key for node in net.nodes), key=_byte_key)
            ):
                _fail(f"parasitic.net[{net_index}] nodes are not native byte order")
            for node in net.nodes:
                _nonnegative(node.capacitance, f"parasitic.net[{net_index}].node.cap")
                _finite(node.x, f"parasitic.net[{net_index}].node.x")
                _finite(node.y, f"parasitic.net[{net_index}].node.y")
            for resistor in net.resistors:
                _nonnegative(resistor.resistance, f"parasitic.net[{net_index}].resistance")
            nets.append(
                model.CandidateParasiticNet(
                    net.map_key,
                    net.embedded_name,
                    net.total_capacitance,
                    net.nodes,
                    net.resistors,
                )
            )
        expected = model.CandidateParasitics(
            source.capacitance_unit,
            source.resistance_unit,
            tuple(nets),
            source.input_port_thresholds,
        )
    if candidate.parasitics != expected:
        _fail("candidate parasitic semantic differs from normalized raw projection")


def _preflight_converted_loads(candidate: model.CandidateInput) -> None:
    native_int_max = (1 << 31) - 1
    for arc in candidate.arcs:
        if arc.output_load is None:
            continue
        for child in arc.transfer.children:
            if abs(child.capacitance_unit_scale - 1.0) < 1e-6:
                continue
            for row in arc.output_load:
                for load in row:
                    converted = math.ceil(load * child.capacitance_unit_scale)
                    if not math.isfinite(converted) or converted < 0 or converted > native_int_max:
                        _fail("converted load is outside the native table-index range")


def recompute_transfer(
    candidate: model.CandidateInput,
    raw: model.RawReferenceInput,
    facts: graph_oracle.GraphFacts,
) -> TransferFacts:
    """Validate complete closure, transfer, capacitance, load, and RC semantics."""
    raw_cells = _validate_library(candidate, raw)
    _graph_transfers(candidate, raw_cells, facts)
    _parasitics(candidate, raw)
    capacitances = _capacitances(candidate, facts, raw_cells)
    loads = _loads(candidate, raw, facts, capacitances)
    _preflight_converted_loads(candidate)
    return TransferFacts(raw_cells, capacitances, loads)
