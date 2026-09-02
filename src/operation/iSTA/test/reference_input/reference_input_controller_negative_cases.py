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
"""Orthogonal V7 negative matrix for the Step 4 fresh-process contract."""
# ruff: noqa: FBT003

import copy
from collections.abc import Callable
from dataclasses import replace
from pathlib import Path
from typing import Any

import reference_input_contract as contract
import reference_input_controller as _controller  # noqa: F401
import reference_input_wire_fixture as fixture

wire = contract.canary_wire
model = wire.model
Record = dict[str, Any]
Mutation = Callable[[Record], None]


def _valid_record(
    descriptor: contract.CaseDescriptor,
    repeat_index: int,
    nonce: str,
) -> Record:
    candidate, raw = fixture.fixture_case(descriptor.name)
    identity = contract.fingerprint_oracle.recompute_identity(candidate, contract.SESSION_TOKEN, 0)
    candidate_bytes = fixture.encode_candidate_wire(fixture.with_identity(candidate, identity))
    raw_bytes = fixture.encode_raw_wire(raw)
    canaries = [
        {
            "family": family,
            "pristine_typed_bytes_hex": fixture.encode_canary_wire(
                family,
                descriptor.name,
                repeat_index,
                nonce,
                after=False,
                pristine=True,
            ).hex(),
            "before_typed_bytes_hex": fixture.encode_canary_wire(
                family,
                descriptor.name,
                repeat_index,
                nonce,
                after=False,
            ).hex(),
            "after_typed_bytes_hex": fixture.encode_canary_wire(
                family,
                descriptor.name,
                repeat_index,
                nonce,
                after=True,
            ).hex(),
        }
        for family in contract.FAMILY_TOKENS
    ]
    return {
        "schema": contract.PROBE_SCHEMA,
        "name": descriptor.name,
        "top": descriptor.top,
        "expected_clock": descriptor.expected_clock,
        "context": dict(contract.CANONICAL_CONTEXT),
        "repeat_index": repeat_index,
        "run_nonce": nonce,
        "session_token": contract.SESSION_TOKEN,
        "pre_candidate_wire_hex": candidate_bytes.hex(),
        "post_candidate_wire_hex": candidate_bytes.hex(),
        "pre_raw_projection_hex": raw_bytes.hex(),
        "post_raw_projection_hex": raw_bytes.hex(),
        "canaries": canaries,
        "complete": True,
    }


def _validate(
    record: Record,
    descriptor: contract.CaseDescriptor,
    repeat_index: int,
    nonce: str,
) -> contract.ValidatedProbe:
    return contract.validate_probe_record(record, descriptor, repeat_index, nonce)


def _reject_record(
    name: str,
    baseline: Record,
    descriptor: contract.CaseDescriptor,
    repeat_index: int,
    nonce: str,
    mutation: Mutation,
) -> None:
    _validate(copy.deepcopy(baseline), descriptor, repeat_index, nonce)
    malformed = copy.deepcopy(baseline)
    mutation(malformed)
    if malformed == baseline:
        raise AssertionError(f"negative case {name!r} made no mutation")
    try:
        _validate(malformed, descriptor, repeat_index, nonce)
    except contract.ReferenceInputContractError:
        return
    raise AssertionError(f"negative case {name!r} unexpectedly validated")


def _write_at_double(writer: fixture.CanonicalWriter, value: Any) -> None:
    writer.enum(value.analysis)
    writer.enum(value.transition)
    writer.canary_double(value.value)


def _write_at_string(writer: fixture.CanonicalWriter, value: Any) -> None:
    writer.enum(value.analysis)
    writer.enum(value.transition)
    writer.string(value.value)


def _write_at_transition(writer: fixture.CanonicalWriter, value: Any) -> None:
    writer.enum(value.analysis)
    writer.enum(value.transition)
    writer.enum(value.value)


