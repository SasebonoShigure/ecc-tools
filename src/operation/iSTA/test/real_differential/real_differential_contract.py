#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# ***************************************************************************************
import argparse
import json
import math
import os
import stat
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import real_differential_wire_contract as wire
import reference_input_candidate_wire as candidate_wire
import reference_input_raw_wire as raw_wire
import reference_input_recompute_fingerprint as fingerprint_oracle
import reference_input_recompute_graph as graph_oracle
import reference_input_recompute_model as model_oracle

BaselineError = wire.BaselineError
ControllerRun = wire.ControllerRun
DifferentialContractError = wire.DifferentialContractError
LaneSlot = wire.LaneSlot
PairSlot = wire.PairSlot
load_manifest = wire.load_manifest
parse_strict_json_for_test = wire.parse_strict_json_for_test
_hex = wire.hex_bytes
_leaves = wire.wire_leaves
_map = wire.mapping
_r = wire.require
_schema = wire.validate_schema_fragment
_sha = wire.sha256_bytes
_timing = wire.validate_neutral_wire
_number = wire.wire_number
_names = str.split
ensure = _r
_J = dict(ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)


def _same(actual, names, expected):
    return all(actual[name] == value for name, value in zip(names, expected, strict=True))


def _validate_scenario(value, case, timing, where):
    _schema("#/$defs/scenario", value, where)
    scenario = _map(value, where)
    valid = scenario["schema"] == "ista-csr-real-differential-scenario/1.0.0"
    pre = scenario["pre_full"]
    size = pre["library_size_before"]
    names = _names(
        "phase state guard_owner token_nonzero member_address_stable bound_graph_arc_count "
        "descriptor library_match_count library_size_before library_size_after disabled_before "
        "disabled_after"
    )
    suffix = (
        (case.raw_descriptor["disabled_anchor"], 1, size, size, False, True)
        if case.is_extended
        else (None, 0, 0, 0, None, None)
    )
    values = ("pre_full", "kInitialized", "session", True, True, 0) + suffix
    valid = valid and (not case.is_extended or size > 0) and _same(pre, names, values)
    disabled, parallel, coverage = (scenario[key] for key in ("disabled", "parallel", "coverage"))
    if case.is_extended:
        names = _names(
            "library_match_count graph_match_count member_use_count applicable same_member "
            "library_size_stable member_disabled graph_disabled source_seed_and_candidate_absent "
            "graph_delay_typed_empty input_output_delay_initialized timing_arc_delay_initialized "
            "timing_arc_fallback_initialized complete"
        )
        valid = valid and _same(disabled, names, (1, 1, 1) + (True,) * (len(names) - 3))
        original, clone = parallel["original_rank"], parallel["clone_rank"]
        names = _names(
            "applicable original_rank clone_rank pre_outgoing post_outgoing same_member "
            "active_count disabled_count outgoing_added incoming_added complete"
        )
        values = (True, original, clone, [original], [original, clone], True, 2, 0, 1, 1, True)
        valid = valid and clone > original and _same(parallel, names, values)
        loop = case.loop_disabled_witness
        names = _names("chain diamond extended non_unate multi_root complete")
        valid = valid and _same(coverage, names, (False, False, True, True, True, True))
        valid = (
            valid
            and coverage["loop_disabled_count"] >= loop.minimum_count
            and coverage["loop_disabled_minimum"] == loop.minimum_count
            and coverage["clock_tags"] == list(case.clock_tags)
            and coverage["near_tie_descriptor"] == case.raw_descriptor["near_tie"]
        )
    else:
        names = _names("chain diamond extended complete")
        values = (case.name == "chain", case.name == "diamond", False, True)
        valid = disabled == {"applicable": False} and parallel == {"applicable": False}
        valid = valid and _same(coverage, names, values)
    names = _names("existing_graph_runs stages complete")
    stages = ["delay_runtime", "clock_propagator", "timing_propagator", "timing_analyzer"]
    valid = valid and _same(scenario["stabilization"], names, (1, stages, True))
    _r(valid, f"{where} machine witness differs")
    levels = [
        {"level": node["level"], "node": node["key"]["full_name"]}
        for node in sorted(timing["nodes"], key=lambda item: item["key"]["full_name"].encode())
    ]
    expected = canonical_json_bytes(
        {"levels": levels, "order": [key["full_name"] for key in timing["timing_order"]]}
    )
    receipt = scenario["level_order"]
    native = _hex(receipt["native_bytes_hex"], f"{where}.level_order.native")
    replay = _hex(receipt["replay_bytes_hex"], f"{where}.level_order.replay")
    values = (_sha(native), _sha(replay), True)
    valid = native == replay == expected
    valid = valid and _same(receipt, _names("native_sha256 replay_sha256 equal"), values)
    _r(valid, f"{where} level/order receipt differs from actual timing")
    return scenario


