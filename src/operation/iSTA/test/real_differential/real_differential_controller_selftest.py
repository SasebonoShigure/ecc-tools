#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# ***************************************************************************************
"""Fresh-process, public-contract, and transaction tests for Step 8."""

import copy
import dataclasses
import hashlib
import math
import os
import re
import secrets
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

import real_differential_contract as contract
import real_differential_model as model
import real_differential_wire_contract as wire
from real_differential_contract import (
    parse_strict_json_for_test,
    validate_controller_summary,
    validate_pair,
    validate_probe,
)
from reference_input_candidate_wire import decode_candidate_wire, encode_full_candidate_wire
from reference_input_model import UnsupportedReason

ensure = contract.ensure
_CANDIDATE_ROUNDTRIP_CASES = ("chain", "diamond", "supported_branches")
_N6_RECEIPT = (6, 18, 36, 55)
_FLOAT = re.compile(rb'(?<![0-9A-Za-z_."])-?(?:0|[1-9]\d*)\.\d+(?:[eE][+-]?\d+)?')
_SCHEMA_ACCEPT_KATS = (
    "full_keyword_bundle",
    "type_sensitive_enum_distinct",
    "boolean_subschema_positions",
)
_SCHEMA_REJECTION_KATS = (
    "bad_root_schema",
    "empty_root_id",
    "nested_schema",
    "nested_id",
    "title_shape",
    "defs_shape",
    "defs_child_shape",
    "properties_shape",
    "properties_child_shape",
    "type_empty",
    "type_unknown",
    "type_nonstring",
    "type_duplicate",
    "required_duplicate",
    "required_nonstring",
    "oneof_empty",
    "oneof_nonschema",
    "prefixitems_empty",
    "prefixitems_nonschema",
    "items_shape",
    "additional_shape",
    "pattern_shape",
    "pattern_invalid",
    "minimum_bool",
    "maximum_nonfinite",
    "minimum_gt_maximum",
    "minitems_bool",
    "maxitems_negative",
    "minitems_gt_maxitems",
    "const_nonjson",
    "const_nonfinite",
    "enum_empty",
    "enum_duplicate",
    "enum_nonfinite",
    "ref_shape",
    "root_unknown",
    "nested_unknown",
    "required_position",
    "properties_position",
    "pattern_position",
    "minimum_position",
    "prefixitems_position",
    "items_position",
    "additional_position",
    "minitems_position",
    "ref_missing_store",
    "ref_store_id_drift",
    "ref_fragment_missing",
    "ref_fragment_nonpointer",
    "ref_fragment_escape",
    "ref_fragment_drift",
    "ref_sibling_conjunction",
    "ref_cycle",
    "public_store_drift",
)
_LAUNCH_ZERO_KATS = ("double_flag", "public_repeats_drift", "duplicate_nonce")
_TRANSACTION_MODES = (
    "success",
    "invalid",
    "validate_two_temps",
    "lane-temp-symlink",
    "lane-temp-swap",
    "lane-temp-reopen",
    "lane-temp-inode",
    "link_oracle",
    "link",
    "link_reference",
    "unlink",
    "link-fsync",
    "unlink_temps",
    "validate_two_finals",
    "lane-final-symlink",
    "lane-final-swap",
    "lane-final-reopen",
    "lane-final-inode",
    "reload",
    "revalidate",
    "summary_last",
    "summary-validate",
    "summary-symlink",
    "summary-swap",
    "summary-link-call",
    "summary-link",
    "summary-unlink",
    "summary-fsync",
    "complete",
    "rollback-aggregate",
    "remove_probe_sta",
    "probe_sta_removed",
    "sta-missing",
    "sta-root-file",
    "sta-root-symlink",
    "sta-lane-extra",
    "sta-descendant-symlink",
    "sta-rmtree-error",
    "validate_lane_inventory",
    "sta-fsync-error",
    "sta-mode-0700",
    "sta-mode-0755",
    "sta-mode-invalid",
)


def _expect_failure(action, token):
    try:
        action()
    except Exception as exc:
        ensure(token in str(exc), f"diagnostic lacks {token}: {exc}")
        return str(exc)
    else:
        raise AssertionError(f"expected rejection containing {token}")


def _flip(value):
    return ("0" if value[0] != "0" else "1") + value[1:]


def _set(value, path, replacement):
    result = copy.deepcopy(value)
    target = result
    for part in path[:-1]:
        target = target[part]
    before = target[path[-1]]
    target[path[-1]] = replacement(before) if callable(replacement) else replacement
    ensure(result != value and target[path[-1]] != before, "mutation made no single-field change")
    return result


def _replace_equal(value, before, after):
    if value == before:
        return copy.deepcopy(after)
    if isinstance(value, dict):
        return {key: _replace_equal(item, before, after) for key, item in value.items()}
    if isinstance(value, list):
        return [_replace_equal(item, before, after) for item in value]
    return value


def _mutate_duplicate_json(_value, _models):
    return b'{"a":1,"a":2}'


def _mutate_trailing_json(_value, _models):
    return b'{"a":1}x'


def _nonfinite(value, token):
    match = _FLOAT.search(value)
    ensure(match is not None, "fresh probe JSON lacks a finite float token")
    return value[: match.start()] + token + value[match.end() :]


def _mutate_nan_json(value, _models):
    return _nonfinite(value, b"NaN")


def _mutate_pos_inf_json(value, _models):
    return _nonfinite(value, b"Infinity")


def _mutate_neg_inf_json(value, _models):
    return _nonfinite(value, b"-Infinity")


def _mutate_bad_sha(value, _models):
    return _set(value, ("source_wires", "pre_poison_raw_sha256"), _flip)


def _mutate_bad_pid(value, _models):
    return _set(value, ("process_id",), lambda item: item + 1)


def _mutate_bad_schema(value, _models):
    return _set(value, ("schema",), "invalid")


def _mutate_raw_drift(value, _models):
    result = copy.deepcopy(value)
    item = result["forbidden_projection"]["post_poison"]
    original_item = copy.deepcopy(item)
    raw = contract.raw_wire.decode_raw_wire(bytes.fromhex(item["raw_wire_hex"]))
    index = next(i for i, arc in enumerate(raw.arcs) if not arc.disabled and not arc.loop_disabled)
    arcs = list(raw.arcs)
    arcs[index] = dataclasses.replace(arcs[index], disabled=True)
    changed = dataclasses.replace(raw, arcs=tuple(arcs))
    payload = contract.raw_wire.encode_raw_wire(changed)
    ensure(
        contract.raw_wire.decode_raw_wire(payload) == changed
        and payload != bytes.fromhex(original_item["raw_wire_hex"]),
        "raw mutation round-trip differs",
    )
    item.update(raw_wire_hex=payload.hex(), raw_sha256=hashlib.sha256(payload).hexdigest())
    restored = copy.deepcopy(result)
    restored["forbidden_projection"]["post_poison"] = original_item
    ensure(restored == value, "raw mutation changed an undeclared record field")
    return result


def _mutate_hash_drift(value, _models):
    return _set(value, ("identity", "topology_fingerprint"), _flip)