def _write_transition(writer: fixture.CanonicalWriter, value: Any) -> None:
    writer.enum(value.transition)
    writer.enum(value.value)


def _write_aio_double(writer: fixture.CanonicalWriter, value: Any) -> None:
    writer.enum(value.analysis)
    writer.enum(value.input_transition)
    writer.enum(value.output_transition)
    writer.canary_double(value.value)


def _write_timing_arc_double(writer: fixture.CanonicalWriter, value: Any) -> None:
    writer.i32(value.timing_arc_index)
    _write_aio_double(writer, value)


def _write_path_state(writer: fixture.CanonicalWriter, value: Any) -> None:
    writer.enum(value.analysis)
    writer.enum(value.path_source)
    writer.enum(value.transition)
    writer.string(value.clock_tag)
    writer.canary_double(value.arrival)
    writer.canary_double(value.slew)
    writer.canary_double(value.launch_time)
    writer.string(value.start_point)
    writer.string(value.predecessor)
    writer.string(value.clock_name)
    writer.string(value.crpr_clock_pin)
    writer.u64(value.predecessor_arc_idx)
    writer.canary_double(value.predecessor_arc_delay)
    writer.enum(value.trans_type)
    writer.enum(value.predecessor_trans_type)
    writer.enum(value.crpr_clock_trans_type)


def _write_paired(
    writer: fixture.CanonicalWriter,
    payload: wire.PairedMaps,
    item_writer: Callable[[fixture.CanonicalWriter, Any], None],
) -> None:
    writer.vector(payload.effective, lambda value: item_writer(writer, value))
    writer.vector(payload.physical, lambda value: item_writer(writer, value))


def _write_payload(  # noqa: PLR0911, PLR0912
    writer: fixture.CanonicalWriter,
    family: model.CanaryFamily,
    payload: Any,
) -> None:
    if family is model.CanaryFamily.TIMING_POINT_LEVEL:
        writer.i32(payload)
        return
    if family is model.CanaryFamily.TIMING_POINT_IS_CLOCK_POINT:
        writer.boolean(payload)
        return
    if family is model.CanaryFamily.TIMING_POINT_SCALARS:
        writer.canary_double(payload.arrival)
        writer.canary_double(payload.required)
        writer.canary_double(payload.slack)
        writer.canary_double(payload.launch_time)
        writer.string(payload.predecessor)
        writer.string(payload.clock_name)
        writer.u64(payload.predecessor_arc_idx)
        return
    if family in {
        model.CanaryFamily.TIMING_POINT_CLOCK_ARRIVAL_MAPS,
        model.CanaryFamily.TIMING_POINT_CLOCK_SLEW_MAPS,
        model.CanaryFamily.TIMING_POINT_CLOCK_PREDECESSOR_ARC_DELAY_MAPS,
    }:
        _write_paired(writer, payload, _write_at_double)
        return
    if family is model.CanaryFamily.TIMING_POINT_CLOCK_PREDECESSOR_MAPS:
        _write_paired(writer, payload, _write_at_string)
        return
    if family is model.CanaryFamily.TIMING_POINT_CLOCK_PREDECESSOR_TRANSITION_MAPS:
        _write_paired(writer, payload, _write_at_transition)
        return
    if family is model.CanaryFamily.TIMING_POINT_DATA_SLEW_MAP:
        writer.vector(payload, lambda value: _write_at_double(writer, value))
        return
    if family is model.CanaryFamily.TIMING_POINT_PATH_STATE_MAP:
        writer.vector(payload, lambda value: _write_path_state(writer, value))
        return
    if family is model.CanaryFamily.ARC_SCALARS:
        writer.canary_double(payload.delay)
        writer.canary_double(payload.delay_max)
        writer.canary_double(payload.delay_min)
        return
    if family is model.CanaryFamily.ARC_TRANSITION_MAPS:
        writer.vector(payload.delays, lambda value: _write_at_double(writer, value))
        writer.vector(payload.transitions, lambda value: _write_transition(writer, value))
        return
    if family in {
        model.CanaryFamily.ARC_INPUT_OUTPUT_DELAY_MAP,
        model.CanaryFamily.ARC_GRAPH_DELAY_MAP,
    }:
        writer.vector(payload, lambda value: _write_aio_double(writer, value))
        return
    if family is model.CanaryFamily.ARC_TIMING_ARC_DELAY_MAP:
        writer.vector(payload, lambda value: _write_timing_arc_double(writer, value))
        return
    if family is model.CanaryFamily.ARC_TIMING_ARC_FALLBACK_PROVENANCE:
        writer.boolean(payload.ordinary_min_replaced)
        writer.boolean(payload.ordinary_max_replaced)
        writer.boolean(payload.fallback_replaced)
        return
    if family is model.CanaryFamily.DATABASE_TIMING_ORDER:
        writer.vector(payload, writer.string)
        return
    raise AssertionError(f"unhandled canary family {family!r}")


