"""Historical per-batch and final-only consumer trace contracts and statistics."""

from json_schema_contract import ValidationError

# Stage 12 uses complete traces as sampling units. Historical v1 batch records
# retain their own validator and are never interpreted as independent traces.
MODES = ("cpu/full", "cpu/incremental", "cuda/full", "cuda/incremental")
TIMING_MODES = ("cpu/full", "cuda/incremental")
TIMING_PAIRS = (("cpu/full", "cuda/incremental"),)
SECONDARY_TIMING_METRICS = ("edit_sta_loop_ms", "workload_ms", "cold_end_to_end_ms")
BOOTSTRAP_SEED = 20260921
BOOTSTRAP_RESAMPLES = 10000


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def consumer_statistics(
    samples: list[dict], metric: str = "consumer_step_ms", *,
    modes: tuple[str, ...] = MODES,
    pairs: tuple[tuple[str, str], ...] = (("cpu/full", "cuda/full"),
                                        ("cuda/full", "cuda/incremental")),
) -> tuple[dict, dict]:
    """Pair each full-trace repetition; never bootstrap individual edit batches."""
    import random
    import statistics

    values = {
        mode: [sample[metric] for sample in samples
               if sample["mode"] == mode and not sample["warmup"]]
        for mode in modes
    }
    summaries = {}
    for mode, times in values.items():
        median = statistics.median(times)
        summaries[mode] = {
            "n": len(times), "median_ms": median, "p95_ms": percentile(times, 0.95),
            "mad_ms": statistics.median(abs(value - median) for value in times),
        }
    comparisons = {}
    for numerator, denominator in pairs:
        ratios = [left / right for left, right in
                  zip(values[numerator], values[denominator], strict=True)]
        rng = random.Random(BOOTSTRAP_SEED)
        boot = [statistics.median(rng.choices(ratios, k=len(ratios)))
                for _ in range(BOOTSTRAP_RESAMPLES)]
        low, high = percentile(boot, 0.025), percentile(boot, 0.975)
        comparisons[f"{denominator}_vs_{numerator}"] = {
            "paired_speedup_median": statistics.median(ratios),
            "ci95": [low, high], "lower_bound_above_one": low > 1,
        }
    return summaries, comparisons


