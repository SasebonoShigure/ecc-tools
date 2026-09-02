#!/usr/bin/env python3
"""Strict typed decoder and completeness checks for the 17 poison families."""

from dataclasses import dataclass
from typing import TypeAlias

import reference_input_model as model
from reference_input_canonical import CanonicalError, CanonicalReader

CANARY_DOMAIN_PREFIX = "ista-csr-reference-input-canary/"
CANARY_DOMAIN_SUFFIX = "/1.0.0"


@dataclass(frozen=True)
class Binding:
    repeat_index: int
    run_nonce: str
    session_token: int


@dataclass(frozen=True)
class PointKey:
    full_name: str


@dataclass(frozen=True)
class ArcKey:
    native_rank: int
    source: str
    sink: str
    kind: model.ArcKind
    owner: str
    library_source_port: str
    library_sink_port: str


@dataclass(frozen=True)
class AnalysisTransitionDouble:
    analysis: model.AnalysisMode
    transition: model.Transition
    value: float


@dataclass(frozen=True)
class AnalysisTransitionString:
    analysis: model.AnalysisMode
    transition: model.Transition
    value: str


@dataclass(frozen=True)
class AnalysisTransitionTransition:
    analysis: model.AnalysisMode
    transition: model.Transition
    value: model.Transition


@dataclass(frozen=True)
class TransitionTransition:
    transition: model.Transition
    value: model.Transition


@dataclass(frozen=True)
class AnalysisInputOutputDouble:
    analysis: model.AnalysisMode
    input_transition: model.Transition
    output_transition: model.Transition
    value: float


@dataclass(frozen=True)
class TimingArcDouble:
    timing_arc_index: int
    analysis: model.AnalysisMode
    input_transition: model.Transition
    output_transition: model.Transition
    value: float


@dataclass(frozen=True)
class PathState:
    analysis: model.AnalysisMode
    path_source: model.PathSource
    transition: model.Transition
    clock_tag: str
    arrival: float
    slew: float
    launch_time: float
    start_point: str
    predecessor: str
    clock_name: str
    crpr_clock_pin: str
    predecessor_arc_idx: int
    predecessor_arc_delay: float
    trans_type: model.Transition
    predecessor_trans_type: model.Transition
    crpr_clock_trans_type: model.Transition


@dataclass(frozen=True)
class PointScalars:
    arrival: float
    required: float
    slack: float
    launch_time: float
    predecessor: str
    clock_name: str
    predecessor_arc_idx: int


@dataclass(frozen=True)
class PairedMaps:
    effective: tuple[object, ...]
    physical: tuple[object, ...]


@dataclass(frozen=True)
class ArcScalars:
    delay: float
    delay_max: float
    delay_min: float


@dataclass(frozen=True)
class ArcTransitionMaps:
    delays: tuple[AnalysisTransitionDouble, ...]
    transitions: tuple[TransitionTransition, ...]


@dataclass(frozen=True)
class FallbackBehavior:
    ordinary_min_replaced: bool
    ordinary_max_replaced: bool
    fallback_replaced: bool


CanaryKey: TypeAlias = PointKey | ArcKey | str
CanaryPayload: TypeAlias = (
    int
    | bool
    | PointScalars
    | PairedMaps
    | tuple[AnalysisTransitionDouble, ...]
    | tuple[PathState, ...]
    | ArcScalars
    | ArcTransitionMaps
    | tuple[AnalysisInputOutputDouble, ...]
    | tuple[TimingArcDouble, ...]
    | FallbackBehavior
    | tuple[str, ...]
)


@dataclass(frozen=True)
class Subject:
    key: CanaryKey
    payload: CanaryPayload


@dataclass(frozen=True)
class CanaryEnvelope:
    family: model.CanaryFamily
    binding: Binding
    subjects: tuple[Subject, ...]


def _at_double(reader: CanonicalReader) -> AnalysisTransitionDouble:
    return AnalysisTransitionDouble(
        reader.enum(model.AnalysisMode, "map.analysis"),
        reader.enum(model.Transition, "map.transition"),
        reader.canary_double(),
    )