def _write_subject(
    writer: fixture.CanonicalWriter,
    family: model.CanaryFamily,
    subject: wire.Subject,
) -> None:
    if family.value <= model.CanaryFamily.TIMING_POINT_PATH_STATE_MAP.value:
        writer.string(subject.key.full_name)
    elif family.value <= model.CanaryFamily.ARC_TIMING_ARC_FALLBACK_PROVENANCE.value:
        writer.u64(subject.key.native_rank)
        writer.string(subject.key.source)
        writer.string(subject.key.sink)
        writer.enum(subject.key.kind)
        writer.string(subject.key.owner)
        writer.string(subject.key.library_source_port)
        writer.string(subject.key.library_sink_port)
    else:
        writer.string(subject.key)
    _write_payload(writer, family, subject.payload)


def _encode_envelope(
    envelope: wire.CanaryEnvelope,
    *,
    domain_family: str | None = None,
) -> bytes:
    writer = fixture.CanonicalWriter()
    token = (
        model.CANARY_FAMILY_TOKENS[envelope.family.value]
        if domain_family is None
        else domain_family
    )
    writer.string(f"{fixture.CANARY_PREFIX}{token}{fixture.CANARY_SUFFIX}")
    writer.u64(envelope.binding.repeat_index)
    writer.string(envelope.binding.run_nonce)
    writer.u64(envelope.binding.session_token)
    writer.vector(
        envelope.subjects,
        lambda subject: _write_subject(writer, envelope.family, subject),
    )
    return writer.result()


def _decode_envelope(record: Record, family_index: int, side: str) -> wire.CanaryEnvelope:
    item = record["canaries"][family_index]
    payload = bytes.fromhex(item[f"{side}_typed_bytes_hex"])
    return wire.decode_canary_wire(payload, item["family"])


def _put_envelope(
    record: Record,
    family_index: int,
    side: str,
    envelope: wire.CanaryEnvelope,
    *,
    domain_family: str | None = None,
) -> None:
    record["canaries"][family_index][f"{side}_typed_bytes_hex"] = _encode_envelope(
        envelope, domain_family=domain_family
    ).hex()


def _with_first_payload(
    envelope: wire.CanaryEnvelope,
    payload: Any,
) -> wire.CanaryEnvelope:
    first = replace(envelope.subjects[0], payload=payload)
    return replace(envelope, subjects=(first, *envelope.subjects[1:]))


def _with_first_key(
    envelope: wire.CanaryEnvelope,
    key: Any,
) -> wire.CanaryEnvelope:
    first = replace(envelope.subjects[0], key=key)
    return replace(envelope, subjects=(first, *envelope.subjects[1:]))


def _replace_wire_byte(payload: bytes, offset: int, value: int) -> bytes:
    result = bytearray(payload)
    result[offset] = value
    return bytes(result)