def _reencode_candidate_mutation(value, transform, models):
    result = copy.deepcopy(value)
    source = result["source_wires"]
    original_source = copy.deepcopy(source)
    baseline = models[source["snapshot_candidate_sha256"]]
    changed = transform(baseline)
    payload = encode_full_candidate_wire(changed)
    decoded = decode_candidate_wire(payload)
    digest = hashlib.sha256(payload).hexdigest()
    ensure(
        decoded == changed
        and changed != baseline
        and payload != bytes.fromhex(original_source["snapshot_candidate_wire_hex"])
        and digest != source["snapshot_candidate_sha256"],
        "candidate mutation strict round-trip differs",
    )
    source.update(
        snapshot_candidate_wire_hex=payload.hex(),
        snapshot_candidate_sha256=digest,
    )
    restored = copy.deepcopy(result)
    for key in ("snapshot_candidate_wire_hex", "snapshot_candidate_sha256"):
        restored["source_wires"][key] = original_source[key]
    ensure(restored == value, "candidate mutation changed an undeclared record field")
    return result


def _mutate_unsupported(value, models):
    def transform(candidate):
        ensure(candidate.unsupported_reasons == (), "candidate unsupported baseline is nonempty")
        changed = dataclasses.replace(
            candidate, unsupported_reasons=(UnsupportedReason.PARASITIC_NET,)
        )
        ensure(
            dataclasses.replace(changed, unsupported_reasons=()) == candidate, "unsupported drift"
        )
        return changed

    return _reencode_candidate_mutation(value, transform, models)


def _mutate_scatter(value, _models):
    return _set(value, ("scatter", "writes_applied"), lambda item: item + 1)


def _mutate_released(value, _models):
    return _set(value, ("observation", "released", "state"), "ready")


def _mutate_stale_argv(value, _models):
    return _set(value, ("argv_receipt", "argv", 0), lambda item: item + "/.")


def _mutate_bad_arc_key(value, _models):
    timing = value["reference_timing"]
    original = next(arc["key"] for arc in timing["arcs"] if arc["key"]["kind"] == "cell")
    changed = original | {"owner_name": ""}
    result = _replace_equal(value, original, changed)
    ensure(result != value, "ArcKey mutation did not reach any reference")
    return result


def _selector_arcs(timing, selector):
    source = f"{selector['instance']}:{selector['source_pin']}"
    sink = f"{selector['instance']}:{selector['sink_pin']}"
    fields = ("owner_name", "library_source_port", "library_sink_port")
    return [
        arc
        for arc in timing["arcs"]
        if arc["key"]["source"]["full_name"] == source
        and arc["key"]["sink"]["full_name"] == sink
        and all(arc["key"][name] == selector[name] for name in fields)
    ]


def _mutate_near_tie(value, _models):
    result = copy.deepcopy(value)
    near = result["scenario"]["coverage"]["near_tie_descriptor"]
    descriptor = copy.deepcopy(near)
    timing = result["reference_timing"]
    arcs = [_selector_arcs(timing, near[name])[0] for name in ("row_first_arc", "row_second_arc")]
    paths = []
    for arc in arcs:
        node = next(item for item in timing["nodes"] if item["key"] == arc["key"]["source"])
        paths.append(
            next(
                item
                for item in node["paths"]
                if (
                    item["analysis"],
                    item["path_source"],
                    item["transition"],
                    item["clock_tag"],
                )
                == (near["analysis"], near["path_source"], near["transition"], near["clock_tag"])
            )
        )
    wrappers = [path["arrival"] for path in paths]
    ensure(
        all(item["kind"] == "finite" and item["negative_zero"] is False for item in wrappers),
        "near mutation baseline wrapper differs",
    )
    delays = [wire.wire_number(arc["graph_delays"][0][0][0], "near delay") for arc in arcs]
    other = wire.wire_number(wrappers[1], "near arrival") + delays[1]
    upper = near["upper_exclusive"]
    before = copy.deepcopy(wrappers[0])
    for direction in (1.0, -1.0):
        arrival = other + direction * 4.0 * upper - delays[0]
        edge = math.inf if direction > 0 else -math.inf
        for _ in range(4097):
            candidate = arrival + delays[0]
            if (
                math.isfinite(arrival)
                and math.isfinite(candidate)
                and abs(candidate - other) > upper
            ):
                wrappers[0]["value"] = arrival
                ensure(
                    wrappers[0] == before | {"value": arrival} and near == descriptor,
                    "near mutation changed finite wrapper metadata",
                )
                wire.validate_neutral_wire(timing, "near mutation timing")
                return result
            arrival = math.nextafter(arrival, edge)
    raise AssertionError("near mutation could not cross upper_exclusive")


def _mutate_prebuild(value, models):
    def transform(candidate):
        identity = candidate.source_identity
        ensure(identity.session_token > 0, "candidate prebuild baseline token is zero")
        changed = dataclasses.replace(
            candidate, source_identity=dataclasses.replace(identity, session_token=0)
        )
        ensure(
            dataclasses.replace(changed, source_identity=identity) == candidate, "prebuild drift"
        )
        return changed

    return _reencode_candidate_mutation(value, transform, models)


def _mutate_checkpoint_copy(value, _models):
    return _set(
        value,
        ("forbidden_projection", "projection_call_receipt", 1, "call_ordinal"),
        0,
    )


def _mutate_canary(value, _models):
    result = copy.deepcopy(value)
    result["reference"]["canary_timing"] = copy.deepcopy(result["reference"]["reference_timing"])
    return result


def _mutate_tolerance_bool(value, _models):
    return _set(value, ("absolute",), bool(1))


def _mutate_tolerance_nan(value, _models):
    return _set(value, ("absolute",), math.nan)


def _mutate_tolerance_pos_inf(value, _models):
    return _set(value, ("absolute",), math.inf)


def _mutate_tolerance_neg_inf(value, _models):
    return _set(value, ("absolute",), -math.inf)


def _mutate_tolerance_negative(value, _models):
    return _set(value, ("absolute",), -1e-9)


def _mutate_tolerance_signed_zero(value, _models):
    return _set(value, ("absolute",), -0.0)


def _mutate_tolerance_drift(value, _models):
    return _set(value, ("absolute",), 2e-9)


def _mutate_summary_pid(value, _models):
    return _set(value, ("controller_pid",), lambda item: item + 1)


def _mutate_summary_nonce(value, _models):
    return _set(value, ("controller_run_nonce",), _flip)


def _mutate_summary_repeat_order(value, _models):
    result = copy.deepcopy(value)
    repeats = result["case_results"][0]["repeats"]
    repeats[0], repeats[1] = repeats[1], repeats[0]
    return result


def _mutate_summary_embedded_schema(value, _models):
    return _set(value, ("case_results", 0, "repeats", 0, "oracle", "schema"), "invalid")