def _one(matches, message):
    _r(len(matches) == 1, message)
    return matches[0]


def _node_key(value):
    kind = value.kind if isinstance(value.kind, str) else ("port", "pin")[value.kind]
    return vars(value) | {"kind": kind}


def _validate_extended(candidate, case, timing, scenario):
    def _selector_matches(candidate, arc, selector):
        key = arc.key
        prefix = f"{selector.instance}:"
        actual = key.source.full_name, key.sink.full_name, key.owner_name
        actual += key.library_source_port, key.library_sink_port
        expected = prefix + selector.source_pin, prefix + selector.sink_pin, *selector.values()[-3:]
        valid = candidate.nodes[arc.source].instance_cell == selector.cell and int(key.kind) == 0
        return valid and actual == expected

    def _selector_arcs(wire_value, selector):
        names = _names("owner_name library_source_port library_sink_port")
        source = f"{selector.instance}:{selector.source_pin}"
        sink = f"{selector.instance}:{selector.sink_pin}"
        return [
            arc
            for arc in wire_value["arcs"]
            if arc["key"]["kind"] == "cell"
            and arc["key"]["source"]["full_name"] == source
            and arc["key"]["sink"]["full_name"] == sink
            and all(arc["key"][name] == getattr(selector, name) for name in names)
        ]

    def _node(wire_value, key):
        matches = [node for node in wire_value["nodes"] if node["key"] == key]
        return _one(matches, "NeutralWire NodeKey witness is not unique")

    def _path(node, analysis, source, transition, tag):
        expected = analysis, source, transition, tag
        names = _names("analysis path_source transition clock_tag")
        matches = [
            item for item in node["paths"] if tuple(item[name] for name in names) == expected
        ]
        return _one(matches, "NeutralWire path witness is not unique")

    def _reachable(candidate, root_name, target_name):
        ids = {node.key.full_name: node.node_id for node in candidate.nodes}
        _r(root_name in ids and target_name in ids, "reachability endpoint is absent")
        pending, reached = {ids[root_name]}, set()
        while pending:
            source = pending.pop()
            reached.add(source)
            pending.update(
                arc.sink
                for arc in candidate.arcs
                if arc.source == source
                and not arc.is_disabled
                and not arc.is_loop_disabled
                and arc.sink not in reached
            )
        return ids[target_name] in reached

    selectors = case.non_unate_anchor, case.disabled_anchor, case.parallel_anchor
    anchors = [
        [arc for arc in candidate.arcs if _selector_matches(candidate, arc, selector)]
        for selector in selectors
    ]
    _r(tuple(map(len, anchors)) == (1, 1, 2), "candidate branch anchor cardinality differs")
    non_unate, disabled = (items[0] for items in anchors[:2])
    parallel = anchors[2]
    valid = any(int(signature.sense) == 3 for signature in non_unate.key.library_arcs)
    valid = (
        valid
        and disabled.is_disabled
        and not disabled.is_loop_disabled
        and scenario["disabled"]["native_rank"] == disabled.native_traversal_rank
    )
    expected = [scenario["parallel"][name] for name in ("original_rank", "clone_rank")]
    valid = valid and all(not arc.is_disabled and not arc.is_loop_disabled for arc in parallel)
    valid = valid and sorted(arc.key.parallel_ordinal for arc in parallel) == [0, 1]
    valid = valid and sorted(arc.native_traversal_rank for arc in parallel) == expected
    loop = case.loop_disabled_witness
    loop_count = sum(
        arc.is_loop_disabled
        and (arc.key.source.full_name, arc.key.sink.full_name, arc.key.owner_name)
        == (loop.source, loop.sink, loop.owner_name)
        for arc in candidate.arcs
    )
    valid = valid and loop_count >= loop.minimum_count
    valid = valid and scenario["coverage"]["loop_disabled_count"] == loop_count
    _r(valid, "candidate branch/loop witness differs")

    message = "NeutralWire branch anchor cardinality differs"
    non_wire, disabled_wire = (
        _one(_selector_arcs(timing, selector), message) for selector in selectors[:2]
    )
    graph = non_wire["graph_delays"][0]
    disabled_source = _node(timing, disabled_wire["key"]["source"])
    valid = all(any(item is not None for item in lane) for lane in graph)
    valid = (
        valid
        and _number(disabled_source["arrival"], "disabled arrival") == -math.inf
        and _number(disabled_source["required"], "disabled required") == math.inf
        and not disabled_source["paths"]
        and all(item is None for item in _leaves(disabled_source["data_slews"]))
    )
    valid = valid and all(item is None for item in _leaves(disabled_wire["graph_delays"]))
    valid = valid and any(
        item is not None for item in _leaves(disabled_wire["input_output_delays"])
    )
    fallback = (
        item
        for entry in disabled_wire["timing_arcs"]
        for item in _leaves(entry["lanes"])
        if item is not None and item["is_fallback"] is True
    )
    _r(valid and any(fallback), "NeutralWire disabled/non-unate witness differs")

    near = case.near_tie
    join_key = _node_key(near.join_node)
    join = _node(timing, join_key)
    arcs = [
        _one(_selector_arcs(timing, selector), "near-tie Arc witness is not unique")
        for selector in (near.row_first_arc, near.row_second_arc)
    ]
    keys = [arc["key"] for arc in arcs]
    node_index = next(
        index for index, node in enumerate(timing["nodes"]) if node["key"] == join_key
    )
    path_args = near.analysis, near.path_source, near.transition
    source_paths = [_path(_node(timing, key["source"]), *path_args, near.clock_tag) for key in keys]
    primary = _node_key(case.data_roots[0])
    valid = timing["in_rows"][node_index]["arcs"] == keys
    valid = valid and all(
        path["start_node"] == primary and path["clock_name"] == near.clock_tag
        for path in source_paths
    )
    candidates = [
        _number(path["arrival"], "near source arrival")
        + _number(arc["graph_delays"][0][0][0], "near graph delay")
        for path, arc in zip(source_paths, arcs, strict=True)
    ]
    _r(valid and all(map(math.isfinite, candidates)), "near-tie input/row witness differs")
    delta = abs(candidates[0] - candidates[1])
    _r(near.lower_exclusive < delta < near.upper_exclusive, "near-tie-actual-numeric")
    direction = 1 if near.analysis == "max" else -1
    strict = int(direction * candidates[1] > direction * candidates[0])
    epsilon = int(direction * candidates[1] > direction * candidates[0] + near.upper_exclusive)
    valid = strict != epsilon and join["predecessor_node"] == keys[strict]["source"]
    valid = valid and join["predecessor_arc"] == keys[strict]
    joined = _path(join, *path_args, near.clock_tag)
    valid = valid and joined["predecessor_node"] == keys[epsilon]["source"]
    valid = valid and joined["predecessor_arc"] == keys[epsilon]
    _r(valid, "near-tie scalar/epsilon winner differs")
    roots = list(zip(map(_node_key, case.data_roots), case.clock_tags, strict=True))
    paths = [_path(_node(timing, key), *path_args, tag) for key, tag in roots]
    valid = all(
        path["start_node"] == key and path["clock_name"] == tag
        for path, (key, tag) in zip(paths, roots, strict=True)
    )
    secondary = near.secondary_root.full_name
    valid = valid and _reachable(candidate, case.data_roots[0].full_name, near.join_node.full_name)
    valid = valid and not _reachable(candidate, secondary, near.join_node.full_name)
    valid = valid and not any(
        path["clock_tag"] == near.secondary_clock_tag for path in join["paths"]
    )
    _r(valid, "multi-root/secondary reachability witness differs")


