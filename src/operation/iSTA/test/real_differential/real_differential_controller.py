#!/usr/bin/env python3
# ***************************************************************************************
# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
#
# iEDA is licensed under Mulan PSL v2.
# ***************************************************************************************
"""Run paired native-oracle/reference probes in isolated fresh processes."""

import argparse
import importlib
import math
import os
import secrets
import shutil
import stat
import subprocess
import sys
import time
from collections.abc import Sequence
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import real_differential_contract as contract
from real_differential_contract import DifferentialContractError, ensure
from real_differential_contract import collect_error as _attempt

_ROOT = Path(__file__)
_ROOT = _ROOT.resolve().parent
_MANIFEST = _ROOT / "manifest.json"
_BASELINE = _ROOT.parent / "baseline" / "manifest.json"


_UID = os.geteuid() if hasattr(os, "geteuid") else None


def _load_selftest_runner(name):
    module = importlib.import_module("real_differential_controller_selftest")
    runner = getattr(module, name, None)
    ensure(callable(runner), f"self-test runner is absent: {name}")
    return runner


_DIR_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW


def _mkdirat(parent, name, where):
    os.mkdir(name, 0o700, dir_fd=parent)
    descriptor = os.open(name, _DIR_FLAGS, dir_fd=parent)
    try:
        os.fchmod(descriptor, 0o700)
        meta = os.fstat(descriptor)
        valid = stat.S_ISDIR(meta.st_mode) and stat.S_IMODE(meta.st_mode) == 0o700
        ensure(valid and (_UID is None or meta.st_uid == _UID), f"{where} directory differs")
        return descriptor
    except Exception:
        os.close(descriptor)
        raise


def _fsync_layout(layout):
    root_fd, slot_fds, lane_fds = layout[3:6]
    errors = []
    for lane_fd in (fd for pair in lane_fds for fd in pair):
        _attempt(errors, f"fsync lane {lane_fd}", lambda lane_fd=lane_fd: os.fsync(lane_fd))
    for slot_fd in slot_fds:
        _attempt(errors, f"fsync slot {slot_fd}", lambda slot_fd=slot_fd: os.fsync(slot_fd))
    _attempt(errors, f"fsync root {root_fd}", lambda: os.fsync(root_fd))
    ensure(not errors, "layout fsync errors: " + str.join("; ", errors))


def _close_layout(layout):
    errors = []
    _attempt(errors, "final fsync", lambda: _fsync_layout(layout))
    for descriptor in reversed(layout[6]):
        _attempt(
            errors, f"close fd {descriptor}", lambda descriptor=descriptor: os.close(descriptor)
        )
    ensure(not errors, "layout finalization errors: " + str.join("; ", errors))