def _mutate_canary_bytes(
    record: Record,
    family_index: int,
    side: str,
    mutation: Callable[[bytes], bytes],
) -> None:
    item = record["canaries"][family_index]
    field = f"{side}_typed_bytes_hex"
    item[field] = mutation(bytes.fromhex(item[field])).hex()


def _restore_first_value(old: tuple[Any, ...], new: tuple[Any, ...]) -> tuple[Any, ...]:
    return (replace(new[0], value=old[0].value), *new[1:])


def _required_leaf_mutations(  # noqa: PLR0911
    family: model.CanaryFamily,
    old: Any,
    new: Any,
) -> tuple[tuple[str, Any], ...]:
    if family in {
        model.CanaryFamily.TIMING_POINT_LEVEL,
        model.CanaryFamily.TIMING_POINT_IS_CLOCK_POINT,
    }:
        return (("value", old),)
    if isinstance(old, wire.PointScalars):
        names = (
            "arrival",
            "required",
            "slack",
            "launch_time",
            "predecessor",
            "clock_name",
            "predecessor_arc_idx",
        )
        return tuple((name, replace(new, **{name: getattr(old, name)})) for name in names)
    if isinstance(old, wire.PairedMaps):
        return tuple(
            (
                side,
                replace(
                    new,
                    **{side: _restore_first_value(getattr(old, side), getattr(new, side))},
                ),
            )
            for side in ("effective", "physical")
        )
    if isinstance(old, wire.ArcScalars):
        names = ("delay", "delay_max", "delay_min")
        return tuple((name, replace(new, **{name: getattr(old, name)})) for name in names)
    if isinstance(old, wire.ArcTransitionMaps):
        return (
            ("trans_delay", replace(new, delays=_restore_first_value(old.delays, new.delays))),
            (
                "trans_type",
                replace(
                    new,
                    transitions=_restore_first_value(old.transitions, new.transitions),
                ),
            ),
        )
    if isinstance(old, wire.FallbackBehavior):
        return tuple(
            (name, replace(new, **{name: value}))
            for name, value in (
                ("fallback_replaced", old.fallback_replaced),
                ("ordinary_max_replaced", not new.ordinary_max_replaced),
            )
        )
    if family is model.CanaryFamily.TIMING_POINT_PATH_STATE_MAP:
        names = (
            "arrival",
            "slew",
            "launch_time",
            "start_point",
            "predecessor",
            "clock_name",
            "crpr_clock_pin",
            "predecessor_arc_idx",
            "predecessor_arc_delay",
            "trans_type",
            "predecessor_trans_type",
            "crpr_clock_trans_type",
        )
        return tuple(
            (
                name,
                (
                    replace(new[0], **{name: getattr(old[0], name)}),
                    *new[1:],
                ),
            )
            for name in names
        )
    if family is model.CanaryFamily.DATABASE_TIMING_ORDER:
        return (("timing_order", old),)
    return (("value", _restore_first_value(old, new)),)


def _set_first_payload(
    record: Record,
    family_index: int,
    side: str,
    payload: Any,
) -> None:
    envelope = _decode_envelope(record, family_index, side)
    _put_envelope(
        record,
        family_index,
        side,
        _with_first_payload(envelope, payload),
    )


def _set_all_bindings(record: Record, family_index: int, **changes: Any) -> None:
    for side in ("pristine", "before", "after"):
        envelope = _decode_envelope(record, family_index, side)
        _put_envelope(
            record,
            family_index,
            side,
            replace(envelope, binding=replace(envelope.binding, **changes)),
        )


def _set_all_subject_keys(record: Record, family_index: int, key: Any) -> None:
    for side in ("pristine", "before", "after"):
        envelope = _decode_envelope(record, family_index, side)
        _put_envelope(record, family_index, side, _with_first_key(envelope, key))