def _equivalent(left: Any, right: Any, absolute: float, relative: float) -> bool:
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return set(left) == set(right) and all(
            _equivalent(left[key], right[key], absolute, relative) for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _equivalent(lhs, rhs, absolute, relative) for lhs, rhs in zip(left, right, strict=True)
        )
    if isinstance(left, float):
        return (
            math.isfinite(left)
            and math.isfinite(right)
            and (left != 0.0 or math.copysign(1.0, left) == math.copysign(1.0, right))
            and abs(left - right) <= absolute + relative * max(abs(left), abs(right))
        )
    return (isinstance(left, (bool, str, int)) or left is None) and left == right


def _validate_canary(reference, candidate):
    def _validate_coverage(value):
        _schema("#/$defs/coverage", value, "reference.coverage")
        coverage = _map(value, "reference.coverage")
        receipt = coverage["canary_receipt"]
        flags = _names("raw_unchanged fingerprints_unchanged disabled_arc_preserved")
        _r(all(receipt[key] is True for key in flags), "canary provenance flag differs")
        stages = (
            ("DelayCalculator", "graph_delay", wire.arc_key_bytes),
            ("ClockPropagator", "is_clock_point", wire.node_key_bytes),
            ("TimingPropagator", "arrival", wire.node_key_bytes),
            ("TimingAnalyzer", "required+slack", wire.node_key_bytes),
        )
        fields = receipt["stage_fields"]
        _r(len(fields) == 4, "canary stage field count differs")
        flags = _names("active reachable present before_after_differ post_rerun_overwritten")
        for item, (stage, field, decoder) in zip(fields, stages, strict=True):
            valid = item["stage"] == stage and item["field"] == field
            _r(valid and all(item[key] is True for key in flags), "canary stage receipt differs")
            decoder(item["semantic_key"])
        return fields

    def _candidate_arc(candidate, key):
        kind = 0 if key["kind"] == "cell" else 1
        names = _names("owner_name library_source_port library_sink_port parallel_ordinal")
        matches = [
            arc
            for arc in candidate.arcs
            if int(arc.kind) == kind
            and arc.key.source.full_name == key["source"]["full_name"]
            and arc.key.sink.full_name == key["sink"]["full_name"]
            and all(getattr(arc.key, name) == key[name] for name in names)
        ]
        return _one(matches, "canary ArcKey does not resolve to candidate")

    fields = _validate_coverage(reference["coverage"])
    correct = _timing(reference["reference_timing"], "reference.reference_timing")
    canary = _timing(reference["canary_timing"], "reference.canary_timing")
    restored = json.loads(json.dumps(canary, ensure_ascii=False, allow_nan=False))
    specs = {
        "DelayCalculator": ("arcs", ("graph_delays",)),
        "ClockPropagator": ("nodes", ("is_clock_point",)),
        "TimingPropagator": ("nodes", ("arrival",)),
        "TimingAnalyzer": ("nodes", ("required", "slack")),
    }
    for receipt in fields:
        stage, key = receipt["stage"], receipt["semantic_key"]
        group, names = specs[stage]
        message = f"canary {group} semantic key does not resolve"
        selected = [
            _one([item for item in timing[group] if item["key"] == key], message)
            for timing in (correct, canary, restored)
        ]
        before, after, target = selected
        left, right = ([item[name] for name in names] for item in (before, after))
        _r(left != right, f"canary-delta:{stage}")
        if stage == "DelayCalculator":
            valid = key["kind"] == "cell"
            valid = valid and any(item is not None for item in _leaves(before["graph_delays"]))
            if candidate is not None:
                arc = _candidate_arc(candidate, key)
                valid = valid and not arc.is_disabled and not arc.is_loop_disabled
                valid = valid and arc.eligibility.data_arrival
        elif stage == "ClockPropagator":
            valid = before["is_clock_point"] is True and bool(before["clock_name"])
        elif stage == "TimingPropagator":
            valid = bool(before["paths"])
            valid = valid and math.isfinite(_number(before["arrival"], "canary arrival"))
        else:
            valid = all(math.isfinite(_number(before[name], f"canary {name}")) for name in names)
            if candidate is not None:
                matches = [node for node in candidate.nodes if _node_key(node.key) == key]
                valid = valid and _one(matches, "canary NodeKey does not resolve").is_endpoint
        _r(valid, f"{stage} canary target is not active/reachable/present")
        target.update({name: before[name] for name in names})
    _r(_equivalent(restored, correct, 0.0, 0.0), "canary-field-drift")