def _prepare_layout(run_root, bundle, repeats):
    run_root = run_root.resolve()
    run_root.mkdir(parents=True, exist_ok=True)
    ensure(stat.S_ISDIR(run_root.lstat().st_mode), "invalid run-root")
    run_root_fd = os.open(run_root, _DIR_FLAGS)
    created, slot_fds, lane_flat = [], [], []
    try:
        nonce = secrets.token_hex(16)
        directory = run_root / f"real-differential-{os.getpid()}-{nonce}"
        root_fd = _mkdirat(run_root_fd, directory.name, "controller root")
        created.append(root_fd)
        slots, lane_fds, nonces = [], [], {nonce}
        for case in bundle.cases:
            for repeat in range(repeats):
                ordinal = len(slots)
                pair_dir = directory / f"slot-{ordinal:03d}-{case.name}-r{repeat}"
                pair_fd = _mkdirat(root_fd, pair_dir.name, "pair slot")
                created.append(pair_fd)
                slot_fds.append(pair_fd)
                lanes, fds = [], []
                for role in ("oracle", "reference"):
                    lane_nonce = secrets.token_hex(16)
                    ensure(lane_nonce not in nonces, "controller/lane nonce collision")
                    nonces.add(lane_nonce)
                    lane_dir = pair_dir / role
                    lane_fd = _mkdirat(pair_fd, role, f"{role} lane")
                    created.append(lane_fd)
                    lane_flat.append(lane_fd)
                    fds.append(lane_fd)
                    temporary = lane_dir / f".{role}-result-{lane_nonce}.tmp"
                    final = lane_dir / f"{role}-result.json"
                    ensure(
                        not any(path.exists() or path.is_symlink() for path in (temporary, final)),
                        f"{role} result path preexists",
                    )
                    lanes.append(contract.LaneSlot(role, lane_nonce, lane_dir, temporary, final))
                lane_fds.append(tuple(fds))
                slots.append(contract.PairSlot(ordinal, case, repeat, pair_dir, *lanes))
        pair_count = len(bundle.cases) * repeats
        lane_count = 2 * pair_count
        ensure(
            (len(bundle.cases), len(slots), len(nonces)) == (3, pair_count, 1 + lane_count),
            "controller slot/nonce cardinality differs",
        )
        layout = directory, nonce, tuple(slots), root_fd
        layout += tuple(slot_fds), tuple(lane_fds), tuple(created)
        _fsync_layout(layout)
        return layout
    except Exception as exc:
        errors = []
        for descriptor in (*lane_flat, *slot_fds, *created[:1]):
            _attempt(
                errors, "partial-layout fsync", lambda descriptor=descriptor: os.fsync(descriptor)
            )
        for descriptor in reversed(created):
            _attempt(
                errors, "partial-layout close", lambda descriptor=descriptor: os.close(descriptor)
            )
        if errors:
            raise DifferentialContractError(str.join("; ", errors)) from exc
        raise
    finally:
        os.close(run_root_fd)