PUBLIC_NEGATIVE_MATRIX = (
    (
        "duplicate_json",
        "loader_bytes",
        parse_strict_json_for_test,
        _mutate_duplicate_json,
        "duplicate-key",
    ),
    (
        "trailing_json",
        "loader_bytes",
        parse_strict_json_for_test,
        _mutate_trailing_json,
        "trailing-data",
    ),
    (
        "nan_json",
        "fresh_probe_json_bytes",
        parse_strict_json_for_test,
        _mutate_nan_json,
        "non-finite",
    ),
    (
        "pos_inf_json",
        "fresh_probe_json_bytes",
        parse_strict_json_for_test,
        _mutate_pos_inf_json,
        "non-finite",
    ),
    (
        "neg_inf_json",
        "fresh_probe_json_bytes",
        parse_strict_json_for_test,
        _mutate_neg_inf_json,
        "non-finite",
    ),
    ("bad_sha", "reference_probe", validate_probe, _mutate_bad_sha, "bad-sha"),
    ("bad_pid", "reference_probe", validate_probe, _mutate_bad_pid, "bad-pid"),
    ("bad_schema", "reference_probe", validate_probe, _mutate_bad_schema, "bad-schema"),
    ("raw_drift", "reference_probe", validate_probe, _mutate_raw_drift, "raw-provenance"),
    ("hash_drift", "reference_probe", validate_probe, _mutate_hash_drift, "hash-drift"),
    ("unsupported", "reference_probe", validate_probe, _mutate_unsupported, "unsupported"),
    ("scatter", "reference_probe", validate_probe, _mutate_scatter, "scatter"),
    ("released", "reference_probe", validate_probe, _mutate_released, "released"),
    ("stale_argv", "reference_probe", validate_probe, _mutate_stale_argv, "stale"),
    ("bad_arc_key", "extended_reference_probe", validate_probe, _mutate_bad_arc_key, "ArcKey"),
    (
        "near_tie",
        "extended_reference_probe",
        validate_probe,
        _mutate_near_tie,
        "near-tie-actual-numeric",
    ),
    ("prebuild", "reference_probe", validate_probe, _mutate_prebuild, "prebuild"),
    (
        "checkpoint_copy",
        "reference_probe",
        validate_probe,
        _mutate_checkpoint_copy,
        "checkpoint-copy",
    ),
    ("canary", "reference_pair", validate_pair, _mutate_canary, "canary"),
    (
        "tolerance_bool",
        "reference_pair+tolerances",
        validate_pair,
        _mutate_tolerance_bool,
        "tolerance",
    ),
    (
        "tolerance_nan",
        "reference_pair+tolerances",
        validate_pair,
        _mutate_tolerance_nan,
        "tolerance",
    ),
    (
        "tolerance_pos_inf",
        "reference_pair+tolerances",
        validate_pair,
        _mutate_tolerance_pos_inf,
        "tolerance",
    ),
    (
        "tolerance_neg_inf",
        "reference_pair+tolerances",
        validate_pair,
        _mutate_tolerance_neg_inf,
        "tolerance",
    ),
    (
        "tolerance_negative",
        "reference_pair+tolerances",
        validate_pair,
        _mutate_tolerance_negative,
        "tolerance",
    ),
    (
        "tolerance_signed_zero",
        "reference_pair+tolerances",
        validate_pair,
        _mutate_tolerance_signed_zero,
        "tolerance",
    ),
    (
        "tolerance_drift",
        "reference_pair+tolerances",
        validate_pair,
        _mutate_tolerance_drift,
        "tolerance",
    ),
    (
        "summary_pid",
        "controller_summary",
        validate_controller_summary,
        _mutate_summary_pid,
        "summary-pid",
    ),
    (
        "summary_nonce",
        "controller_summary",
        validate_controller_summary,
        _mutate_summary_nonce,
        "summary-nonce",
    ),
    (
        "summary_repeat_order",
        "controller_summary",
        validate_controller_summary,
        _mutate_summary_repeat_order,
        "summary-repeat-order",
    ),
    (
        "summary_embedded_schema",
        "controller_summary",
        validate_controller_summary,
        _mutate_summary_embedded_schema,
        "summary-embedded-schema",
    ),
)


def _validate_candidate_roundtrips(records, repeats):
    ensure(repeats == 3, "public matrix repeats differs")
    payloads, models = set(), {}
    for case_index, name in enumerate(_CANDIDATE_ROUNDTRIP_CASES):
        record = records[case_index * repeats][1]
        source = record["source_wires"]
        payload = bytes.fromhex(source["snapshot_candidate_wire_hex"])
        digest = hashlib.sha256(payload).hexdigest()
        ensure(digest == source["snapshot_candidate_sha256"], f"{name} candidate SHA differs")
        decoded = decode_candidate_wire(payload)
        ensure(encode_full_candidate_wire(decoded) == payload, f"{name} full codec differs")
        models[digest] = decoded
        payloads.add(payload)
    ensure(len(payloads) == 3 and len(models) == 3, "candidate payloads are not case-distinct")
    return models


def _invoke(entry, kind, value, arguments):
    if kind in {"reference_probe", "extended_reference_probe"}:
        entry(value, *arguments)
    elif kind in {"reference_pair", "reference_pair+tolerances"}:
        entry(
            value["oracle"],
            value["reference"],
            value["bundle"],
            value["absolute"],
            value["relative"],
        )
    elif kind == "controller_summary":
        bundle, repeats, pid, nonce, executables, contexts = arguments
        entry(value, bundle, repeats, pid, nonce, executables, contexts)
    else:
        entry(value)


def _target(row, summary, validation, records):
    _name, kind, _entry, _mutation, _token = row
    bundle, repeats, _pid, _nonce, executables, contexts = validation
    if kind == "loader_bytes":
        return b'{"a":1}', ()
    if kind == "fresh_probe_json_bytes":
        return contract.canonical_json_bytes(records[0][1]) + b"\n", ()
    index = 2 * repeats if kind == "extended_reference_probe" else 0
    if kind in {"reference_probe", "extended_reference_probe"}:
        slot, execution = contexts[index]
        lane = slot.reference
        child_pid, argv = execution[1]
        arguments = (
            slot.descriptor,
            "reference",
            slot.repeat_index,
            lane.run_nonce,
            child_pid,
            tuple(argv),
            executables[1],
            lane.directory,
            lane.temporary_result,
        )
        return records[index][1], arguments
    if kind in {"reference_pair", "reference_pair+tolerances"}:
        return {
            "oracle": records[index][0],
            "reference": records[index][1],
            "bundle": bundle,
            "absolute": bundle.absolute_tolerance,
            "relative": bundle.relative_tolerance,
        }, ()
    return summary, validation


def _file_state(path):
    meta = path.lstat()
    return (
        meta.st_dev,
        meta.st_ino,
        meta.st_uid,
        stat.S_IMODE(meta.st_mode),
        meta.st_nlink,
        meta.st_size,
        hashlib.sha256(path.read_bytes()).hexdigest(),
    )


def _layout_state(slots):
    lanes = tuple(lane for slot in slots for lane in slot.lanes())
    temps = tuple(
        (str(lane.temporary_result), _file_state(lane.temporary_result)) for lane in lanes
    )
    root = slots[0].directory.parent
    directories = (root, *(slot.directory for slot in slots), *(lane.directory for lane in lanes))
    entries = tuple(
        (str(path), tuple(sorted(item.name for item in path.iterdir()))) for path in directories
    )
    finals = tuple(lane.final_result for lane in lanes)
    nonce = root.name.rsplit("-", 1)[1]
    finals += (
        root / f".real-differential-controller-{nonce}.tmp",
        root / "real-differential-controller-result.json",
    )
    return temps, entries, tuple(path.exists() or path.is_symlink() for path in finals)