def _at_string(reader: CanonicalReader) -> AnalysisTransitionString:
    return AnalysisTransitionString(
        reader.enum(model.AnalysisMode, "map.analysis"),
        reader.enum(model.Transition, "map.transition"),
        reader.string(),
    )


def _at_transition(reader: CanonicalReader) -> AnalysisTransitionTransition:
    return AnalysisTransitionTransition(
        reader.enum(model.AnalysisMode, "map.analysis"),
        reader.enum(model.Transition, "map.transition"),
        reader.enum(model.Transition, "map.value"),
    )


def _transition_transition(reader: CanonicalReader) -> TransitionTransition:
    return TransitionTransition(
        reader.enum(model.Transition, "transition_map.key"),
        reader.enum(model.Transition, "transition_map.value"),
    )


def _aio_double(reader: CanonicalReader) -> AnalysisInputOutputDouble:
    return AnalysisInputOutputDouble(
        reader.enum(model.AnalysisMode, "delay_map.analysis"),
        reader.enum(model.Transition, "delay_map.input_transition"),
        reader.enum(model.Transition, "delay_map.output_transition"),
        reader.canary_double(),
    )


def _timing_arc_double(reader: CanonicalReader) -> TimingArcDouble:
    return TimingArcDouble(
        reader.i32(),
        reader.enum(model.AnalysisMode, "timing_arc_map.analysis"),
        reader.enum(model.Transition, "timing_arc_map.input_transition"),
        reader.enum(model.Transition, "timing_arc_map.output_transition"),
        reader.canary_double(),
    )