def _reject_envelope_matrix(
    baseline: Record,
    descriptor: contract.CaseDescriptor,
    repeat_index: int,
    nonce: str,
) -> None:
    def reject(name: str, mutation: Mutation) -> None:
        _reject_record(name, baseline, descriptor, repeat_index, nonce, mutation)

    reject("root-extra", lambda item: item.__setitem__("unexpected", True))
    reject(
        "canary-key-missing",
        lambda item: item["canaries"][0].pop("pristine_typed_bytes_hex"),
    )
    reject("canary-key-extra", lambda item: item["canaries"][0].__setitem__("x", "00"))
    reject("family-missing", lambda item: item["canaries"].pop())
    reject("family-overflow", lambda item: item["canaries"].append(item["canaries"][0]))
    reject(
        "family-unknown",
        lambda item: item["canaries"][0].__setitem__("family", "unknown"),
    )
    reject(
        "family-duplicate",
        lambda item: item["canaries"].__setitem__(1, copy.deepcopy(item["canaries"][0])),
    )
    reject("family-permuted", lambda item: item["canaries"].reverse())
    reject(
        "canary-truncated",
        lambda item: _mutate_canary_bytes(item, 0, "after", lambda value: value[:-1]),
    )
    reject(
        "canary-trailing",
        lambda item: _mutate_canary_bytes(item, 0, "after", lambda value: value + b"\x00"),
    )

    def wrong_domain(item: Record) -> None:
        envelope = _decode_envelope(item, 0, "after")
        _put_envelope(item, 0, "after", envelope, domain_family=contract.FAMILY_TOKENS[1])

    reject("wrong-canary-domain", wrong_domain)
    reject(
        "wrong-outer-family",
        lambda item: item["canaries"][0].__setitem__(
            "after_typed_bytes_hex", item["canaries"][1]["after_typed_bytes_hex"]
        ),
    )
    reject("wrong-binding-token", lambda item: _set_all_bindings(item, 0, session_token=2))
    reject("wrong-binding-nonce", lambda item: _set_all_bindings(item, 0, run_nonce="f" * 32))
    reject(
        "wrong-subject-inventory",
        lambda item: _set_all_subject_keys(item, 0, wire.PointKey("not-a-raw-node")),
    )

    def bad_enum(item: Record) -> None:
        envelope = _decode_envelope(item, 10, "after")
        bad_key = replace(envelope.subjects[0].key, kind=model.Transition.NONE)
        _put_envelope(item, 10, "after", _with_first_key(envelope, bad_key))

    reject("bad-enum", bad_enum)
    reject(
        "bad-bool",
        lambda item: _mutate_canary_bytes(
            item, 15, "after", lambda value: _replace_wire_byte(value, len(value) - 1, 2)
        ),
    )
    reject(
        "bad-double",
        lambda item: _mutate_canary_bytes(
            item, 10, "after", lambda value: _replace_wire_byte(value, len(value) - 9, 3)
        ),
    )


def _reject_completeness_matrix(
    baseline: Record,
    descriptor: contract.CaseDescriptor,
    repeat_index: int,
    nonce: str,
) -> None:
    for family_index, token in enumerate(contract.FAMILY_TOKENS):
        _reject_record(
            f"one-hot-missing-{token}",
            baseline,
            descriptor,
            repeat_index,
            nonce,
            lambda item, index=family_index: item["canaries"][index].__setitem__(
                "after_typed_bytes_hex",
                item["canaries"][index]["before_typed_bytes_hex"],
            ),
        )
        before = _decode_envelope(baseline, family_index, "before")
        after = _decode_envelope(baseline, family_index, "after")
        old_payload = before.subjects[0].payload
        new_payload = after.subjects[0].payload
        for leaf, incomplete_payload in _required_leaf_mutations(
            before.family, old_payload, new_payload
        ):
            _reject_record(
                f"required-leaf-{token}-{leaf}",
                baseline,
                descriptor,
                repeat_index,
                nonce,
                lambda item, index=family_index, payload=incomplete_payload: _set_first_payload(
                    item, index, "after", payload
                ),
            )