def _handle_state(handles):
    return tuple(
        (id(item), item.pid, tuple(item.args), item.returncode, item.poll(), item.wait(timeout=0))
        for item in handles
    )


def run_public_negative_matrix(
    summary, validation, context_bytes, records, records_bytes, slots, handles
):
    bundle, repeats, pid, nonce, executables, contexts = validation
    ensure(repeats == 3 and len(records) == 9 and len(handles) == 18, "public matrix N3 differs")
    validate_controller_summary(summary, bundle, repeats, pid, nonce, executables, contexts)
    models = _validate_candidate_roundtrips(records, repeats)
    ensure(context_bytes == contract.canonical_context_bytes(contexts), "context bytes differ")
    ensure(records_bytes == contract.canonical_json_bytes(records), "record bytes differ")
    frozen = (
        copy.deepcopy(summary),
        copy.deepcopy(records),
        copy.deepcopy(contexts),
        context_bytes,
        records_bytes,
        _layout_state(slots),
        _handle_state(handles),
    )
    for name, kind, entry, mutation, token in PUBLIC_NEGATIVE_MATRIX:
        baseline, arguments = _target(
            (name, kind, entry, mutation, token), summary, validation, records
        )
        _invoke(entry, kind, baseline, arguments)
        before = copy.deepcopy(baseline)
        changed = mutation(baseline, models)
        ensure(baseline == before and changed != baseline, f"{name} mutation isolation differs")
        _expect_failure(
            lambda entry=entry, kind=kind, changed=changed, arguments=arguments: _invoke(
                entry, kind, changed, arguments
            ),
            token,
        )
        current = (
            summary,
            records,
            contexts,
            contract.canonical_context_bytes(contexts),
            contract.canonical_json_bytes(records),
            _layout_state(slots),
            _handle_state(handles),
        )
        ensure(current == frozen, f"{name} changed frozen run evidence")


_STUB = r"""#!/usr/bin/env python3
import os
import signal
import sys
import time
from pathlib import Path
mode, pid, output, permissions = sys.argv[1:]
Path(pid).write_text(str(os.getpid()), encoding="utf-8")
if output != "-":
    sta = Path("sta")
    timing = sta / "timing_reporter"
    nested = timing / "nested"
    sta.mkdir(mode=0o700)
    timing.mkdir(mode=0o700)
    nested.mkdir(mode=0o700)
    for directory in (sta, timing, nested):
        directory.chmod(0o700)
    reports = (
        (sta / "sta.log", b"sta log\n"),
        (timing / "qor_summary.rpt", b"qor summary\n"),
        (nested / "timing_paths.json", b"{}\n"),
    )
    for report, raw in reports:
        report.write_bytes(raw)
        report.chmod(0o600)
    requested_mode = int(permissions, 8)
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, requested_mode)
    os.fchmod(fd, requested_mode)
    os.write(fd, b"{}\n")
    os.fsync(fd)
    os.close(fd)
if mode in {"exit", "write"}:
    raise SystemExit(0)
if mode == "short":
    time.sleep(0.05)
    raise SystemExit(0)
if mode == "flood":
    for _ in range(64):
        os.write(1, b"x" * 4096)
        os.write(2, b"x" * 4096)
    raise SystemExit(0)
if mode == "ignore":
    signal.signal(signal.SIGTERM, lambda *_: Path(pid + ".term").write_text("term"))
while True:
    time.sleep(0.05)
"""


def _command(stub, mode, pid, output="-", permissions=0o600):
    return sys.executable, str(stub), mode, str(pid), str(output), oct(permissions)


def _dead(processes):
    ensure(
        all(
            process.poll() is not None and not Path(f"/proc/{process.pid}").exists()
            for process in processes
        ),
        "self-test left a live child",
    )


def _process_tests(api, root, stub):
    seen = []
    first = _command(stub, "hang", root / "launch.pid")
    _expect_failure(
        lambda: api.spawn_and_drain(((first, root), ((str(root / "missing"),), root)), 0.2, seen),
        "pair launch",
    )
    ensure(len(seen) == 1 and not Path(f"/proc/{seen[0]}").exists(), "launch cleanup differs")
    seen.clear()
    commands = tuple(
        (_command(stub, mode, root / f"{mode}.pid"), root) for mode in ("short", "ignore")
    )
    _expect_failure(lambda: api.spawn_and_drain(commands, 0.15, seen), "pair launch")
    ensure(
        len(seen) == 2
        and all(not Path(f"/proc/{pid}").exists() for pid in seen)
        and (root / "ignore.pid.term").is_file(),
        "timeout cleanup differs",
    )
    commands = tuple(
        (_command(stub, "flood", root / f"flood-{index}.pid"), root) for index in range(2)
    )
    completed = api.spawn_and_drain(commands, 5.0)
    ensure(
        all(
            item[0].returncode == 0 and min(len(item[1]), len(item[2])) >= 262144
            for item in completed
        ),
        "dual-pipe drain differs",
    )
    _dead(tuple(item[0] for item in completed))


def _launch_zero_tests(api, root, bundle):
    controller = Path(__file__).with_name("real_differential_controller.py")
    sentinels = root / "oracle-launched", root / "reference-launched"
    probes = root / "no_launch_oracle.py", root / "no_launch_reference.py"
    for probe, sentinel in zip(probes, sentinels, strict=True):
        probe.write_text(
            "#!/usr/bin/env python3\n"
            "from pathlib import Path\n"
            f"Path({str(sentinel)!r}).write_text('launched', encoding='utf-8')\n",
            encoding="utf-8",
        )
        probe.chmod(0o700)
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    completed = []

    def reject(name, arguments, token):
        result = subprocess.run(
            arguments,
            cwd=root,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            check=False,
            timeout=10.0,
            env=environment,
        )
        diagnostic = result.stderr.decode("utf-8", errors="backslashreplace")
        ensure(result.returncode != 0 and token in diagnostic, f"{name} CLI rejection differs")
        ensure(not any(path.exists() for path in sentinels), f"{name} launched a probe")
        completed.append(name)

    reject(
        "double_flag",
        (
            sys.executable,
            str(controller),
            "--self-test",
            "--public-negative-self-test",
        ),
        "not allowed with argument",
    )
    reject(
        "public_repeats_drift",
        (
            sys.executable,
            str(controller),
            "--oracle-probe",
            str(probes[0]),
            "--reference-probe",
            str(probes[1]),
            "--run-root",
            str(root / "public-drift"),
            "--public-negative-self-test",
            "--repeats",
            "4",
        ),
        "public flag requires repeats 3",
    )
    with patch.object(secrets, "token_hex", return_value="a" * 32):
        _expect_failure(
            lambda: api.prepare_layout(root / "duplicate-nonce", bundle, 3),
            "nonce collision",
        )
    ensure(not any(path.exists() for path in sentinels), "duplicate nonce launched a probe")
    completed.append("duplicate_nonce")
    ensure(tuple(completed) == _LAUNCH_ZERO_KATS, "launch-zero KAT inventory differs")