def _validate_reference(record, case, timing):
    def _validate_observation(value, identity, wire_value):
        observation = _map(value, "reference.observation")
        nodes, arcs = len(wire_value["nodes"]), len(wire_value["arcs"])
        levels = max(node["level"] for node in wire_value["nodes"])
        extents = dict.fromkeys(_names("node_keys level_nodes node_to_level"), nodes)
        extents.update(dict.fromkeys(_names("arc_keys out_arc_ids in_arc_ids"), arcs))
        extents.update(
            {"out_row_offsets": nodes + 1, "in_row_offsets": nodes + 1, "level_offsets": levels + 1}
        )
        observed = []
        for key, state in (("ready_post_scatter", "ready"), ("released", "released")):
            item = observation[key]
            names = _names("schema_version state identity counts")
            values = ("1.0.0", state, identity, {"nodes": nodes, "arcs": arcs, "levels": levels})
            valid = _same(item, names, values)
            valid = valid and all(
                item["buffers"][name]["logical_size"] == size
                and item["buffers"][name]["capacity"] >= size
                for name, size in extents.items()
            )
            names = _names("capacity support last_rejection last_invalidation")
            values = (
                {"last_action": "initial_allocate", "event_count": 1},
                {"reference_supported": True, "unsupported_reasons": []},
                None,
                None,
            )
            valid = valid and _same(item, names, values) and item["host_bytes"]["logical"] > 0
            valid = valid and item["host_bytes"]["capacity"] >= item["host_bytes"]["logical"]
            _r(valid, f"observation {key} state/extents/support differ")
            observed.append({name: value for name, value in item.items() if name != "state"})
        _r(observed[0] == observed[1], "released observation changed immutable fields")

    for key in ("scattered_timing", "canary_timing", "rerun_timing"):
        _timing(record[key], f"reference.{key}")
    source = record["source_wires"]
    raw_source = _hex(source["pre_poison_raw_wire_hex"], "source pre-poison raw")
    candidate_bytes = _hex(source["snapshot_candidate_wire_hex"], "source candidate")
    valid = _sha(raw_source) == source["pre_poison_raw_sha256"]
    valid = valid and _sha(candidate_bytes) == source["snapshot_candidate_sha256"]
    _r(valid, "bad-sha source wire")
    try:
        candidate = candidate_wire.decode_candidate_wire(candidate_bytes)
    except RuntimeError as exc:
        raise DifferentialContractError(f"candidate strict decode failed: {exc}") from exc
    identity = candidate.source_identity
    _r(identity.session_token > 0 and identity.graph_generation == 0, "prebuild identity")
    _r(not candidate.unsupported_reasons, "unsupported candidate")
    expected = {
        "fingerprint_schema_version": identity.fingerprint_schema_version,
        "session_token": identity.session_token,
        "graph_generation": identity.graph_generation,
        **{key: getattr(identity, key) for key in wire.FINGERPRINT_KEYS},
    }
    _schema("#/$defs/identity", record["identity"], "reference.identity")
    _r(_map(record["identity"], "reference.identity") == expected, "hash-drift identity")
    poison = zip(wire.POISON_FAMILIES, record["poison"], strict=True)
    valid = len(record["poison"]) == len(wire.POISON_FAMILIES) and all(
        item["family"] == family and item["before_sha256"] != item["after_sha256"]
        for family, item in poison
    )
    _r(valid, "poison family order/change differs")
    projection = record["forbidden_projection"]
    receipts = [
        {"checkpoint": name, "call_ordinal": ordinal}
        for ordinal, name in enumerate(wire.CHECKPOINTS)
    ]
    _r(projection["projection_call_receipt"] == receipts, "checkpoint-copy receipt")
    checkpoints = []
    for name in wire.CHECKPOINTS:
        item = projection[name]
        payload = _hex(item["raw_wire_hex"], f"projection.{name}.wire")
        _r(_sha(payload) == item["raw_sha256"], f"bad-sha projection {name}")
        checkpoints.append(payload)
    _r(checkpoints[0] == raw_source and len(set(checkpoints)) == 1, "raw-provenance bytes")
    try:
        for payload in checkpoints:
            decoded = raw_wire.decode_raw_wire(payload)
            graph = graph_oracle.recompute_graph(candidate, decoded)
            model_oracle.recompute_model(candidate, decoded, graph)
            actual = fingerprint_oracle.recompute_identity(
                candidate, identity.session_token, identity.graph_generation
            )
            _r(actual == identity, "raw-provenance six-hash identity differs")
    except RuntimeError as exc:
        raise DifferentialContractError(f"raw-provenance recomputation failed: {exc}") from exc
    scatter = record["scatter"]
    names = _names(
        "code writes_applied invalidation_reason current_capture_error current_fingerprints"
    )
    writes = 21 * len(timing["nodes"]) + 8 * len(timing["arcs"]) + 1
    fingerprints = {key: expected[key] for key in wire.FINGERPRINT_KEYS}
    values = ("success", writes, None, None, fingerprints)
    _r(_same(scatter, names, values), "scatter success/write/fingerprint receipt differs")
    _validate_observation(record["observation"], expected, timing)
    if case.is_extended:
        _validate_extended(candidate, case, timing, record["scenario"])
    _validate_canary(record, candidate)