def validate_consumer_measurement(value: dict, mode: str, prefix: int,
                                  threads: int, affinity: list[int]) -> dict:
    import math

    def require(condition, message):
        if not condition:
            raise ValidationError(message)

    final_only = value["schema_version"] == "ista-consumer-sample/3.0.0"
    require(final_only or value["schema_version"] == "ista-consumer-sample/2.0.0",
            "unsupported raw consumer sample")
    require(mode in (TIMING_MODES if final_only else MODES), "unsupported consumer mode")
    if final_only:
        require(value["report_schedule"] == "final_only", "report schedule differs")
        require(value["result"]["report_consumptions"] == 1, "final report must be consumed once")
        for key in ("timing_node_count", "baseline_instance_count", "baseline_timing_node_count"):
            require(type(value["result"][key]) is int and value["result"][key] >= 0,
                    "missing actual result scale")
        require(value["result"]["timing_node_count"] >=
                value["result"]["baseline_timing_node_count"] > 0, "invalid result node count")
    require(value["debug_checks_enabled"] is False
            and value["build"]["debug_checks_enabled"] is False
            and value["build"]["build_config"] == "Release",
            "consumer performance requires Release/OFF")
    require(value["correctness_comparison"] == value["diagnostics"] == "not_run",
            "OFF performance must not run diagnostic comparisons")
    require(f'{value["requested_backend"]}/{value["requested_strategy"]}' == mode,
            "requested mode differs")
    require(value["threads_requested"] == threads and value["cpu_affinity"] == affinity,
            "host configuration differs from frozen manifest")
    require(value["thread_activity"] == "external_profile",
            "requested threads are not evidence of actual thread execution")
    require(value["prefix_batches"] == prefix == value["result"]["generation"]
            and value["lifecycle_cleanup_completed"] is True, "incomplete workload")
    device = value["device"]
    if mode.startswith("cpu/"):
        require(device is None, "CPU sample must not claim CUDA device execution")
    else:
        import re
        require(isinstance(device, dict) and set(device) == {
            "name", "uuid", "driver_version", "runtime_version",
        }, "CUDA sample is missing device identity")
        require(isinstance(device["name"], str) and bool(device["name"])
                and isinstance(device["uuid"], str)
                and re.fullmatch(r"GPU-[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}",
                                 device["uuid"]) is not None
                and all(type(device[field]) is int and device[field] > 0
                        for field in ("driver_version", "runtime_version")),
                "invalid CUDA device identity")
    require(value["result_requirements"] == {
        "timing_values": "all_nodes_arrival_required_slack", "qor": "complete",
        "max_paths": 20, "nworst": 20, "file_artifacts": False,
    }, "consumer requirements differ")

    def duration(number):
        require(type(number) in (float, int) and math.isfinite(number) and number >= 0,
                "duration must be finite and nonnegative")
        return number

    batches = value["batch_timings"]
    require(len(batches) == prefix, "missing batch timings")
    for generation, batch in enumerate(batches, 1):
        require(batch["generation_after"] == generation and batch["fallback"] is False
                and f'{batch["executed_backend"]}/{batch["executed_strategy"]}' == mode,
                "batch execution differs from required mode")
        if final_only and mode.startswith("cuda/"):
            require("readback" in batch, "missing CUDA readback accounting")
        if mode.startswith("cuda/") and "readback" in batch:
            readback = batch["readback"]
            categories = ("levelization_decisions", "level_metadata",
                          "status", "candidate", "result")
            require(readback["other_d2h_bytes"] == 0
                    and sum(readback[key]["bytes"] for key in categories) == batch["d2h_bytes"],
                    "performance CUDA readback categories differ from actual bytes")
            require(sum(duration(readback[key]["wait_ms"]) for key in categories)
                    <= duration(readback["stream_wait_ms"]) + 1e-6,
                    "CUDA readback waits exceed all stream waits")
        if mode.startswith("cuda/"):
            for counter in ("explicit_kernel_launches", "device_algorithm_calls",
                            "levelization_decision_bytes", "allocations", "h2d_bytes",
                            "d2h_bytes"):
                require(type(batch.get(counter)) is int and batch[counter] >= 0,
                        f"CUDA sample is missing a nonnegative {counter}")
            require("kernel_launches" not in batch, "ambiguous total CUDA launch count")
        elapsed = duration(batch["consumer_step_ms"])
        phases = duration(batch["commit_ms"])
        fields = (("query_materialize_ms", "report_prepare_ms", "consumer_read_ms")
                  if final_only else ("query_materialize_ms", "consumer_read_ms"))
        if final_only:
            require(batch["results_consumed"] is (generation == prefix),
                    "results must be consumed only after the final batch")
        if final_only and generation < prefix:
            require(all(batch[key] is None for key in fields),
                    "unexecuted reports must be null")
        else:
            phases += sum(duration(batch[key]) for key in fields)
        require(elapsed > 0 and elapsed + 1e-6 >= phases, "consumer omits completed phase")
        for key, number in batch.items():
            if key.endswith("_ms") and number is not None:
                duration(number)
        for number in batch.get("cuda_phases", {}).values():
            duration(number)
    times = {key: duration(value[key]) for key in (
        "consumer_step_ms", "load_initialize_ms", "backend_setup_ms", "workload_ms",
        "cleanup_ms", "cold_end_to_end_ms",
    )}
    require(math.isclose(times["consumer_step_ms"],
                         sum(batch["consumer_step_ms"] for batch in batches),
                         rel_tol=1e-12, abs_tol=1e-9), "trace timing is not sum of its batches")
    require(times["consumer_step_ms"] + times["backend_setup_ms"] <= times["workload_ms"] + 1e-9
            and times["cold_end_to_end_ms"] + 1e-9 >=
            sum(times[key] for key in ("load_initialize_ms", "workload_ms", "cleanup_ms")),
            "inclusive workload/lifecycle timing is inconsistent")
    extra = {}
    if final_only:
        loop = duration(value["consumer_loop_ms"])
        edit = duration(value["edit_sta_loop_ms"])
        require(times["consumer_step_ms"] <= loop + 1e-6
                and edit <= loop
                and loop + times["backend_setup_ms"] <= times["workload_ms"] + 1e-6,
                "continuous loop timing omits completed work")
        final_report = sum(duration(batches[-1][key]) for key in
                           ("query_materialize_ms", "report_prepare_ms", "consumer_read_ms"))
        require(edit + final_report <= loop + 1e-6, "loop omits final results")
        extra = {"consumer_loop_ms": loop, "edit_sta_loop_ms": edit,
                 "lifecycle_cleanup_completed": value["lifecycle_cleanup_completed"],
                 "result": {key: item for key, item in value["result"].items() if key != "public"}}
    return {**times, **extra, "batch_timings": batches, "threads_requested": threads,
            "cpu_affinity": affinity, "thread_activity": value["thread_activity"], "device": device}