def _schema_baseline_tests():
    identifier = "urn:ista:step8:selftest"
    external_id = f"{identifier}:external"
    external = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": external_id,
        "type": "string",
        "pattern": "^z$",
    }
    leaf = {"type": "integer", "minimum": 0, "maximum": 2, "const": 1, "enum": [1]}
    schema = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": identifier,
        "title": "selftest",
        "$defs": {"leaf": leaf, "unused": {"type": ["null", "string"]}},
        "type": "object",
        "required": ["x", "a", "extras", "many", "external"],
        "properties": {
            "x": {"$ref": "#/$defs/leaf", "oneOf": [{"const": 1}], "minimum": 0},
            "a": {
                "type": "array",
                "prefixItems": [{"type": "string", "pattern": "^x$"}],
                "items": False,
                "minItems": 1,
                "maxItems": 1,
            },
            "extras": {"type": "object", "additionalProperties": {"type": "integer"}},
            "many": {"type": "array", "items": {"type": "string"}},
            "external": {"$ref": external_id},
        },
        "additionalProperties": False,
    }
    instance = {
        "x": 1,
        "a": ["x"],
        "extras": {"answer": 1},
        "many": ["a", "b"],
        "external": "z",
    }
    store = {identifier: schema, external_id: external}
    wire.validate_schema_for_test(schema, instance, store)
    malformed = (
        (("$schema",), "urn:not-draft"),
        (("$id",), ""),
        (("title",), 7),
        (("$defs",), []),
        (("properties",), []),
        (("type",), ["object", "object"]),
        (("required",), ["x", "x"]),
        (("properties", "x", "oneOf"), []),
        (("properties", "a", "prefixItems"), []),
        (("properties", "many", "items"), 7),
        (("properties", "extras", "additionalProperties"), 7),
        (("properties", "a", "prefixItems", 0, "pattern"), 7),
        (("$defs", "leaf", "minimum"), True),
        (("$defs", "leaf", "maximum"), math.inf),
        (("properties", "a", "minItems"), False),
        (("properties", "a", "maxItems"), -1),
        (("$defs", "leaf", "const"), {1: "bad"}),
        (("$defs", "leaf", "enum"), []),
        (("properties", "external", "$ref"), 7),
    )
    for path, replacement in malformed:
        poisoned = _set(schema, path, replacement)
        poisoned_id = poisoned.get("$id")
        poisoned_store = {
            poisoned_id if isinstance(poisoned_id, str) else identifier: poisoned,
            external_id: external,
        }
        _expect_failure(
            lambda poisoned=poisoned, poisoned_store=poisoned_store: wire.validate_schema_for_test(
                poisoned, instance, poisoned_store
            ),
            "schema-definition",
        )
    bad = (
        (schema | {"future": 1}, "schema-definition"),
        (_set(schema, ("$defs", "unused"), {"future": 1}), "schema-definition"),
        (_set(schema, ("properties", "x", "minimum"), 2), "outside schema bounds"),
        (_set(schema, ("properties", "x", "$ref"), "urn:missing"), "foreign or missing $ref"),
        (
            _set(schema, ("properties", "x", "$ref"), "#/$defs/missing"),
            "$ref pointer does not exist",
        ),
        (_set(schema, ("properties", "x", "$ref"), "#bad"), "non-pointer fragment"),
        (_set(schema, ("properties", "x", "$ref"), "#/$defs/~2bad"), "illegal escape"),
    )
    for poisoned, token in bad:
        _expect_failure(
            lambda poisoned=poisoned: wire.validate_schema_for_test(
                poisoned, instance, {identifier: poisoned, external_id: external}
            ),
            token,
        )
    cyclic = copy.deepcopy(schema)
    cyclic["$defs"]["cycle"] = {"$ref": "#/$defs/cycle"}
    _expect_failure(
        lambda: wire.validate_schema_for_test(
            cyclic, instance, {identifier: cyclic, external_id: external}
        ),
        "cyclic $ref",
    )


