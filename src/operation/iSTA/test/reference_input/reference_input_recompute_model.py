#!/usr/bin/env python3
"""Independent seed, endpoint, and support reconstruction for detached timing."""

from dataclasses import dataclass

import reference_input_model as model
import reference_input_recompute_graph as graph_oracle
import reference_input_recompute_transfer as transfer_oracle
from reference_input_canonical import CanonicalError


@dataclass(frozen=True)
class ModelFacts:
    graph: graph_oracle.GraphFacts
    expected_start_seeds: tuple[model.CandidateStartSeed, ...]
    expected_endpoint_required: tuple[model.CandidateEndpointRequired, ...]
    expected_capacitances: tuple[model.CandidatePinCapacitance, ...]
    expected_loads: tuple[tuple[tuple[float, float], tuple[float, float]] | None, ...]
    expected_unsupported_reasons: tuple[model.UnsupportedReason, ...]


def _fail(message: str) -> None:
    raise CanonicalError(f"reference-input model recomputation: {message}")


def _byte_key(value: str) -> bytes:
    return value.encode("utf-8", errors="strict")


def _clock_tag(
    candidate: model.CandidateInput,
    facts: graph_oracle.GraphFacts,
    node_id: int,
    port_constraints: dict[int, model.CandidatePortConstraint],
) -> str:
    direct = facts.direct_clock_by_node[node_id]
    if direct is not None:
        return candidate.clocks[direct].embedded_clock_name

    instance = facts.instance_by_node[node_id]
    if instance is not None and candidate.instances[instance].is_sequential:
        clock_node = facts.clock_node_by_instance[instance]
        if clock_node is not None and facts.clock_owner_by_node[clock_node] is not None:
            return facts.clock_owner_by_node[clock_node] or ""

    if facts.clock_owner_by_node[node_id] is not None:
        return facts.clock_owner_by_node[node_id] or ""
    constraint = port_constraints.get(node_id)
    if constraint is not None and constraint.clock_name:
        return constraint.clock_name
    return candidate.clocks[0].map_key if candidate.clocks else "clk"


def _none_matrix() -> tuple[tuple[None, None], tuple[None, None]]:
    return ((None, None), (None, None))


def _value_matrix(value: float) -> tuple[tuple[float, float], tuple[float, float]]:
    return ((value, value), (value, value))


def _start_seeds(
    candidate: model.CandidateInput,
    facts: graph_oracle.GraphFacts,
) -> tuple[model.CandidateStartSeed, ...]:
    constraints = transfer_oracle.index_port_constraints(candidate)
    result: list[model.CandidateStartSeed] = []
    for node_id in facts.expected_start_ids:
        node = candidate.nodes[node_id]
        instance = facts.instance_by_node[node_id]
        clock_tag = _clock_tag(candidate, facts, node_id, constraints)
        path_start = node_id
        availability = model.StartSeedAvailability.MATERIALIZED
        path_source: model.PathSource | None = None
        arrival = _none_matrix()
        slew = _none_matrix()
        crpr_node: int | None = None
        crpr_transition: model.Transition | None = None

        if instance is not None and candidate.instances[instance].is_sequential:
            availability = model.StartSeedAvailability.UNSUPPORTED_SEQUENTIAL
            clock_node = facts.clock_node_by_instance[instance]
            output_node = facts.output_node_by_instance[instance]
            reachable = clock_node is not None and facts.clock_reachable_nodes[clock_node]
            selected = node_id in (clock_node, output_node)
            if reachable and selected:
                if output_node is None:
                    _fail("reachable selected register seed has no output role")
                path_source = model.PathSource.REGISTER
                path_start = output_node if node_id == clock_node else node_id
                crpr_node = clock_node
                raw_instance = facts.raw_instance_by_name[
                    candidate.instances[instance].instance_name
                ]
                selected_arc = raw_instance.selected_clock_arc
                trigger = (
                    selected_arc.ordered_child_triggers[0]
                    if selected_arc is not None and selected_arc.ordered_child_triggers
                    else model.Transition.RISE
                )
                crpr_transition = (
                    model.Transition.RISE if trigger == model.Transition.NONE else trigger
                )
        else:
            direct = facts.direct_clock_by_node[node_id]
            constraint = constraints.get(node_id)
            has_input_delay = constraint is not None and any(
                value is not None for value in constraint.input_delay
            )
            input_port = node.is_port and node.direction in (
                model.Direction.INPUT,
                model.Direction.INOUT,
            )
            if input_port and (direct is not None or has_input_delay):
                path_source = model.PathSource.INPUT
                if direct is not None:
                    clock = candidate.clocks[direct]
                    arrival = (
                        (clock.edges[0], clock.edges[1]),
                        (clock.edges[0], clock.edges[0]),
                    )
                else:
                    maximum = 0.0 if constraint is None else constraint.input_delay[0] or 0.0
                    minimum = (
                        maximum
                        if constraint is None or constraint.input_delay[1] is None
                        else constraint.input_delay[1]
                    )
                    arrival = ((maximum, maximum), (minimum, minimum))
                input_slew = (
                    0.0
                    if constraint is None or constraint.input_transition is None
                    else constraint.input_transition
                )
                slew = _value_matrix(input_slew)
            else:
                input_slew = (
                    0.0
                    if constraint is None or constraint.input_transition is None
                    else constraint.input_transition
                )
                slew = _value_matrix(input_slew)

        result.append(
            model.CandidateStartSeed(
                node_id,
                path_start,
                availability,
                path_source,
                clock_tag,
                arrival,
                slew,
                crpr_node,
                crpr_transition,
            )
        )

    expected = tuple(result)
    if candidate.start_seeds != expected:
        _fail("candidate start seeds differ from independent clock/constraint replay")
    return expected