def _communicate(process, deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise subprocess.TimeoutExpired(process.args, 0)
    return process.communicate(timeout=remaining)


def _cleanup(processes):
    errors = []

    def signal(action):
        for process in processes:
            if process.poll() is not None:
                continue
            method = process.terminate if action == "terminate" else process.kill
            _attempt(errors, f"{action} PID {process.pid}", method, (ProcessLookupError,))

    def drain(stage):
        deadline = time.monotonic() + 2.0
        with ThreadPoolExecutor(max_workers=max(1, len(processes))) as pool:
            futures = [pool.submit(_communicate, process, deadline) for process in processes]
            for process, future in zip(processes, futures, strict=True):
                _attempt(errors, f"{stage} drain PID {process.pid}", future.result)

    signal("terminate")
    drain("terminate")
    signal("kill")
    drain("kill")
    for process in processes:
        _attempt(errors, f"wait PID {process.pid}", lambda process=process: process.wait(timeout=0))
    live = [process.pid for process in processes if process.poll() is None]
    ensure(not live, f"child cleanup left live PIDs {live}")
    ensure(not errors, "child cleanup errors: " + str.join("; ", errors))


def _spawn_and_drain(commands, timeout, spawned_pids=None):
    ensure(len(commands) == 2, "pair launch requires exactly two commands")
    deadline = time.monotonic() + timeout
    processes = []
    try:
        for argv, cwd in commands:
            process = subprocess.Popen(
                argv,
                cwd=cwd,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            processes.append(process)
            if spawned_pids is not None:
                spawned_pids.append(process.pid)
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(_communicate, process, deadline) for process in processes]
            streams = tuple(future.result() for future in futures)
    except Exception as exc:
        try:
            _cleanup(processes)
        except DifferentialContractError as cleanup:
            raise DifferentialContractError(f"pair launch/drain failed: {exc}; {cleanup}") from exc
        raise DifferentialContractError(f"pair launch/drain failed: {exc}") from exc
    if any(process.poll() is None for process in processes):
        _cleanup(processes)
        raise DifferentialContractError("pair child remained live")
    return tuple((process, *streams) for process, streams in zip(processes, streams, strict=True))


def _read_fd(descriptor, links, where):
    os.lseek(descriptor, 0, os.SEEK_SET)
    before = os.fstat(descriptor)
    identity = contract.file_identity(before)
    valid = stat.S_ISREG(before.st_mode) and identity[3] == 0o600 and identity[5] == links
    ensure(valid and (_UID is None or identity[4] == _UID), f"{where} metadata differs")
    chunks = []
    while chunk := os.read(descriptor, 65536):
        chunks.append(chunk)
    document = contract.parse_strict_json_for_test(bytes.join(b"", chunks))
    ensure(contract.file_identity(os.fstat(descriptor)) == identity, f"{where} changed")
    return document, identity


def _assert_linked(descriptor, directory_fd, name, saved, links, where):
    expected = (*saved[:5], links)
    held = contract.file_identity(os.fstat(descriptor))
    linked = contract.file_identity(os.stat(name, dir_fd=directory_fd, follow_symlinks=False))
    ensure(held == expected and linked == expected, f"{where} inode differs")


def _lane_inventory(directory_fd):
    names = os.listdir(directory_fd)
    ensure(len(names) == len(set(names)), "lane inventory contains duplicate names")
    return set(names)


def _remove_probe_sta(temps):
    ensure(
        shutil.rmtree.avoids_symlink_attacks,
        "probe sta removal is not symlink-attack resistant",
    )
    prepared = []
    for lane, lane_fd, file_fd, saved in temps:
        _assert_linked(file_fd, lane_fd, lane.temporary_result.name, saved, 1, "lane temp")
        lane_info = os.fstat(lane_fd)
        lane_identity = contract.file_identity(lane_info)
        ensure(
            stat.S_ISDIR(lane_info.st_mode)
            and lane_identity[3] == 0o700
            and (_UID is None or lane_identity[4] == _UID),
            f"{lane.role} lane metadata differs before probe sta removal",
        )
        ensure(
            _lane_inventory(lane_fd) == {lane.temporary_result.name, "sta"},
            f"{lane.role} lane inventory differs before probe sta removal",
        )
        sta_info = os.stat("sta", dir_fd=lane_fd, follow_symlinks=False)
        sta_identity = contract.file_identity(sta_info)
        ensure(
            stat.S_ISDIR(sta_info.st_mode)
            and sta_identity[0] == lane_identity[0]
            and sta_identity[3] in {0o700, 0o755}
            and (_UID is None or sta_identity[4] == _UID),
            f"{lane.role} probe sta metadata differs",
        )
        prepared.append((lane, lane_fd, file_fd, saved, lane_identity, sta_identity))

    for lane, lane_fd, file_fd, saved, lane_identity, sta_identity in prepared:
        ensure(
            contract.file_identity(os.fstat(lane_fd)) == lane_identity,
            f"{lane.role} lane identity changed before probe sta removal",
        )
        current_sta = os.stat("sta", dir_fd=lane_fd, follow_symlinks=False)
        ensure(
            stat.S_ISDIR(current_sta.st_mode)
            and contract.file_identity(current_sta) == sta_identity,
            f"{lane.role} probe sta identity changed before removal",
        )
        _assert_linked(file_fd, lane_fd, lane.temporary_result.name, saved, 1, "lane temp")
        shutil.rmtree("sta", dir_fd=lane_fd)
        os.fsync(lane_fd)
        ensure(
            _lane_inventory(lane_fd) == {lane.temporary_result.name},
            f"{lane.role} lane inventory differs after probe sta removal",
        )
        _assert_linked(file_fd, lane_fd, lane.temporary_result.name, saved, 1, "lane temp")


def _hardlink(directory_fd, source, target):
    os.link(
        source,
        target,
        src_dir_fd=directory_fd,
        dst_dir_fd=directory_fd,
        follow_symlinks=False,
    )


def _unlink(errors, directory_fd, name):
    _attempt(
        errors,
        f"unlink {name}",
        lambda: os.unlink(name, dir_fd=directory_fd),
        (FileNotFoundError,),
    )


def _validate_record(document, slot, lane, execution, executable):
    pid, argv = execution
    return contract.validate_probe(
        document,
        slot.descriptor,
        lane.role,
        slot.repeat_index,
        lane.run_nonce,
        pid,
        argv,
        executable,
        lane.directory,
        lane.temporary_result,
    )


def _run_pair(oracle, reference, slot, lane_fds, timeout, bundle):
    lanes = slot.lanes()
    argvs = tuple(
        contract.probe_argv(exe, slot, lane)
        for exe, lane in zip((oracle, reference), lanes, strict=True)
    )
    launched = _spawn_and_drain(
        tuple((argv, lane.directory) for argv, lane in zip(argvs, lanes, strict=True)), timeout
    )
    processes = tuple(item[0] for item in launched)
    ensure(processes[0].pid != processes[1].pid, "pair PIDs are equal")
    records, temps = [], []
    try:
        for lane, lane_fd, executable, argv, result in zip(
            lanes, lane_fds, (oracle, reference), argvs, launched, strict=True
        ):
            process, _stdout, stderr = result
            if process.returncode != 0:
                preview = bytes.decode(stderr[:400], "utf-8", errors="backslashreplace")
                raise DifferentialContractError(f"{lane.role} child exit: {preview!r}")
            ensure(tuple(process.args) == argv, "completed child argv changed")
            try:
                os.stat(lane.final_result.name, dir_fd=lane_fd, follow_symlinks=False)
            except FileNotFoundError:
                pass
            else:
                raise DifferentialContractError(f"{lane.role} child wrote final")
            descriptor = os.open(
                lane.temporary_result.name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=lane_fd
            )
            try:
                document, identity = _read_fd(descriptor, 1, f"{lane.role} temp")
            except Exception:
                os.close(descriptor)
                raise
            temps.append((lane, lane_fd, descriptor, identity))
            records.append(_validate_record(document, slot, lane, (process.pid, argv), executable))
        contract.validate_pair(
            records[0], records[1], bundle, bundle.absolute_tolerance, bundle.relative_tolerance
        )
        execution = tuple(
            (process.pid, argv) for process, argv in zip(processes, argvs, strict=True)
        )
        return slot, records[0], records[1], execution, processes, tuple(temps)
    except Exception:
        for _lane, _lane_fd, descriptor, _identity in reversed(temps):
            os.close(descriptor)
        raise


def commit_validated_run(
    layout, bundle, results, repeats, executables, contexts, summary, phase_hook=None
):
    directory, nonce, _slots, root_fd = layout[:4]
    temps = tuple(temp for result in results for temp in result[5])
    temp_name = f".real-differential-controller-{nonce}.tmp"
    final_name = "real-differential-controller-result.json"
    summary_fd = -1
    validation = (bundle, repeats, os.getpid(), nonce, executables, contexts)

    def phase(name):
        if phase_hook is not None:
            phase_hook(name)

    try:
        phase("validate_two_temps")
        for lane, lane_fd, file_fd, saved in temps:
            _assert_linked(file_fd, lane_fd, lane.temporary_result.name, saved, 1, "lane temp")
        phase("remove_probe_sta")
        _remove_probe_sta(temps)
        for lane, lane_fd, file_fd, saved in temps:
            _assert_linked(file_fd, lane_fd, lane.temporary_result.name, saved, 1, "lane temp")
        phase("probe_sta_removed")
        groups = (temps[::2], "link_oracle"), (temps[1::2], "link_reference")
        for selected, label in groups:
            phase(label)
            for lane, lane_fd, file_fd, saved in selected:
                _hardlink(lane_fd, lane.temporary_result.name, lane.final_result.name)
                _assert_linked(file_fd, lane_fd, lane.final_result.name, saved, 2, "lane final")
        phase("unlink_temps")
        for lane, lane_fd, file_fd, saved in temps:
            os.unlink(lane.temporary_result.name, dir_fd=lane_fd)
            _assert_linked(file_fd, lane_fd, lane.final_result.name, saved, 1, "lane unlinked")
        phase("validate_lane_inventory")
        for lane, lane_fd, _file_fd, _saved in temps:
            ensure(
                _lane_inventory(lane_fd) == {lane.final_result.name},
                f"{lane.role} lane inventory differs after final publication",
            )
        phase("validate_two_finals")
        reloaded = []
        for result in results:
            records = []
            for temp, executable, execution in zip(result[5], executables, result[3], strict=True):
                lane, lane_fd, file_fd, saved = temp
                document, identity = _read_fd(file_fd, 1, "lane linked final")
                ensure(identity[:5] == saved[:5], "reloaded final inode differs")
                _assert_linked(file_fd, lane_fd, lane.final_result.name, saved, 1, "reloaded final")
                records.append(_validate_record(document, result[0], lane, execution, executable))
            contract.validate_pair(
                records[0], records[1], bundle, bundle.absolute_tolerance, bundle.relative_tolerance
            )
            reloaded.append((result[0], records[0], records[1], result[3], result[4], result[5]))
        phase("reload")
        ensure(
            contract.build_controller_summary(bundle, reloaded, repeats, nonce) == summary,
            "reloaded finals differ",
        )
        b, r, p, n, e, c = validation
        contract.validate_controller_summary(summary, b, r, p, n, e, c)
        phase("revalidate")
        phase("summary_last")
        summary_fd = os.open(
            temp_name, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=root_fd
        )
        os.fchmod(summary_fd, 0o600)
        payload = contract.canonical_json_bytes(summary) + b"\n"
        while payload:
            written = os.write(summary_fd, payload)
            if written <= 0:
                raise DifferentialContractError("summary write made no progress")
            payload = payload[written:]
        os.fsync(summary_fd)
        decoded, saved_summary = _read_fd(summary_fd, 1, "summary temp")
        contract.validate_controller_summary(decoded, b, r, p, n, e, c)
        ensure(decoded == summary, "summary same-fd decode differs")
        phase("summary-write")
        _assert_linked(summary_fd, root_fd, temp_name, saved_summary, 1, "summary temp")
        _hardlink(root_fd, temp_name, final_name)
        _assert_linked(summary_fd, root_fd, final_name, saved_summary, 2, "summary final")
        phase("summary-link")
        os.unlink(temp_name, dir_fd=root_fd)
        _assert_linked(summary_fd, root_fd, final_name, saved_summary, 1, "summary unlinked")
        os.close(summary_fd)
        summary_fd = -1
        _fsync_layout(layout)
        phase("complete")
        return directory / final_name
    except Exception as exc:
        errors = []
        if summary_fd >= 0:
            _attempt(errors, "close summary", lambda: os.close(summary_fd))
        targets = [
            *((lane_fd, lane.final_result.name) for lane, lane_fd, _fd, _saved in temps),
            (root_fd, temp_name),
            (root_fd, final_name),
        ]
        for _ in range(2):
            for directory_fd, name in targets:
                _unlink(errors, directory_fd, name)
        _attempt(errors, "rollback fsync", lambda: _fsync_layout(layout))
        detail = f"; cleanup: {str.join('; ', errors)}" if errors else ""
        raise DifferentialContractError(f"controller publication failed: {exc}{detail}") from exc


def run_controller(
    *,
    oracle_probe,
    reference_probe,
    manifest_path,
    baseline_manifest,
    run_root,
    repeats,
    timeout_seconds,
    public_negative_self_test=False,
):
    ensure(type(repeats) is int and repeats >= 3, "repeats must be integer >= 3")
    ensure(not public_negative_self_test or repeats == 3, "public flag requires repeats 3")
    ensure(math.isfinite(timeout_seconds) and timeout_seconds > 0, "invalid timeout")
    oracle = contract.resolve_probe(oracle_probe, "oracle")
    reference = contract.resolve_probe(reference_probe, "reference")
    ensure(oracle != reference, "oracle/reference executables must differ")
    bundle = contract.load_manifest(manifest_path, baseline_manifest)
    layout = _prepare_layout(run_root, bundle, repeats)
    results = []
    failure = None
    try:
        for slot, lane_fds in zip(layout[2], layout[5], strict=True):
            results.append(_run_pair(oracle, reference, slot, lane_fds, timeout_seconds, bundle))
        contexts = tuple((result[0], result[3]) for result in results)
        summary = contract.build_controller_summary(bundle, results, repeats, layout[1])
        validation = (bundle, repeats, os.getpid(), layout[1], (oracle, reference), contexts)
        b, r, p, n, e, c = validation
        contract.validate_controller_summary(summary, b, r, p, n, e, c)
        if public_negative_self_test:
            records = tuple((result[1], result[2]) for result in results)
            public_runner = _load_selftest_runner("run_public_negative_matrix")
            public_runner(
                summary,
                validation,
                contract.canonical_context_bytes(contexts),
                records,
                contract.canonical_json_bytes(records),
                layout[2],
                tuple(process for result in results for process in result[4]),
            )
        summary_path = commit_validated_run(
            layout,
            bundle,
            results,
            repeats,
            (oracle, reference),
            contexts,
            summary,
        )
        return contract.ControllerRun(layout[0], summary_path, summary)
    except Exception as exc:
        failure = exc
        raise
    finally:
        cleanup = []
        targets = (
            (lane_fd, name)
            for slot, lane_fds in zip(layout[2], layout[5], strict=True)
            for lane, lane_fd in zip(slot.lanes(), lane_fds, strict=True)
            for name in (
                lane.temporary_result.name,
                lane.final_result.name if failure is not None else "",
            )
            if name
        )
        for lane_fd, name in targets:
            _unlink(cleanup, lane_fd, name)
        if failure is not None:
            for name in (
                f".real-differential-controller-{layout[1]}.tmp",
                "real-differential-controller-result.json",
            ):
                _unlink(cleanup, layout[3], name)
        for _lane, _lane_fd, descriptor, _saved in reversed(
            [temp for result in results for temp in result[5]]
        ):
            _attempt(
                cleanup,
                f"close temp fd {descriptor}",
                lambda descriptor=descriptor: os.close(descriptor),
            )
        _attempt(cleanup, "layout close", lambda: _close_layout(layout))
        if cleanup:
            detail = str.join("; ", cleanup)
            if failure is not None:
                raise DifferentialContractError(f"{failure}; cleanup: {detail}") from failure
            raise DifferentialContractError(detail)


def _parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--oracle-probe", type=Path)
    parser.add_argument("--reference-probe", type=Path)
    parser.add_argument("--manifest", type=Path, default=_MANIFEST)
    parser.add_argument("--baseline-manifest", type=Path, default=_BASELINE)
    parser.add_argument("--run-root", type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout-seconds", type=contract.positive_float, default=120.0)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--self-test", action="store_true")
    group.add_argument("--public-negative-self-test", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None):
    parser = _parser()
    args = parser.parse_args(argv)
    try:
        if args.self_test:
            selftest_runner = _load_selftest_runner("run_self_test")
            selftest_runner(
                contract.SelfTestApi(
                    args.manifest.resolve(),
                    args.baseline_manifest.resolve(),
                    _spawn_and_drain,
                    _prepare_layout,
                    commit_validated_run,
                )
            )
            return 0
        required = (args.oracle_probe, args.reference_probe, args.run_root)
        ensure(all(value is not None for value in required), "probe executables/run-root required")
        run = run_controller(
            oracle_probe=args.oracle_probe,
            reference_probe=args.reference_probe,
            manifest_path=args.manifest,
            baseline_manifest=args.baseline_manifest,
            run_root=args.run_root,
            repeats=args.repeats,
            timeout_seconds=args.timeout_seconds,
            public_negative_self_test=args.public_negative_self_test,
        )
        print(run.summary_path)
        return 0
    except (contract.BaselineError, OSError) as exc:
        print(f"CSR real differential controller failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