def validate_consumer_samples(document: dict) -> None:
    final_only = document["schema_version"] == "3.0.0"
    modes = TIMING_MODES if final_only else MODES
    protocol = document["protocol"]
    if protocol["mode_order"] != list(modes):
        raise ValidationError("modes must use the frozen rotation")
    samples = document["samples"]
    expected = [(warmup, repetition, mode)
                for warmup, count in ((True, protocol["warmups"]), (False, protocol["repetitions"]))
                for repetition in range(1, count + 1) for mode in modes]
    actual = [(item["warmup"], item["repetition"], item["mode"]) for item in samples]
    if actual != expected:
        raise ValidationError("missing, duplicated, reordered or unpaired full-trace samples")
    case = document["case"]
    cuda_device = None
    for item in samples:
        # Reuse the raw contract without duplicating full QOR/path objects.
        value = dict(item)
        backend, strategy = item["mode"].split("/")
        value.update({
            "schema_version": f"ista-consumer-sample/{document['schema_version']}",
            "build": document["build"], "debug_checks_enabled": False,
            "correctness_comparison": "not_run", "diagnostics": "not_run",
            "requested_backend": backend, "requested_strategy": strategy,
            "prefix_batches": case["prefix_batches"],
            "result": item["result"] if final_only else {"generation": case["prefix_batches"]},
            "lifecycle_cleanup_completed": (item["lifecycle_cleanup_completed"]
                                            if final_only else True),
            "result_requirements": document["result_requirements"],
        })
        if final_only:
            value["report_schedule"] = document["report_schedule"]
        validate_consumer_measurement(value, item["mode"], case["prefix_batches"],
                                      case["threads"], case["cpu_affinity"])
        if item["mode"].startswith("cuda/"):
            if cuda_device is not None and item["device"] != cuda_device:
                raise ValidationError("CUDA device/runtime changed during paired experiment")
            cuda_device = item["device"]
    summaries, comparisons = (
        consumer_statistics(samples, "consumer_loop_ms", modes=TIMING_MODES, pairs=TIMING_PAIRS)
        if final_only else consumer_statistics(samples))
    if document["summaries"] != summaries or document["comparisons"] != comparisons:
        raise ValidationError("statistics differ from retained full-trace samples")
    if final_only and "additional_metrics" in document:
        for metric in SECONDARY_TIMING_METRICS:
            phase_summaries, phase_comparisons = consumer_statistics(
                samples, metric, modes=TIMING_MODES, pairs=TIMING_PAIRS)
            if document["additional_metrics"][metric] != {
                    "summaries": phase_summaries, "comparisons": phase_comparisons}:
                raise ValidationError(f"{metric} statistics differ from retained samples")
    engineering = protocol["warmups"] >= 5 and protocol["repetitions"] >= 30
    expected_gate = (
        "passed" if all(result["lower_bound_above_one"] for result in comparisons.values())
        else "failed"
    ) if engineering else "not_run"
    if document["performance_gate"] != expected_gate:
        raise ValidationError("performance gate contradicts samples or smoke size")
    if document["sampling_kind"] != ("engineering" if engineering else "short_smoke"):
        raise ValidationError("sampling kind differs from sample counts")


def _consumer_statistics_self_test() -> None:
    samples = [
        {"mode": mode, "warmup": False, "consumer_step_ms": float(value)}
        for repetition in range(1, 6)
        for mode, value in zip(MODES, (8 * repetition, 6 * repetition,
                                       4 * repetition, 2 * repetition), strict=True)
    ]
    summaries, comparisons = consumer_statistics(samples)
    assert summaries["cpu/full"] == {"n": 5, "median_ms": 24.0, "p95_ms": 38.4, "mad_ms": 8.0}
    assert all(result == {"paired_speedup_median": 2.0, "ci95": [2.0, 2.0],
                          "lower_bound_above_one": True} for result in comparisons.values())
    # Pairing matters: quotient of mode medians would miss this distribution.
    uneven = [{"mode": mode, "warmup": False, "consumer_step_ms": float(value)}
              for row in ((1, 1, 1, 1), (10, 10, 100, 100), (100, 100, 10, 10))
              for mode, value in zip(MODES, row, strict=True)]
    assert consumer_statistics(uneven)[1]["cuda/full_vs_cpu/full"]["paired_speedup_median"] == 1
    print("consumer statistics self-test passed")
