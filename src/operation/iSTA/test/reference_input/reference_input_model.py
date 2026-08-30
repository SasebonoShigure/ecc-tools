#!/usr/bin/env python3
"""Typed DTOs and frozen enum tokens for independent Step 4 test wires."""

from dataclasses import dataclass
from enum import IntEnum


class NodeKind(IntEnum):
    PORT = 0
    INSTANCE_PIN = 1


class Direction(IntEnum):
    NONE = 0
    INPUT = 1
    OUTPUT = 2
    INOUT = 3


class ArcKind(IntEnum):
    CELL = 0
    NET = 1


class AnalysisMode(IntEnum):
    MAX = 0
    MIN = 1


class Transition(IntEnum):
    RISE = 0
    FALL = 1
    NONE = 2


class PathSource(IntEnum):
    INPUT = 0
    REGISTER = 1


class StartSeedAvailability(IntEnum):
    MATERIALIZED = 0
    UNSUPPORTED_SEQUENTIAL = 1


class UnsupportedReason(IntEnum):
    PARASITIC_NET = 0
    SEQUENTIAL_INSTANCE = 1
    SEQUENTIAL_LAUNCH = 2
    TIMING_CHECK = 3
    CRPR = 4
    PROPAGATED_CLOCK = 5
    CLOCK_TREE_FANOUT = 6
    CLOCK_GATING_OR_DIVIDER = 7
    CONDITIONAL_TIMING_ARC = 8
    CASE_ANALYSIS = 9


class TimingSense(IntEnum):
    NONE_POSITIVE_FALLBACK = 0
    POSITIVE_UNATE = 1
    NEGATIVE_UNATE = 2
    NON_UNATE = 3


class TableVariable(IntEnum):
    NONE = 0
    INPUT_TRANSITION = 1
    OUTPUT_CAPACITANCE = 2
    CONSTRAINED_TRANSITION = 3


class TimingCheckKind(IntEnum):
    NONE = 0
    SETUP = 1
    HOLD = 2
    RECOVERY = 3
    REMOVAL = 4
    WIDTH = 5
    PERIOD = 6


class CanaryFamily(IntEnum):
    TIMING_POINT_LEVEL = 0
    TIMING_POINT_IS_CLOCK_POINT = 1
    TIMING_POINT_SCALARS = 2
    TIMING_POINT_CLOCK_ARRIVAL_MAPS = 3
    TIMING_POINT_CLOCK_SLEW_MAPS = 4
    TIMING_POINT_CLOCK_PREDECESSOR_MAPS = 5
    TIMING_POINT_CLOCK_PREDECESSOR_ARC_DELAY_MAPS = 6
    TIMING_POINT_CLOCK_PREDECESSOR_TRANSITION_MAPS = 7
    TIMING_POINT_DATA_SLEW_MAP = 8
    TIMING_POINT_PATH_STATE_MAP = 9
    ARC_SCALARS = 10
    ARC_TRANSITION_MAPS = 11
    ARC_INPUT_OUTPUT_DELAY_MAP = 12
    ARC_GRAPH_DELAY_MAP = 13
    ARC_TIMING_ARC_DELAY_MAP = 14
    ARC_TIMING_ARC_FALLBACK_PROVENANCE = 15
    DATABASE_TIMING_ORDER = 16