def _schema_tests():
    _schema_baseline_tests()
    draft = "https://json-schema.org/draft/2020-12/schema"
    identifier = "urn:ista:step8:hardening"
    external_id = f"{identifier}:external"
    external = {"$schema": draft, "$id": external_id, "type": "string", "pattern": "^z$"}
    leaf = {
        "type": "integer",
        "minimum": 0,
        "maximum": 2,
        "const": 1,
        "enum": [1, 1.0],
    }
    schema = {
        "$schema": draft,
        "$id": identifier,
        "title": "hardening",
        "$defs": {"leaf": leaf, "unused": {"type": ["null", "string"]}},
        "type": "object",
        "required": ["x", "a", "extras", "many", "external"],
        "properties": {
            "x": {"$ref": "#/$defs/leaf", "oneOf": [{"const": 1}], "minimum": 0},
            "a": {
                "type": "array",
                "prefixItems": [{"type": "string", "pattern": "^x$"}],
                "items": False,
                "minItems": 1,
                "maxItems": 1,
            },
            "extras": {"type": "object", "additionalProperties": {"type": "integer"}},
            "many": {"type": "array", "items": {"type": "string"}},
            "external": {"$ref": external_id},
        },
        "additionalProperties": False,
    }
    instance = {
        "x": 1,
        "a": ["x"],
        "extras": {"answer": 1},
        "many": ["a", "b"],
        "external": "z",
    }
    accepted = []
    wire.validate_schema_for_test(schema, instance, {identifier: schema, external_id: external})
    accepted.append("full_keyword_bundle")
    enum_id = f"{identifier}:enum"
    enum_schema = {"$schema": draft, "$id": enum_id, "type": "number", "enum": [1, 1.0]}
    for value in (1, 1.0):
        wire.validate_schema_for_test(enum_schema, value, {enum_id: enum_schema})
    accepted.append("type_sensitive_enum_distinct")
    array_id = f"{identifier}:array"
    array_schema = {
        "$schema": draft,
        "$id": array_id,
        "type": "array",
        "prefixItems": [{"type": "string"}],
        "items": False,
    }
    object_id = f"{identifier}:object"
    object_schema = {
        "$schema": draft,
        "$id": object_id,
        "type": "object",
        "properties": {},
        "additionalProperties": False,
    }
    wire.validate_schema_for_test(array_schema, ["x"], {array_id: array_schema})
    wire.validate_schema_for_test(object_schema, {}, {object_id: object_schema})
    accepted.append("boolean_subschema_positions")
    ensure(tuple(accepted) == _SCHEMA_ACCEPT_KATS, "schema accept KAT inventory differs")

    cases = []

    def add(name, document, token="schema-definition", *, value=None, store=None):
        actual_value = instance if value is None else value
        actual_store = {identifier: document, external_id: external} if store is None else store
        cases.append((name, document, actual_value, actual_store, token))

    simple = (
        ("bad_root_schema", ("$schema",), "urn:not-draft"),
        ("empty_root_id", ("$id",), ""),
        ("nested_schema", ("$defs", "leaf"), leaf | {"$schema": draft}),
        ("nested_id", ("$defs", "leaf"), leaf | {"$id": "urn:nested"}),
        ("title_shape", ("title",), 7),
        ("defs_shape", ("$defs",), []),
        ("defs_child_shape", ("$defs", "unused"), 7),
        ("properties_shape", ("properties",), []),
        ("properties_child_shape", ("properties", "extras"), 7),
        ("type_empty", ("$defs", "unused", "type"), []),
        ("type_unknown", ("$defs", "unused", "type"), "future"),
        ("type_nonstring", ("$defs", "unused", "type"), 7),
        ("type_duplicate", ("$defs", "unused", "type"), ["string", "string"]),
        ("required_duplicate", ("required",), ["x", "x"]),
        ("required_nonstring", ("required",), ["x", 7]),
        ("oneof_empty", ("properties", "x", "oneOf"), []),
        ("oneof_nonschema", ("properties", "x", "oneOf"), [7]),
        ("prefixitems_empty", ("properties", "a", "prefixItems"), []),
        ("prefixitems_nonschema", ("properties", "a", "prefixItems"), [7]),
        ("items_shape", ("properties", "many", "items"), 7),
        ("additional_shape", ("properties", "extras", "additionalProperties"), 7),
        ("pattern_shape", ("properties", "a", "prefixItems", 0, "pattern"), 7),
        ("pattern_invalid", ("properties", "a", "prefixItems", 0, "pattern"), "["),
        ("minimum_bool", ("$defs", "leaf", "minimum"), True),
        ("maximum_nonfinite", ("$defs", "leaf", "maximum"), math.inf),
        ("minimum_gt_maximum", ("$defs", "leaf", "minimum"), 3),
        ("minitems_bool", ("properties", "a", "minItems"), False),
        ("maxitems_negative", ("properties", "a", "maxItems"), -1),
        ("minitems_gt_maxitems", ("properties", "a", "minItems"), 2),
        ("const_nonjson", ("$defs", "leaf", "const"), {1: "bad"}),
        ("const_nonfinite", ("$defs", "leaf", "const"), math.nan),
        ("enum_empty", ("$defs", "leaf", "enum"), []),
        ("enum_duplicate", ("$defs", "leaf", "enum"), ["same", "same"]),
        ("enum_nonfinite", ("$defs", "leaf", "enum"), [math.inf]),
        ("ref_shape", ("properties", "external", "$ref"), 7),
    )
    for name, path, replacement in simple:
        add(name, _set(schema, path, replacement))
    add("root_unknown", schema | {"future": 1})
    add("nested_unknown", _set(schema, ("$defs", "unused"), {"future": 1}))
    positions = (
        ("required_position", {"type": "string", "required": []}),
        ("properties_position", {"type": "array", "properties": {}}),
        ("pattern_position", {"type": "integer", "pattern": "^x$"}),
        ("minimum_position", {"type": "string", "minimum": 0}),
        ("prefixitems_position", {"type": "object", "prefixItems": [{}]}),
        ("items_position", {"type": "object", "items": {}}),
        ("additional_position", {"type": "array", "additionalProperties": {}}),
        ("minitems_position", {"type": "object", "minItems": 0}),
    )
    for name, replacement in positions:
        add(name, _set(schema, ("$defs", "unused"), replacement))
    add(
        "ref_missing_store",
        _set(schema, ("properties", "x", "$ref"), "urn:missing"),
        "foreign or missing $ref",
    )
    external_drift = external | {"$id": f"{external_id}:drift"}
    add(
        "ref_store_id_drift",
        schema,
        store={identifier: schema, external_id: external_drift},
    )
    add(
        "ref_fragment_missing",
        _set(schema, ("properties", "x", "$ref"), "#/$defs/missing"),
        "$ref pointer does not exist",
    )
    add(
        "ref_fragment_nonpointer",
        _set(schema, ("properties", "x", "$ref"), "#bad"),
        "non-pointer fragment",
    )
    add(
        "ref_fragment_escape",
        _set(schema, ("properties", "x", "$ref"), "#/$defs/~2bad"),
        "illegal escape",
    )
    add(
        "ref_fragment_drift",
        _set(schema, ("properties", "x", "$ref"), "#/$defs/unused"),
        "invalid JSON type",
    )
    add(
        "ref_sibling_conjunction",
        schema,
        "schema constant",
        value=_set(instance, ("x",), 2),
    )
    cyclic = copy.deepcopy(schema)
    cyclic["$defs"]["cycle"] = {"$ref": "#/$defs/cycle"}
    add("ref_cycle", cyclic, "cyclic $ref")
    store_drift = _set(schema, ("title",), "store drift")
    add(
        "public_store_drift",
        schema,
        store={identifier: store_drift, external_id: external},
    )
    ensure(tuple(item[0] for item in cases) == _SCHEMA_REJECTION_KATS, "schema KAT order")
    for name, document, value, store, token in cases:
        _expect_failure(
            lambda document=document, value=value, store=store: wire.validate_schema_for_test(
                document, value, store
            ),
            token,
        )
        ensure(name in _SCHEMA_REJECTION_KATS, "schema KAT name drift")


def _build_run(api, root, bundle, stub, invalid):
    layout = api.prepare_layout(root, bundle, 6)
    ensure(
        (6, len(layout[2]), sum(len(slot.lanes()) for slot in layout[2]), len(layout[6]))
        == _N6_RECEIPT,
        "N6 layout receipt differs",
    )
    results = []
    for slot, lane_fds in zip(layout[2], layout[5], strict=True):
        commands = []
        for lane in slot.lanes():
            permissions = (
                0o644 if invalid and slot.ordinal == 0 and lane.role == "reference" else 0o600
            )
            commands.append(
                (
                    _command(
                        stub,
                        "write",
                        root / f".probe-{slot.ordinal:03d}-{lane.role}.pid",
                        lane.temporary_result,
                        permissions,
                    ),
                    lane.directory,
                )
            )
        completed = api.spawn_and_drain(tuple(commands), 5.0)
        temps, execution = [], []
        for lane, lane_fd, item in zip(slot.lanes(), lane_fds, completed, strict=True):
            process = item[0]
            descriptor = os.open(
                lane.temporary_result.name, os.O_RDWR | os.O_NOFOLLOW, dir_fd=lane_fd
            )
            temps.append((lane, lane_fd, descriptor, contract.file_identity(os.fstat(descriptor))))
            execution.append((process.pid, tuple(process.args)))
        results.append(
            (
                slot,
                {},
                {},
                tuple(execution),
                tuple(item[0] for item in completed),
                tuple(temps),
            )
        )
    return layout, tuple(results), tuple((item[0], item[3]) for item in results)