def _set_scaffold_pair(
    record: Record,
    family_index: int,
    pristine_payload: Any,
    before_payload: Any,
) -> None:
    _set_first_payload(record, family_index, "pristine", pristine_payload)
    _set_first_payload(record, family_index, "before", before_payload)


def _reject_scaffold_matrix(
    baseline: Record,
    descriptor: contract.CaseDescriptor,
    repeat_index: int,
    nonce: str,
) -> None:
    def reject(name: str, mutation: Mutation) -> None:
        _reject_record(name, baseline, descriptor, repeat_index, nonce, mutation)

    family_index = model.CanaryFamily.TIMING_POINT_DATA_SLEW_MAP.value
    default = wire.AnalysisTransitionDouble(model.AnalysisMode.MAX, model.Transition.RISE, 7.0)
    extra = wire.AnalysisTransitionDouble(model.AnalysisMode.MAX, model.Transition.FALL, 8.0)
    cases = (
        ("precondition-wrong", (), (replace(default, value=8.0),)),
        ("precondition-extra", (), (default, extra)),
        ("precondition-still-empty", (), ()),
    )
    for name, pristine_payload, before_payload in cases:
        reject(
            name,
            lambda item, pristine=pristine_payload, before=before_payload: _set_scaffold_pair(
                item, family_index, pristine, before
            ),
        )
    original = _decode_envelope(baseline, family_index, "pristine").subjects[0].payload
    drifted = (replace(original[0], transition=model.Transition.FALL),)
    reject(
        "precondition-shape-drift",
        lambda item: _set_first_payload(item, family_index, "before", drifted),
    )
    reject(
        "non-map-pristine-drift",
        lambda item: _set_first_payload(item, 0, "pristine", -1),
    )
    reject(
        "timing-arc-fallback-scaffold-missing",
        lambda item: _set_first_payload(
            item,
            model.CanaryFamily.ARC_TIMING_ARC_DELAY_MAP.value,
            "before",
            _decode_envelope(
                item,
                model.CanaryFamily.ARC_TIMING_ARC_DELAY_MAP.value,
                "pristine",
            )
            .subjects[0]
            .payload,
        ),
    )
    reject(
        "fallback-provenance-scaffold-wrong",
        lambda item: _set_first_payload(
            item,
            model.CanaryFamily.ARC_TIMING_ARC_FALLBACK_PROVENANCE.value,
            "pristine",
            wire.FallbackBehavior(False, False, False),
        ),
    )


def run_negative_matrix(manifest: Path) -> None:
    descriptor = contract.load_case_descriptors(manifest)[0]
    repeat_index = 0
    nonce = "a" * 32
    baseline = _valid_record(descriptor, repeat_index, nonce)
    _validate(copy.deepcopy(baseline), descriptor, repeat_index, nonce)
    _reject_envelope_matrix(baseline, descriptor, repeat_index, nonce)
    _reject_completeness_matrix(baseline, descriptor, repeat_index, nonce)
    _reject_scaffold_matrix(baseline, descriptor, repeat_index, nonce)

    def reject(name: str, mutation: Mutation) -> None:
        _reject_record(name, baseline, descriptor, repeat_index, nonce, mutation)

    canned = _valid_record(descriptor, 1, "b" * 32)["canaries"]
    reject(
        "cross-run-canned-pair",
        lambda item: item.__setitem__("canaries", copy.deepcopy(canned)),
    )

    _candidate, raw = fixture.fixture_case(descriptor.name)
    wrong_raw = replace(raw, nodes=tuple(reversed(raw.nodes)))
    wrong_raw_hex = fixture.encode_raw_wire(wrong_raw).hex()
    reject(
        "wrong-raw-inventory",
        lambda item: (
            item.__setitem__("pre_raw_projection_hex", wrong_raw_hex),
            item.__setitem__("post_raw_projection_hex", wrong_raw_hex),
        ),
    )