CANARY_FAMILY_TOKENS = (
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


@dataclass(frozen=True)
class AnalysisContext:
    liberty_corner: str
    temperature: str
    rc_corner: str


@dataclass(frozen=True)
class SnapshotIdentity:
    fingerprint_schema_version: str
    session_token: int
    graph_generation: int
    topology_fingerprint: str
    clock_constraint_fingerprint: str
    library_fingerprint: str
    corner_fingerprint: str
    parasitic_fingerprint: str
    timing_input_fingerprint: str


@dataclass(frozen=True)
class RawNode:
    map_key: str
    full_name: str
    is_port: bool
    direction: Direction
    instance_name: str


@dataclass(frozen=True)
class RawSelectedClockArc:
    source: str
    sink: str
    ordered_child_triggers: tuple[Transition, ...]


@dataclass(frozen=True)
class RawInstance:
    map_key: str
    cell_name: str
    sequential: bool
    gating: bool
    macro: bool
    clock_pin_name: str
    data_pin_name: str
    output_pin_name: str
    ordered_pin_names: tuple[str, ...]
    selected_clock_arc: RawSelectedClockArc | None


@dataclass(frozen=True)
class RawArc:
    native_rank: int
    source: str
    sink: str
    kind: ArcKind
    owner: str
    library_source_port: str
    library_sink_port: str
    is_clock_arc: bool
    disabled: bool
    loop_disabled: bool


@dataclass(frozen=True)
class RawNet:
    map_key: str
    embedded_name: str
    ordered_drivers: tuple[str, ...]
    ordered_loads: tuple[str, ...]


@dataclass(frozen=True)
class RawClockConstraint:
    map_key: str
    embedded_name: str
    ordered_sources: tuple[str, ...]
    period: float
    rise_edge: float
    fall_edge: float
    propagated: bool


@dataclass(frozen=True)
class RawPortConstraint:
    map_key: str
    embedded_port_name: str
    clock_name: str
    input_delay_max: float | None
    input_delay_min: float | None
    input_transition: float | None
    output_delay_max: float | None
    output_delay_min: float | None
    output_load: float | None


@dataclass(frozen=True)
class RawCaseConstraint:
    map_key: str
    raw_name: str
    value: bool


@dataclass(frozen=True)
class RawTimingCheck:
    instance_name: str
    cell_name: str
    ordinal: int
    kind: TimingCheckKind
    raw_clock_name: str
    full_clock_name: str
    raw_data_name: str
    full_data_name: str


@dataclass(frozen=True)
class RawTimingTable:
    present: bool
    variable1: TableVariable | None
    variable2: TableVariable | None
    axis1: tuple[float, ...]
    axis2: tuple[float, ...]
    values: tuple[float, ...]


@dataclass(frozen=True)
class RawLibraryPort:
    name: str
    direction: Direction
    clock: bool
    output: bool
    default_capacitance: float
    transition_capacitance: tuple[tuple[float | None, ...], ...]


@dataclass(frozen=True)
class RawLibraryArcChild:
    library_name: str
    index: int
    sense: TimingSense
    trigger_transition: Transition
    check_transition: Transition
    sdf_condition: str
    time_scale: float
    capacitance_scale: float
    slew_derate: float
    slew_lower_thresholds: tuple[float, float]
    slew_upper_thresholds: tuple[float, float]
    input_thresholds: tuple[float, float]
    output_thresholds: tuple[float, float]
    delay_tables: tuple[RawTimingTable, RawTimingTable]
    slew_tables: tuple[RawTimingTable, RawTimingTable]

    check_tables: tuple[RawTimingTable, RawTimingTable]


@dataclass(frozen=True)
class RawLibraryCellArc:
    source_port: str
    sink_port: str
    constant_delay: float
    delay_min: float
    delay_max: float
    is_clock_arc: bool
    disabled: bool
    is_clear_or_preset: bool
    ordered_children: tuple[RawLibraryArcChild, ...]


@dataclass(frozen=True)
class RawLibraryCheckArc:
    raw_clock_port: str
    raw_data_port: str
    kind: TimingCheckKind
    check_time: float
    clock_transition: Transition
    ordered_children: tuple[RawLibraryArcChild, ...]


@dataclass(frozen=True)
class RawLibraryCellPayload:
    sequential: bool
    gating: bool
    macro: bool
    clock_pin_name: str
    data_pin_name: str
    output_pin_name: str
    capacitance_unit: str
    time_unit: str
    slew_derate: float
    slew_lower_thresholds: tuple[float, float]
    slew_upper_thresholds: tuple[float, float]
    input_thresholds: tuple[float, float]
    output_thresholds: tuple[float, float]
    ports: tuple[RawLibraryPort, ...]
    cell_arcs: tuple[RawLibraryCellArc, ...]
    check_arcs: tuple[RawLibraryCheckArc, ...]


@dataclass(frozen=True)
class RawReferencedLibraryCell:
    library_name: str
    cell_name: str
    payload: RawLibraryCellPayload | None


@dataclass(frozen=True)
class RawParasiticNode:
    map_key: str
    embedded_name: str
    capacitance: float
    x: float
    y: float


@dataclass(frozen=True)
class RawParasiticResistor:
    source: str
    sink: str
    resistance: float


@dataclass(frozen=True)
class RawParasiticNet:
    map_key: str
    embedded_name: str
    total_capacitance: float
    nodes: tuple[RawParasiticNode, ...]
    resistors: tuple[RawParasiticResistor, ...]


@dataclass(frozen=True)
class RawParasitics:
    source_path: str
    capacitance_unit: str
    resistance_unit: str
    nets: tuple[RawParasiticNet, ...]


@dataclass(frozen=True)
class RawReferenceInput:
    nodes: tuple[RawNode, ...]
    instances: tuple[RawInstance, ...]
    arcs: tuple[RawArc, ...]
    nets: tuple[RawNet, ...]
    clock_constraints: tuple[RawClockConstraint, ...]
    port_constraints: tuple[RawPortConstraint, ...]
    case_constraints: tuple[RawCaseConstraint, ...]
    timing_checks: tuple[RawTimingCheck, ...]
    referenced_library_cells: tuple[RawReferencedLibraryCell, ...]
    parasitics: RawParasitics | None


@dataclass(frozen=True)
class NodeKey:
    kind: NodeKind
    full_name: str
    instance_name: str
    pin_name: str


@dataclass(frozen=True)
class TimingTableSignature:
    present: bool
    variable1: TableVariable
    variable2: TableVariable


@dataclass(frozen=True)
class LibraryArcSignature:
    library_name: str
    arc_index: int
    sense: TimingSense
    trigger_transition: Transition
    check_transition: Transition
    sdf_condition: str
    delay_tables: tuple[TimingTableSignature, TimingTableSignature]
    slew_tables: tuple[TimingTableSignature, TimingTableSignature]


@dataclass(frozen=True)
class ArcKey:
    source: NodeKey
    sink: NodeKey
    kind: ArcKind
    owner_name: str
    library_source_port: str
    library_sink_port: str
    library_arcs: tuple[LibraryArcSignature, ...]
    parallel_ordinal: int


@dataclass(frozen=True)
class CandidateTimingTable:
    present: bool
    variable1: TableVariable
    variable2: TableVariable
    axes: tuple[tuple[float, ...], ...]
    values: tuple[float, ...]


@dataclass(frozen=True)
class ChildTimingArc:
    signature: LibraryArcSignature
    time_unit_scale: float
    capacitance_unit_scale: float
    slew_derate: float
    slew_lower_threshold_pct: tuple[float, float]
    slew_upper_threshold_pct: tuple[float, float]
    input_threshold_pct: tuple[float, float]
    output_threshold_pct: tuple[float, float]
    delay_tables: tuple[CandidateTimingTable, CandidateTimingTable]
    slew_tables: tuple[CandidateTimingTable, CandidateTimingTable]
    check_tables: tuple[CandidateTimingTable, CandidateTimingTable]


@dataclass(frozen=True)
class CellTransfer:
    delay: float
    delay_min: float
    delay_max: float
    is_clock_arc: bool
    is_disabled: bool
    is_clear_or_preset: bool
    children: tuple[ChildTimingArc, ...]


@dataclass(frozen=True)
class ArcEligibility:
    levelization: bool
    data_arrival: bool
    data_slew: bool
    clock: bool


@dataclass(frozen=True)
class CandidateNode:
    node_id: int
    key: NodeKey
    direction: Direction
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
class NodeLookup:
    key: NodeKey
    node_id: int


@dataclass(frozen=True)
class CandidateInstance:
    instance_name: str
    cell_name: str
    ordered_pins: tuple[int, ...]
    is_sequential: bool
    has_timing_checks: bool
    is_clock_gating: bool
    is_macro: bool


@dataclass(frozen=True)
class CandidateArc:
    arc_id: int
    key: ArcKey
    source: int
    sink: int
    kind: ArcKind
    native_traversal_rank: int
    is_clock_arc: bool
    is_disabled: bool
    is_loop_disabled: bool
    eligibility: ArcEligibility
    transfer: CellTransfer
    output_load: tuple[tuple[float, float], tuple[float, float]] | None


@dataclass(frozen=True)
class ArcLookup:
    key: ArcKey
    arc_id: int


@dataclass(frozen=True)
class CandidateRawNodeReference:
    full_name: str
    resolved_node: int | None


@dataclass(frozen=True)
class CandidateClockConstraint:
    map_key: str
    embedded_clock_name: str
    ordered_sources: tuple[CandidateRawNodeReference, ...]
    period: float
    edges: tuple[float, float]
    propagated: bool


@dataclass(frozen=True)
class CandidatePortConstraint:
    map_key: str
    embedded_port_name: str
    resolved_node: int | None
    input_delay: tuple[float | None, float | None]
    input_transition: float | None
    output_delay: tuple[float | None, float | None]
    output_load: float | None
    clock_name: str


@dataclass(frozen=True)
class CandidateCaseConstraint:
    raw_name: str
    resolved_node: int | None
    value: bool


@dataclass(frozen=True)
class CandidateTimingCheckEndpoint:
    instance_name: str
    referenced_check_ordinal: int
    check_kind: TimingCheckKind
    clock_pin_raw_name: str
    data_pin_raw_name: str
    clock_pin_full_name: str
    data_pin_full_name: str
    resolved_clock_node: int | None
    resolved_data_node: int | None


@dataclass(frozen=True)
class CandidateStartSeed:
    seed_node: int
    path_start_node: int
    availability: StartSeedAvailability
    path_source: PathSource | None
    clock_tag: str
    arrival: tuple[tuple[float | None, float | None], tuple[float | None, float | None]]
    slew: tuple[tuple[float | None, float | None], tuple[float | None, float | None]]
    crpr_node: int | None
    crpr_transition: Transition | None


@dataclass(frozen=True)
class CandidateEndpointRequired:
    node: int
    clock_name: str
    clock_period: float
    output_delay: tuple[float | None, float | None]


@dataclass(frozen=True)
class CandidatePinCapacitance:
    node: int
    default_capacitance: float
    transition_capacitance: tuple[
        tuple[float | None, float | None], tuple[float | None, float | None]
    ]


@dataclass(frozen=True)
class CandidateLibraryPort:
    name: str
    direction: Direction
    is_clock: bool
    is_output: bool
    default_capacitance: float
    transition_capacitance: tuple[
        tuple[float | None, float | None], tuple[float | None, float | None]
    ]


@dataclass(frozen=True)
class CandidateLibraryCellArc:
    source_port: str
    sink_port: str
    transfer: CellTransfer


@dataclass(frozen=True)
class CandidateLibraryTimingCheck:
    clock_port: str
    data_port: str
    kind: TimingCheckKind
    check_time: float
    clock_transition: Transition
    children: tuple[ChildTimingArc, ...]


@dataclass(frozen=True)
class CandidateReferencedLibraryCell:
    cell_name: str
    present: bool
    library_name: str
    is_sequential: bool
    is_clock_gating: bool
    is_macro: bool
    clock_pin_name: str
    data_pin_name: str
    output_pin_name: str
    ports: tuple[CandidateLibraryPort, ...]
    cell_arcs: tuple[CandidateLibraryCellArc, ...]
    timing_checks: tuple[CandidateLibraryTimingCheck, ...]


@dataclass(frozen=True)
class CandidateParasiticNet:
    map_key: str
    embedded_name: str
    lumped_capacitance: float
    nodes: tuple[RawParasiticNode, ...]
    resistors: tuple[RawParasiticResistor, ...]


@dataclass(frozen=True)
class CandidateParasitics:
    capacitance_unit: str
    resistance_unit: str
    nets: tuple[CandidateParasiticNet, ...]


@dataclass(frozen=True)
class CandidateInput:
    schema_version: str
    source_identity: SnapshotIdentity
    analysis_context: AnalysisContext
    analysis_order: tuple[AnalysisMode, AnalysisMode]
    path_source_order: tuple[PathSource, PathSource]
    transition_order: tuple[Transition, Transition]
    nodes: tuple[CandidateNode, ...]
    node_ids_by_key: tuple[NodeLookup, ...]
    instances: tuple[CandidateInstance, ...]
    arcs: tuple[CandidateArc, ...]
    arc_ids_by_key: tuple[ArcLookup, ...]
    out_row_offsets: tuple[int, ...]
    out_arc_ids: tuple[int, ...]
    in_row_offsets: tuple[int, ...]
    in_arc_ids: tuple[int, ...]
    ordered_root_ids: tuple[int, ...]
    ordered_start_ids: tuple[int, ...]
    ordered_end_ids: tuple[int, ...]
    clock_tags: tuple[str, ...]
    clocks: tuple[CandidateClockConstraint, ...]
    clock_owner_by_node: tuple[str | None, ...]
    port_constraints: tuple[CandidatePortConstraint, ...]
    case_analysis_constraints: tuple[CandidateCaseConstraint, ...]
    timing_check_endpoints: tuple[CandidateTimingCheckEndpoint, ...]
    referenced_library_cells: tuple[CandidateReferencedLibraryCell, ...]
    parasitics: CandidateParasitics
    start_seeds: tuple[CandidateStartSeed, ...]
    endpoint_required: tuple[CandidateEndpointRequired, ...]
    cell_pin_capacitances: tuple[CandidatePinCapacitance, ...]
    unsupported_reasons: tuple[UnsupportedReason, ...]