def _transaction_test(api, root, bundle, stub, mode):
    layout, results, contexts = _build_run(api, root, bundle, stub, mode == "invalid")
    executables, summary = (Path("/usr/bin/true"), Path("/usr/bin/false")), {}
    original_link, original_fsync, original_unlink, original_open, original_rmtree = (
        os.link,
        os.fsync,
        os.unlink,
        os.open,
        shutil.rmtree,
    )
    links, synced, validations, unlink_poisoned, summary_opens = 0, [], 0, False, []
    handles = tuple(process for result in results for process in result[4])
    handle_state = _handle_state(handles)
    temps = tuple(temp for result in results for temp in result[5])
    lanes = tuple(lane for slot in layout[2] for lane in slot.lanes())
    lane_fds = tuple(descriptor for pair in layout[5] for descriptor in pair)
    sta_paths = tuple(lane.directory / "sta" for lane in lanes)
    ensure(len(sta_paths) == len(lane_fds) == 36, "transaction sta cardinality differs")
    for sta_path in sta_paths:
        sta_info = sta_path.lstat()
        reports = (
            sta_path / "sta.log",
            sta_path / "timing_reporter" / "qor_summary.rpt",
            sta_path / "timing_reporter" / "nested" / "timing_paths.json",
        )
        ensure(
            stat.S_ISDIR(sta_info.st_mode)
            and stat.S_IMODE(sta_info.st_mode) == 0o700
            and sta_info.st_uid == os.geteuid(),
            "transaction sta root differs",
        )
        for report in reports:
            report_info = report.lstat()
            ensure(
                stat.S_ISREG(report_info.st_mode)
                and stat.S_IMODE(report_info.st_mode) == 0o600
                and report_info.st_nlink == 1,
                "transaction sta report differs",
            )
    external_dir = root / "external-sta-target"
    external_dir.mkdir(mode=0o700)
    external_dir.chmod(0o700)
    sentinel = external_dir / "sentinel.bin"
    sentinel.write_bytes(b"external sentinel\n")
    sentinel.chmod(0o600)
    sentinel_state = _file_state(sentinel)
    survivor_states = tuple(
        (path / "sta.log", _file_state(path / "sta.log")) for path in sta_paths[:-1]
    )
    summary_temp_name = f".real-differential-controller-{layout[1]}.tmp"
    summary_final_name = "real-differential-controller-result.json"
    rollback_targets = tuple(
        (lane_fd, lane.final_result.name) for lane, lane_fd, _file_fd, _saved in temps
    ) + ((layout[3], summary_temp_name), (layout[3], summary_final_name))
    rollback_trace, drift_cleanup = [], []
    rollback_started = False
    rmtree_attempts, sta_removals = [], []
    sta_fsync_poisoned = False
    successful = mode in {
        "success",
        "sta-descendant-symlink",
        "sta-mode-0700",
        "sta-mode-0755",
    }

    poison_sta = sta_paths[-1]
    if mode == "sta-mode-0755":
        for sta_path in sta_paths:
            sta_path.chmod(0o755)
    elif mode == "sta-mode-invalid":
        poison_sta.chmod(0o711)
    elif mode == "sta-missing":
        original_rmtree(poison_sta)
    elif mode == "sta-root-file":
        original_rmtree(poison_sta)
        poison_sta.write_bytes(b"not a directory\n")
        poison_sta.chmod(0o755)
    elif mode == "sta-root-symlink":
        original_rmtree(poison_sta)
        poison_sta.symlink_to(external_dir, target_is_directory=True)
    elif mode == "sta-lane-extra":
        extra = lanes[-1].directory / ".sta-lane-extra"
        extra.write_bytes(b"extra\n")
        extra.chmod(0o600)
    elif mode == "sta-descendant-symlink":
        nested = sta_paths[0] / "timing_reporter" / "nested"
        original_rmtree(nested)
        nested.symlink_to(external_dir, target_is_directory=True)

    def rmtree(path, *args, **kwargs):
        rmtree_attempts.append((str(path), kwargs.get("dir_fd")))
        ensure(
            str(path) == "sta" and kwargs.get("dir_fd") in lane_fds,
            "transaction sta rmtree call differs",
        )
        if mode == "sta-rmtree-error" and len(rmtree_attempts) == 2:
            raise OSError("sta rmtree poison")
        result = original_rmtree(path, *args, **kwargs)
        sta_removals.append(kwargs.get("dir_fd"))
        return result

    def poison_lane(final, variant):
        lane, lane_fd, held_fd, _saved = temps[0]
        name = lane.final_result.name if final else lane.temporary_result.name
        if variant == "symlink":
            original_unlink(name, dir_fd=lane_fd)
            os.symlink("missing", name, dir_fd=lane_fd)
            return
        if variant == "swap":
            original_unlink(name, dir_fd=lane_fd)
            replacement = original_open(
                name,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                0o600,
                dir_fd=lane_fd,
            )
            try:
                os.write(replacement, b"{}\n")
                original_fsync(replacement)
            finally:
                os.close(replacement)
            return
        drift_name = f".lane-{variant}-drift"
        if variant == "reopen":
            replacement = original_open(
                drift_name,
                os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                0o600,
                dir_fd=lane_fd,
            )
            try:
                os.write(replacement, b"{}\n")
                original_fsync(replacement)
                os.dup2(replacement, held_fd)
            finally:
                os.close(replacement)
                original_unlink(drift_name, dir_fd=lane_fd)
            return
        ensure(variant == "inode", "unknown lane drift variant")
        original_link(
            name,
            drift_name,
            src_dir_fd=lane_fd,
            dst_dir_fd=lane_fd,
            follow_symlinks=False,
        )
        drift_cleanup.append((lane_fd, drift_name))

    def link(*args, **kwargs):
        nonlocal links
        links += 1
        if mode in {"link", "unlink", "link-fsync"} and links == 2:
            raise OSError("second-link poison")
        if mode == "summary-link-call" and links == 37:
            raise OSError("summary-link poison")
        return original_link(*args, **kwargs)

    def fsync(descriptor):
        nonlocal sta_fsync_poisoned
        synced.append(descriptor)
        if (
            mode == "sta-fsync-error"
            and not sta_fsync_poisoned
            and descriptor == lane_fds[0]
            and sta_removals == [descriptor]
        ):
            sta_fsync_poisoned = True
            raise OSError("sta fsync poison")
        aggregate = mode == "rollback-aggregate" and rollback_started
        if aggregate and descriptor in {layout[6][0], layout[3]}:
            raise OSError("aggregate fsync poison")
        if mode in {"link-fsync", "summary-fsync"} and descriptor == layout[3]:
            raise OSError("layout fsync poison")
        return original_fsync(descriptor)

    def unlink(path, *args, **kwargs):
        nonlocal rollback_started, unlink_poisoned
        key = kwargs.get("dir_fd"), str(path)
        if key == rollback_targets[0]:
            rollback_started = True
        if rollback_started and key in rollback_targets:
            rollback_trace.append(key)
        aggregate = mode == "rollback-aggregate" and rollback_started
        if aggregate and key in {rollback_targets[0], rollback_targets[-1]}:
            raise OSError("aggregate unlink poison")
        lane_target = mode == "unlink" and str(path).endswith("oracle-result.json")
        summary_target = mode == "summary-unlink" and str(path).startswith(
            ".real-differential-controller-"
        )
        if not unlink_poisoned and (lane_target or summary_target):
            unlink_poisoned = True
            raise OSError("unlink poison")
        return original_unlink(path, *args, **kwargs)

    def open_file(path, flags, *args, **kwargs):
        if flags & os.O_RDWR and flags & os.O_CREAT:
            summary_opens.append((path, flags))
        return original_open(path, flags, *args, **kwargs)

    def validate_summary(*_args):
        nonlocal validations
        validations += 1
        if mode == "summary-validate" and validations == 2:
            raise AssertionError("summary validation poison")

    def phase(name):
        if mode == "validate_lane_inventory" and name == "validate_lane_inventory":
            extra = lanes[0].directory / ".post-inventory-extra"
            extra.write_bytes(b"post publication extra\n")
            extra.chmod(0o600)
            return
        if mode == name:
            raise OSError(f"{name} phase poison")
        if mode.startswith("lane-temp-") and name == "validate_two_temps":
            poison_lane(final=False, variant=mode.removeprefix("lane-temp-"))
        if mode.startswith("lane-final-") and name == "validate_two_finals":
            poison_lane(final=True, variant=mode.removeprefix("lane-final-"))
        if mode == "rollback-aggregate" and name == "validate_two_temps":
            raise OSError("rollback aggregate primary poison")
        if mode in {"summary-unlink", "summary-fsync"} and name == "summary-link":
            raise OSError(f"{mode} primary poison")
        if mode == "summary-symlink" and name == "summary-write":
            path = layout[0] / f".real-differential-controller-{layout[1]}.tmp"
            path.unlink()
            path.symlink_to("missing")
        if mode == "summary-swap" and name == "summary-write":
            path = layout[0] / f".real-differential-controller-{layout[1]}.tmp"
            path.rename(path.with_suffix(".swap"))
            descriptor = original_open(
                path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600
            )
            os.write(descriptor, b"{}\n")
            original_fsync(descriptor)
            os.close(descriptor)

    try:
        with (
            patch.object(contract, "validate_probe", side_effect=lambda document, *_: document),
            patch.object(contract, "validate_pair", return_value=None),
            patch.object(contract, "build_controller_summary", return_value=summary),
            patch.object(contract, "validate_controller_summary", side_effect=validate_summary),
            patch.object(os, "link", side_effect=link),
            patch.object(os, "fsync", side_effect=fsync),
            patch.object(os, "unlink", side_effect=unlink),
            patch.object(os, "open", side_effect=open_file),
            patch.object(shutil, "rmtree", side_effect=rmtree) as mocked_rmtree,
        ):
            mocked_rmtree.avoids_symlink_attacks = original_rmtree.avoids_symlink_attacks

            def action():
                return api.commit_validated_run(
                    layout, bundle, results, 6, executables, contexts, summary, phase
                )

            try:
                published = action() if successful else _expect_failure(action, "publication")
            except AssertionError as error:
                raise AssertionError(f"transaction {mode}: {error}") from error
        if mode == "rollback-aggregate":
            ensure(
                all(
                    token in published
                    for token in (
                        "rollback aggregate primary",
                        "aggregate unlink",
                        "aggregate fsync",
                    )
                ),
                "rollback error aggregation differs",
            )
        finals = tuple(lane.final_result for slot in layout[2] for lane in slot.lanes())
        summary_temp = layout[0] / f".real-differential-controller-{layout[1]}.tmp"
        summary_path = layout[0] / "real-differential-controller-result.json"
        expected_flags = os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
        ensure(
            not summary_opens or summary_opens == [(summary_temp.name, expected_flags)],
            "summary open/reopen receipt differs",
        )
        if successful:
            ensure(
                published == summary_path and all(path.is_file() for path in finals),
                "commit success differs",
            )
            ensure(
                all(
                    tuple(sorted(item.name for item in lane.directory.iterdir()))
                    == (lane.final_result.name,)
                    for lane in lanes
                ),
                "commit success lane inventory differs",
            )
            ensure(
                tuple(sta_removals) == lane_fds and len(rmtree_attempts) == 36,
                "commit success sta removal receipt differs",
            )
        else:
            remaining = [
                path
                for path in (*finals, summary_temp, summary_path)
                if path.exists() or path.is_symlink()
            ]
            ensure(not remaining, "rollback left final")
            ensure(
                tuple(rollback_trace) == rollback_targets * 2,
                "rollback did not cover every target in exactly two rounds",
            )
            if mode in {"unlink", "summary-unlink"}:
                ensure(unlink_poisoned, "unlink poison scope differs")
        ensure(_file_state(sentinel) == sentinel_state, "external sta target changed")
        preflight_zero = {
            "remove_probe_sta",
            "sta-missing",
            "sta-root-file",
            "sta-root-symlink",
            "sta-lane-extra",
            "sta-mode-invalid",
        }
        if mode in preflight_zero:
            ensure(
                not rmtree_attempts and not sta_removals and links == 0,
                "sta preflight rejection mutated publication state",
            )
            ensure(
                all(
                    path.is_file() and _file_state(path) == state for path, state in survivor_states
                ),
                "sta preflight rejection changed a later lane",
            )
        if mode == "probe_sta_removed":
            ensure(
                len(rmtree_attempts) == 36
                and tuple(sta_removals) == lane_fds
                and links == 0
                and all(not path.exists() and not path.is_symlink() for path in sta_paths),
                "probe_sta_removed receipt differs",
            )
        if mode == "sta-rmtree-error":
            ensure(
                len(rmtree_attempts) == 2
                and tuple(sta_removals) == lane_fds[:1]
                and links == 0
                and not sta_paths[0].exists()
                and all(path.is_dir() for path in sta_paths[1:]),
                "sta rmtree failure receipt differs",
            )
        if mode == "sta-fsync-error":
            ensure(
                sta_fsync_poisoned
                and len(rmtree_attempts) == 1
                and tuple(sta_removals) == lane_fds[:1]
                and links == 0
                and not sta_paths[0].exists()
                and all(path.is_dir() for path in sta_paths[1:]),
                "sta fsync failure receipt differs",
            )
        if mode == "validate_lane_inventory":
            ensure(
                len(rmtree_attempts) == 36
                and tuple(sta_removals) == lane_fds
                and links == 36
                and (lanes[0].directory / ".post-inventory-extra").is_file(),
                "lane inventory validation receipt differs",
            )
        ensure(_handle_state(handles) == handle_state, "commit changed completed handles")
        base_fsyncs = 2 if mode == "complete" else 1
        ensure(
            all(
                synced.count(descriptor) == base_fsyncs + sta_removals.count(descriptor)
                for descriptor in layout[6]
            ),
            "layout fsync count differs",
        )
        swap = summary_temp.with_suffix(".swap")
        if mode == "summary-swap":
            ensure(swap.is_file(), "summary swap witness is absent")
            swap.unlink()
    finally:
        for directory_fd, name in drift_cleanup:
            original_unlink(name, dir_fd=directory_fd)
        for result in results:
            for _lane, _lane_fd, descriptor, _saved in result[5]:
                os.close(descriptor)
        for descriptor in reversed(layout[6]):
            os.close(descriptor)


def run_self_test(api):
    ensure(model is not None, "model bootstrap failed")
    _schema_tests()
    bundle = model.load_manifest(api.manifest_path, api.baseline_manifest_path)
    with tempfile.TemporaryDirectory(prefix="ista-step8-selftest-") as raw:
        root, stub = Path(raw), Path(raw) / "probe_stub.py"
        stub.write_text(_STUB, encoding="utf-8")
        _launch_zero_tests(api, root, bundle)
        _process_tests(api, root, stub)
        for mode in _TRANSACTION_MODES:
            _transaction_test(api, root / mode, bundle, stub, mode)