def validate_probe(raw, case, lane, repeat, nonce, pid, argv, executable, directory, temporary):
    def _validate_argv_receipt(value, case, argv, executable, directory, temporary):
        _schema("#/$defs/argvReceipt", value, "probe.argv_receipt")
        receipt = _map(value, "probe.argv_receipt")
        clock_sha = _sha(wire.canonical_clock_json(case.expected_clocks).encode())
        valid = tuple(receipt["argv"]) == argv and receipt["clock_bytes_sha256"] == clock_sha
        _r(valid, "stale probe argv/clock receipt differs")
        names = _names("cwd executable run_dir result_temp")
        values = tuple(map(str, (directory, executable, directory, temporary)))
        _r(_same(receipt, names, values), "stale probe argv path receipt differs")
        manifest = {
            "path": str(case.descriptor_manifest),
            "sha256": case.descriptor_manifest_sha256,
        }
        inputs = [
            {"label": item.label, "path": str(item.path), "sha256": item.sha256}
            for item in case.inputs
        ]
        valid = receipt["descriptor_manifest"] == manifest and receipt["resolved_inputs"] == inputs
        _r(valid, "probe descriptor/input receipt differs")

    _r(lane in {"oracle", "reference"}, "probe lane is unknown")
    if isinstance(raw, dict):
        _r(raw.get("schema") == wire.PROBE_SCHEMA, "bad-schema probe")
    wire.validate_probe_document(raw, f"{lane} probe")
    record = _map(raw, f"{lane} probe")
    names = _names("schema lane case top expected_clocks")
    values = (wire.PROBE_SCHEMA, lane, case.name, case.top, list(case.expected_clocks))
    _r(_same(record, names, values), f"{lane} probe identity/context differs")
    names = _names("repeat_index run_nonce input_case_sha256 complete")
    values = (repeat, nonce, case.input_case_sha256, True)
    _r(_same(record, names, values), f"{lane} probe repeat/context differs")
    _r(record["process_id"] == pid, "bad-pid probe process differs")
    _validate_argv_receipt(record["argv_receipt"], case, argv, executable, directory, temporary)
    timing = _timing(
        record["timing"] if lane == "oracle" else record["reference_timing"], f"{lane}.timing"
    )
    _validate_scenario(record["scenario"], case, timing, f"{lane}.scenario")
    if lane == "reference":
        _validate_reference(record, case, timing)
    return record