def _path_state(reader: CanonicalReader) -> PathState:
    return PathState(
        reader.enum(model.AnalysisMode, "path.analysis"),
        reader.enum(model.PathSource, "path.path_source"),
        reader.enum(model.Transition, "path.transition"),
        reader.string(),
        reader.canary_double(),
        reader.canary_double(),
        reader.canary_double(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.string(),
        reader.u64(),
        reader.canary_double(),
        reader.enum(model.Transition, "path.trans_type"),
        reader.enum(model.Transition, "path.predecessor_trans_type"),
        reader.enum(model.Transition, "path.crpr_clock_trans_type"),
    )


def _point_key(reader: CanonicalReader) -> PointKey:
    return PointKey(reader.string())


def _arc_key(reader: CanonicalReader) -> ArcKey:
    return ArcKey(
        reader.u64(),
        reader.string(),
        reader.string(),
        reader.enum(model.ArcKind, "arc_key.kind"),
        reader.string(),
        reader.string(),
        reader.string(),
    )


def _point_scalars(reader: CanonicalReader) -> PointScalars:
    return PointScalars(
        reader.canary_double(),
        reader.canary_double(),
        reader.canary_double(),
        reader.canary_double(),
        reader.string(),
        reader.string(),
        reader.u64(),
    )


def _paired(reader: CanonicalReader, item_reader: object, field: str) -> PairedMaps:
    if not callable(item_reader):
        raise CanonicalError("internal canary item reader is not callable")
    return PairedMaps(
        reader.vector(lambda _index: item_reader(reader), f"{field}.effective"),
        reader.vector(lambda _index: item_reader(reader), f"{field}.physical"),
    )


def _payload(reader: CanonicalReader, family: model.CanaryFamily) -> CanaryPayload:
    if family is model.CanaryFamily.TIMING_POINT_LEVEL:
        return reader.i32()
    if family is model.CanaryFamily.TIMING_POINT_IS_CLOCK_POINT:
        return reader.boolean()
    if family is model.CanaryFamily.TIMING_POINT_SCALARS:
        return _point_scalars(reader)
    if family in {
        model.CanaryFamily.TIMING_POINT_CLOCK_ARRIVAL_MAPS,
        model.CanaryFamily.TIMING_POINT_CLOCK_SLEW_MAPS,
        model.CanaryFamily.TIMING_POINT_CLOCK_PREDECESSOR_ARC_DELAY_MAPS,
    }:
        return _paired(reader, _at_double, "point.double_maps")
    if family is model.CanaryFamily.TIMING_POINT_CLOCK_PREDECESSOR_MAPS:
        return _paired(reader, _at_string, "point.string_maps")
    if family is model.CanaryFamily.TIMING_POINT_CLOCK_PREDECESSOR_TRANSITION_MAPS:
        return _paired(reader, _at_transition, "point.transition_maps")
    if family is model.CanaryFamily.TIMING_POINT_DATA_SLEW_MAP:
        return reader.vector(lambda _index: _at_double(reader), "point.data_slew_map")
    if family is model.CanaryFamily.TIMING_POINT_PATH_STATE_MAP:
        return reader.vector(lambda _index: _path_state(reader), "point.path_state_map")
    if family is model.CanaryFamily.ARC_SCALARS:
        return ArcScalars(reader.canary_double(), reader.canary_double(), reader.canary_double())
    if family is model.CanaryFamily.ARC_TRANSITION_MAPS:
        return ArcTransitionMaps(
            reader.vector(lambda _index: _at_double(reader), "arc.transition_delays"),
            reader.vector(lambda _index: _transition_transition(reader), "arc.transition_types"),
        )
    if family in {
        model.CanaryFamily.ARC_INPUT_OUTPUT_DELAY_MAP,
        model.CanaryFamily.ARC_GRAPH_DELAY_MAP,
    }:
        return reader.vector(lambda _index: _aio_double(reader), "arc.delay_map")
    if family is model.CanaryFamily.ARC_TIMING_ARC_DELAY_MAP:
        return reader.vector(lambda _index: _timing_arc_double(reader), "arc.timing_arc_delay_map")
    if family is model.CanaryFamily.ARC_TIMING_ARC_FALLBACK_PROVENANCE:
        return FallbackBehavior(reader.boolean(), reader.boolean(), reader.boolean())
    if family is model.CanaryFamily.DATABASE_TIMING_ORDER:
        return reader.vector(lambda _index: reader.string(), "database.timing_order")
    raise CanonicalError(f"unsupported canary family {family!r}")


def _subject(reader: CanonicalReader, family: model.CanaryFamily) -> Subject:
    if family.value <= model.CanaryFamily.TIMING_POINT_PATH_STATE_MAP.value:
        key: CanaryKey = _point_key(reader)
    elif family.value <= model.CanaryFamily.ARC_TIMING_ARC_FALLBACK_PROVENANCE.value:
        key = _arc_key(reader)
    else:
        key = reader.string()
        if key != "database":
            raise CanonicalError("database timing-order subject key must be 'database'")
    return Subject(key, _payload(reader, family))


def decode_canary_wire(payload: bytes, outer_family: str) -> CanaryEnvelope:
    if outer_family not in model.CANARY_FAMILY_TOKENS:
        raise CanonicalError(f"unknown outer canary family {outer_family!r}")
    family = model.CanaryFamily(model.CANARY_FAMILY_TOKENS.index(outer_family))
    reader = CanonicalReader(payload, f"canary {outer_family}")
    expected_domain = f"{CANARY_DOMAIN_PREFIX}{outer_family}{CANARY_DOMAIN_SUFFIX}"
    if reader.string() != expected_domain:
        raise CanonicalError("canary domain/family/version mismatch")
    binding = Binding(reader.u64(), reader.string(), reader.u64())
    subjects = reader.vector(lambda _index: _subject(reader, family), "subjects")
    reader.finish()
    if not subjects:
        raise CanonicalError("canary family has no subjects")
    if len({subject.key for subject in subjects}) != len(subjects):
        raise CanonicalError("canary subjects are duplicated")
    return CanaryEnvelope(family, binding, subjects)


def _entry_keys(entries: tuple[object, ...]) -> tuple[object, ...]:
    keys = []
    for entry in entries:
        if isinstance(
            entry,
            (AnalysisTransitionDouble, AnalysisTransitionString, AnalysisTransitionTransition),
        ):
            keys.append((entry.analysis, entry.transition))
        elif isinstance(entry, TransitionTransition):
            keys.append(entry.transition)
        elif isinstance(entry, AnalysisInputOutputDouble):
            keys.append((entry.analysis, entry.input_transition, entry.output_transition))
        elif isinstance(entry, TimingArcDouble):
            keys.append(
                (
                    entry.timing_arc_index,
                    entry.analysis,
                    entry.input_transition,
                    entry.output_transition,
                )
            )
        elif isinstance(entry, PathState):
            keys.append(
                (entry.analysis, entry.path_source, entry.transition, entry.clock_tag.encode())
            )
        else:
            raise CanonicalError(f"unknown canary map entry {entry!r}")
    result = tuple(keys)
    if result != tuple(sorted(result)) or len(set(result)) != len(result):
        raise CanonicalError("canary map keys are not strict native order")
    return result


def _require_all_values_changed(before: tuple[object, ...], after: tuple[object, ...]) -> None:
    if not before or _entry_keys(before) != _entry_keys(after):
        raise CanonicalError("canary map key/order/cardinality drifted or is empty")
    for old, new in zip(before, after, strict=True):
        old_values = old.__dict__.copy()
        new_values = new.__dict__.copy()
        if isinstance(old, PathState):
            key_names = {"analysis", "path_source", "transition", "clock_tag"}
        elif isinstance(old, TimingArcDouble):
            key_names = {
                "timing_arc_index",
                "analysis",
                "input_transition",
                "output_transition",
            }
        elif isinstance(old, AnalysisInputOutputDouble):
            key_names = {"analysis", "input_transition", "output_transition"}
        elif isinstance(old, TransitionTransition):
            key_names = {"transition"}
        else:
            key_names = {"analysis", "transition"}
        for name in key_names:
            old_values.pop(name)
            new_values.pop(name)
        if set(old_values) != set(new_values) or any(
            old_values[name] == new_values[name] for name in old_values
        ):
            raise CanonicalError("canary required value leaf did not change")


def _validate_scaffolded_map(
    pristine: tuple[object, ...],
    before: tuple[object, ...],
    default: object,
) -> None:
    if pristine:
        _entry_keys(pristine)
        if before != pristine:
            raise CanonicalError("nonempty pristine canary map changed during precondition")
    elif before != (default,):
        raise CanonicalError("empty pristine canary map has wrong singleton precondition")
    _entry_keys(before)


def _validate_paired_scaffold(
    pristine: object,
    before: object,
    effective_default: object,
    physical_default: object,
) -> None:
    if not isinstance(pristine, PairedMaps) or not isinstance(before, PairedMaps):
        raise CanonicalError("paired-map precondition payload type differs")
    _validate_scaffolded_map(pristine.effective, before.effective, effective_default)
    _validate_scaffolded_map(pristine.physical, before.physical, physical_default)


def _validate_subject_scaffold(
    family: model.CanaryFamily,
    pristine: CanaryPayload,
    before: CanaryPayload,
) -> None:
    max_rise = (model.AnalysisMode.MAX, model.Transition.RISE)
    if family is model.CanaryFamily.TIMING_POINT_CLOCK_ARRIVAL_MAPS:
        _validate_paired_scaffold(
            pristine,
            before,
            AnalysisTransitionDouble(*max_rise, 1.0),
            AnalysisTransitionDouble(*max_rise, 2.0),
        )
        return
    if family is model.CanaryFamily.TIMING_POINT_CLOCK_SLEW_MAPS:
        _validate_paired_scaffold(
            pristine,
            before,
            AnalysisTransitionDouble(*max_rise, 3.0),
            AnalysisTransitionDouble(*max_rise, 4.0),
        )
        return
    if family is model.CanaryFamily.TIMING_POINT_CLOCK_PREDECESSOR_MAPS:
        _validate_paired_scaffold(
            pristine,
            before,
            AnalysisTransitionString(*max_rise, "__csr_predecessor__"),
            AnalysisTransitionString(*max_rise, "__csr_physical_predecessor__"),
        )
        return
    if family is model.CanaryFamily.TIMING_POINT_CLOCK_PREDECESSOR_ARC_DELAY_MAPS:
        _validate_paired_scaffold(
            pristine,
            before,
            AnalysisTransitionDouble(*max_rise, 5.0),
            AnalysisTransitionDouble(*max_rise, 6.0),
        )
        return
    if family is model.CanaryFamily.TIMING_POINT_CLOCK_PREDECESSOR_TRANSITION_MAPS:
        default = AnalysisTransitionTransition(*max_rise, model.Transition.RISE)
        _validate_paired_scaffold(pristine, before, default, default)
        return
    if family is model.CanaryFamily.TIMING_POINT_DATA_SLEW_MAP:
        if not isinstance(pristine, tuple) or not isinstance(before, tuple):
            raise CanonicalError("data-slew precondition payload type differs")
        _validate_scaffolded_map(pristine, before, AnalysisTransitionDouble(*max_rise, 7.0))
        return
    if family is model.CanaryFamily.TIMING_POINT_PATH_STATE_MAP:
        if not isinstance(pristine, tuple) or not isinstance(before, tuple):
            raise CanonicalError("path-state precondition payload type differs")
        default_path = PathState(
            model.AnalysisMode.MAX,
            model.PathSource.INPUT,
            model.Transition.RISE,
            "__csr_poison_clock__",
            float("-inf"),
            0.0,
            0.0,
            "",
            "",
            "",
            "",
            (1 << 64) - 1,
            0.0,
            model.Transition.NONE,
            model.Transition.NONE,
            model.Transition.NONE,
        )
        _validate_scaffolded_map(pristine, before, default_path)
        return
    if family is model.CanaryFamily.ARC_TRANSITION_MAPS:
        if not isinstance(pristine, ArcTransitionMaps) or not isinstance(before, ArcTransitionMaps):
            raise CanonicalError("arc-transition precondition payload type differs")
        _validate_scaffolded_map(
            pristine.delays,
            before.delays,
            AnalysisTransitionDouble(*max_rise, 1.0),
        )
        _validate_scaffolded_map(
            pristine.transitions,
            before.transitions,
            TransitionTransition(model.Transition.RISE, model.Transition.RISE),
        )
        return
    if family in {
        model.CanaryFamily.ARC_INPUT_OUTPUT_DELAY_MAP,
        model.CanaryFamily.ARC_GRAPH_DELAY_MAP,
    }:
        if not isinstance(pristine, tuple) or not isinstance(before, tuple):
            raise CanonicalError("arc-delay precondition payload type differs")
        default_value = 2.0 if family is model.CanaryFamily.ARC_INPUT_OUTPUT_DELAY_MAP else 3.0
        _validate_scaffolded_map(
            pristine,
            before,
            AnalysisInputOutputDouble(
                model.AnalysisMode.MAX,
                model.Transition.RISE,
                model.Transition.RISE,
                default_value,
            ),
        )
        return
    if family is model.CanaryFamily.ARC_TIMING_ARC_DELAY_MAP:
        if not isinstance(pristine, tuple) or not isinstance(before, tuple):
            raise CanonicalError("timing-arc precondition payload type differs")
        if pristine:
            _entry_keys(pristine)
        expected = list(pristine)
        occupied = {entry.timing_arc_index for entry in pristine}
        if not pristine:
            expected.append(TimingArcDouble(0, *max_rise, model.Transition.RISE, 4.0))
            occupied.add(0)
        fallback_key = -(1 << 31)
        while fallback_key in occupied:
            fallback_key += 1
        expected.append(TimingArcDouble(fallback_key, *max_rise, model.Transition.RISE, 40.0))
        expected.sort(
            key=lambda entry: (
                entry.timing_arc_index,
                entry.analysis,
                entry.input_transition,
                entry.output_transition,
            )
        )
        if before != tuple(expected):
            raise CanonicalError("timing-arc fallback precondition differs")
        _entry_keys(before)
        return
    if family is model.CanaryFamily.ARC_TIMING_ARC_FALLBACK_PROVENANCE:
        expected_fallback = FallbackBehavior(
            ordinary_min_replaced=False,
            ordinary_max_replaced=False,
            fallback_replaced=True,
        )
        if pristine != expected_fallback or before != expected_fallback:
            raise CanonicalError("fallback-provenance pristine/precondition differs")
        return
    if before != pristine:
        raise CanonicalError("non-map canary changed during precondition")


def validate_canary_scaffold(
    pristine: CanaryEnvelope,
    before: CanaryEnvelope,
) -> None:
    if pristine.family is not before.family or pristine.binding != before.binding:
        raise CanonicalError("canary pristine/precondition family or binding drifted")
    if tuple(item.key for item in pristine.subjects) != tuple(item.key for item in before.subjects):
        raise CanonicalError("canary pristine/precondition subject inventory drifted")
    for pristine_subject, before_subject in zip(pristine.subjects, before.subjects, strict=True):
        _validate_subject_scaffold(
            pristine.family,
            pristine_subject.payload,
            before_subject.payload,
        )


def validate_canary_pair(before: CanaryEnvelope, after: CanaryEnvelope) -> str:
    if before.family is not after.family or before.binding != after.binding:
        raise CanonicalError("canary family/binding drifted")
    if tuple(item.key for item in before.subjects) != tuple(item.key for item in after.subjects):
        raise CanonicalError("canary subject inventory drifted")
    family = before.family
    for old_subject, new_subject in zip(before.subjects, after.subjects, strict=True):
        old = old_subject.payload
        new = new_subject.payload
        if family in {
            model.CanaryFamily.TIMING_POINT_LEVEL,
            model.CanaryFamily.TIMING_POINT_IS_CLOCK_POINT,
        }:
            if old == new:
                raise CanonicalError("canary scalar did not change")
        elif isinstance(old, PointScalars) and isinstance(new, PointScalars):
            if any(
                a == b for a, b in zip(old.__dict__.values(), new.__dict__.values(), strict=True)
            ):
                raise CanonicalError("timing-point scalar family is incomplete")
        elif isinstance(old, PairedMaps) and isinstance(new, PairedMaps):
            _require_all_values_changed(old.effective, new.effective)
            _require_all_values_changed(old.physical, new.physical)
        elif isinstance(old, ArcScalars) and isinstance(new, ArcScalars):
            if any(
                a == b for a, b in zip(old.__dict__.values(), new.__dict__.values(), strict=True)
            ):
                raise CanonicalError("arc scalar family is incomplete")
        elif isinstance(old, ArcTransitionMaps) and isinstance(new, ArcTransitionMaps):
            _require_all_values_changed(old.delays, new.delays)
            _require_all_values_changed(old.transitions, new.transitions)
        elif isinstance(old, FallbackBehavior) and isinstance(new, FallbackBehavior):
            if (
                old.ordinary_min_replaced != new.ordinary_min_replaced
                or old.ordinary_max_replaced != new.ordinary_max_replaced
                or old.fallback_replaced == new.fallback_replaced
            ):
                raise CanonicalError("fallback-provenance behavior delta is invalid")
        elif family is model.CanaryFamily.DATABASE_TIMING_ORDER:
            if not isinstance(old, tuple) or not isinstance(new, tuple):
                raise CanonicalError("database timing order has wrong payload type")
            if len(old) != len(new) or not old or old == new:
                raise CanonicalError("database timing order did not change in place")
        elif isinstance(old, tuple) and isinstance(new, tuple):
            _require_all_values_changed(old, new)
        else:
            raise CanonicalError("canary before/after payload types differ")
    return model.CANARY_FAMILY_TOKENS[family.value]