def _endpoint_required(
    candidate: model.CandidateInput,
    facts: graph_oracle.GraphFacts,
) -> tuple[model.CandidateEndpointRequired, ...]:
    constraints = transfer_oracle.index_port_constraints(candidate)
    result: list[model.CandidateEndpointRequired] = []
    for node_id in facts.expected_end_ids:
        node = candidate.nodes[node_id]
        if not node.is_port or node.direction not in (
            model.Direction.OUTPUT,
            model.Direction.INOUT,
        ):
            continue
        tag = _clock_tag(candidate, facts, node_id, constraints)
        clock = next((item for item in candidate.clocks if item.map_key == tag), None)
        if clock is None and candidate.clocks:
            clock = candidate.clocks[0]
        constraint = constraints.get(node_id)
        result.append(
            model.CandidateEndpointRequired(
                node_id,
                tag,
                0.0 if clock is None else clock.period,
                (None, None) if constraint is None else constraint.output_delay,
            )
        )

    expected = tuple(result)
    if candidate.endpoint_required != expected:
        _fail("candidate endpoint-required values differ from raw constraints")

    tags = {
        value
        for clock in candidate.clocks
        for value in (clock.map_key, clock.embedded_clock_name)
        if value
    }
    tags.update(item.clock_name for item in candidate.port_constraints if item.clock_name)
    tags.update(item.clock_tag for item in candidate.start_seeds if item.clock_tag)
    tags.update(item.clock_name for item in candidate.endpoint_required if item.clock_name)
    expected_tags = tuple(sorted(tags, key=_byte_key))
    if candidate.clock_tags != expected_tags:
        _fail("candidate clock_tags differs from the byte-sorted semantic union")
    return expected


def _unsupported(
    candidate: model.CandidateInput,
    facts: graph_oracle.GraphFacts,
) -> tuple[model.UnsupportedReason, ...]:
    divider = False
    library_cells = {cell.cell_name: cell for cell in candidate.referenced_library_cells}
    for arc in candidate.arcs:
        if arc.kind != model.ArcKind.NET:
            continue
        source_owner = facts.instance_by_node[arc.source]
        sink_owner = facts.instance_by_node[arc.sink]
        is_divider = False
        if source_owner is not None and sink_owner is not None:
            source_instance = candidate.instances[source_owner]
            sink_instance = candidate.instances[sink_owner]
            source_cell = library_cells.get(source_instance.cell_name)
            if source_cell is None:
                _fail("net arc source instance lacks referenced library closure")
            raw_sink = facts.raw_instance_by_name[sink_instance.instance_name]
            sink_clock = raw_sink.clock_pin_name
            is_divider = (
                source_cell.present
                and source_cell.is_sequential
                and not source_cell.is_clock_gating
                and not source_cell.is_macro
                and candidate.nodes[arc.sink].key.full_name == sink_clock
            )
        if arc.is_disabled != is_divider:
            _fail(f"net arc {arc.arc_id} disabled flag differs from divider predicate")
        divider |= is_divider

    has_crpr = False
    for seed in candidate.start_seeds:
        if (seed.crpr_node is None) != (seed.crpr_transition is None):
            _fail("start seed has a one-sided CRPR pair")
        has_crpr |= seed.crpr_node is not None

    conditional = any(
        child.signature.sdf_condition
        for cell in candidate.referenced_library_cells
        for arc in cell.cell_arcs
        for child in arc.transfer.children
    ) or any(
        child.signature.sdf_condition
        for cell in candidate.referenced_library_cells
        for check in cell.timing_checks
        for child in check.children
    )
    conditions = (
        bool(candidate.parasitics.nets),
        any(instance.is_sequential for instance in candidate.instances),
        any(seed.path_source == model.PathSource.REGISTER for seed in candidate.start_seeds),
        bool(candidate.timing_check_endpoints),
        has_crpr,
        any(clock.propagated for clock in candidate.clocks),
        any(facts.replayed_clock_arcs),
        divider or any(instance.is_clock_gating for instance in candidate.instances),
        conditional,
        bool(candidate.case_analysis_constraints),
    )
    expected = tuple(
        reason
        for reason, present in zip(model.UnsupportedReason, conditions, strict=True)
        if present
    )
    if candidate.unsupported_reasons != expected:
        _fail("candidate unsupported reasons differ from exact derived predicates")
    return expected


def recompute_model(
    candidate: model.CandidateInput,
    raw: model.RawReferenceInput,
    facts: graph_oracle.GraphFacts | None = None,
) -> ModelFacts:
    """Validate and reconstruct all non-fingerprint detached model semantics."""
    if facts is None:
        facts = graph_oracle.recompute_graph(candidate, raw)
    transfer = transfer_oracle.recompute_transfer(candidate, raw, facts)
    seeds = _start_seeds(candidate, facts)
    endpoints = _endpoint_required(candidate, facts)
    reasons = _unsupported(candidate, facts)
    return ModelFacts(
        facts,
        seeds,
        endpoints,
        transfer.capacitances,
        transfer.loads,
        reasons,
    )