def compare_neutral(left, right, absolute, relative, where="timing"):
    _r(_equivalent(left, right, absolute, relative), f"{where} differs")


def validate_pair(oracle, reference, bundle, absolute, relative):
    numbers = type(absolute) in (int, float) and type(relative) in (int, float)
    valid = numbers and math.isfinite(absolute) and math.isfinite(relative)
    valid = valid and absolute >= 0.0 and relative >= 0.0
    valid = valid and absolute == bundle.absolute_tolerance
    valid = valid and relative == bundle.relative_tolerance
    _r(valid, "tolerance must match immutable manifest bundle")
    correct = [_timing(oracle["timing"], "oracle.timing")]
    keys = ("reference_timing", "scattered_timing", "rerun_timing")
    correct += [_timing(reference[key], f"reference.{key}") for key in keys]
    canary = _timing(reference["canary_timing"], "reference.canary_timing")
    _r(oracle["scenario"] == reference["scenario"], "scenario receipts differ")
    for index, value in enumerate(correct[1:], 1):
        compare_neutral(correct[0], value, absolute, relative, f"correct timing {index}")
    _validate_canary(reference, None)
    valid = all(not _equivalent(canary, value, absolute, relative) for value in correct)
    _r(valid, "canary must differ from every correct wire")


def validate_controller_summary(raw, bundle, repeats, pid, nonce, executables, contexts):
    try:
        wire.validate_controller_document(raw)
    except DifferentialContractError as exc:
        if ".schema differs from schema constant" in str(exc):
            raise DifferentialContractError("summary-embedded-schema") from exc
        raise
    summary = _map(raw, "controller summary")
    _r(summary["controller_pid"] == pid, "summary-pid differs")
    _r(summary["controller_run_nonce"] == nonce, "summary-nonce differs")
    names = _names("schema complete requested_repeats")
    _r(_same(summary, names, (wire.CONTROLLER_SCHEMA, True, repeats)), "summary-context")
    valid = len(summary["case_results"]) == len(bundle.cases)
    valid = valid and len(contexts) == len(bundle.cases) * repeats
    _r(valid, "controller case/context cardinality differs")
    seen, topologies, cursor = {nonce}, set(), 0
    for result, case in zip(summary["case_results"], bundle.cases, strict=True):
        names = _names("case input_case_sha256")
        valid = _same(result, names, (case.name, case.input_case_sha256))
        valid = valid and len(result["repeats"]) == repeats
        _r(valid, "controller case descriptor/repeat count differs")
        fingerprints = []
        for index, entry in enumerate(result["repeats"]):
            slot, execution = contexts[cursor]
            valid = entry["repeat_index"] == index == slot.repeat_index
            valid = valid and slot.ordinal == cursor and slot.descriptor.name == case.name
            _r(valid and len(execution) == 2, "summary-repeat-order differs")
            _r(execution[0][0] != execution[1][0], "paired child PIDs are equal")
            records = []
            for lane, executable, (child_pid, argv) in zip(
                slot.lanes(), executables, execution, strict=True
            ):
                _r(lane.run_nonce not in seen, "controller nonce collision")
                seen.add(lane.run_nonce)
                try:
                    role = lane.role
                    args = entry[role], case, role, index, lane.run_nonce, child_pid, tuple(argv)
                    record = validate_probe(
                        *args, executable, lane.directory, lane.temporary_result
                    )
                except DifferentialContractError as exc:
                    if "bad-schema" in str(exc):
                        raise DifferentialContractError("summary-embedded-schema") from exc
                    raise
                records.append(record)
            validate_pair(
                records[0], records[1], bundle, bundle.absolute_tolerance, bundle.relative_tolerance
            )
            fingerprints.append(tuple(records[1]["identity"][key] for key in wire.FINGERPRINT_KEYS))
            cursor += 1
        _r(len(set(fingerprints)) == 1, "content fingerprints changed across repeats")
        index = wire.FINGERPRINT_KEYS.index("topology_fingerprint")
        topologies.add(fingerprints[0][index])
    valid = cursor == len(contexts) and len(topologies) == len(bundle.cases)
    _r(valid, "controller context/topology distinctness differs")
    return summary


def collect_error(errors, label, action, ignored=()):
    try:
        action()
    except ignored:
        pass
    except Exception as exc:
        errors.append(f"{label}: {exc}")


def file_identity(metadata):
    prefix = tuple(getattr(metadata, name) for name in ("st_dev", "st_ino", "st_size"))
    return *prefix, stat.S_IMODE(metadata.st_mode), metadata.st_uid, metadata.st_nlink


def positive_float(value):
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("timeout must be positive")
    return parsed


def resolve_probe(path, role):
    resolved = path.resolve()
    _r(resolved.is_file() and os.access(resolved, os.X_OK), f"{role} probe is not executable")
    return resolved


@dataclass(frozen=True, slots=True)
class SelfTestApi:
    manifest_path: Path
    baseline_manifest_path: Path
    spawn_and_drain: Callable[..., Any]
    prepare_layout: Callable[..., Any]
    commit_validated_run: Callable[..., Any]


def probe_argv(executable, slot, lane):
    case = slot.descriptor
    pairs = [("--descriptor-manifest", str(case.descriptor_manifest))]
    for label in _names("tech_lef cell_lef liberty verilog sdc"):
        pairs.append((f"--{label.replace('_', '-')}", str(case.input(label).path)))
    pairs += [
        ("--top", case.top),
        ("--case-name", case.name),
        ("--expected-clocks-json", wire.canonical_clock_json(case.expected_clocks)),
        ("--repeat-index", str(slot.repeat_index)),
        ("--input-case-sha256", case.input_case_sha256),
        ("--run-nonce", lane.run_nonce),
        ("--run-dir", str(lane.directory)),
        ("--result-temp", lane.temporary_result.name),
    ]
    return str(executable), *(value for pair in pairs for value in pair)


def build_controller_summary(bundle, results, repeats, nonce):
    cases = []
    for case in bundle.cases:
        records = [
            {"repeat_index": slot.repeat_index, "oracle": oracle, "reference": reference}
            for slot, oracle, reference, *_ in results
            if slot.descriptor.name == case.name
        ]
        cases.append(
            {"case": case.name, "input_case_sha256": case.input_case_sha256, "repeats": records}
        )
    return {
        "schema": wire.CONTROLLER_SCHEMA,
        "controller_pid": os.getpid(),
        "controller_run_nonce": nonce,
        "requested_repeats": repeats,
        "case_results": cases,
        "complete": True,
    }


def canonical_json_bytes(value):
    return json.dumps(value, **_J).encode()


def canonical_context_bytes(contexts):
    return canonical_json_bytes(
        [
            {
                "ordinal": slot.ordinal,
                "case": slot.descriptor.name,
                "repeat": slot.repeat_index,
                "lanes": [
                    {"role": lane.role, "nonce": lane.run_nonce, "pid": pid, "argv": list(argv)}
                    for lane, (pid, argv) in zip(slot.lanes(), execution, strict=True)
                ],
            }
            for slot, execution in contexts
        ]
    )
