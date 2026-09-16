#!/usr/bin/env python3
"""Validate the closed-world Stage 4 fixed-buffer trace contract."""

import argparse
import copy
import json
import math
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

from buffer_trace_profile_contract import validate_frozen_profiles
from json_schema_contract import ValidationError, fail, load_json, loads_json, validate_schema
import validate_timing_edit_trace as stage3

SCHEMA_PATH = Path(__file__).with_name("buffer_fixed_trace_v2.schema.json")
MANIFEST_PATH = Path(__file__).with_name("buffer_trace_contract_manifest_v1.json")
STAGE3_SCHEMA_PATH = Path(__file__).with_name("timing_edit_trace_v1.schema.json")
_STAGE3_REF_PREFIX = "timing_edit_trace_v1.schema.json#/$defs/"


def _rewrite_stage3_local_refs(value: object) -> object:
    if isinstance(value, dict):
        result: dict[str, object] = {}
        for key, child in value.items():
            if key == "$ref" and isinstance(child, str) and child.startswith("#/$defs/"):
                result[key] = "#/$defs/stage3_" + child.removeprefix("#/$defs/")
            else:
                result[key] = _rewrite_stage3_local_refs(child)
        return result
    if isinstance(value, list):
        return [_rewrite_stage3_local_refs(item) for item in value]
    return value


def _prepare_schema(schema: object) -> dict[str, object]:
    if not isinstance(schema, dict) or not isinstance(schema.get("$defs"), dict):
        raise ValidationError("Stage 4 schema must contain an object $defs")
    stage3_schema = load_json(STAGE3_SCHEMA_PATH)
    if not isinstance(stage3_schema, dict) or not isinstance(stage3_schema.get("$defs"), dict):
        raise ValidationError("Stage 3 schema must contain an object $defs")
    prepared = copy.deepcopy(schema)
    definitions = prepared["$defs"]
    for name, definition in stage3_schema["$defs"].items():
        definitions[f"stage3_{name}"] = _rewrite_stage3_local_refs(definition)

    def rewrite(value: object) -> object:
        if isinstance(value, dict):
            result: dict[str, object] = {}
            for key, child in value.items():
                if key == "$ref" and isinstance(child, str) and child.startswith(_STAGE3_REF_PREFIX):
                    result[key] = "#/$defs/stage3_" + child.removeprefix(_STAGE3_REF_PREFIX)
                elif key == "oneOf":
                    # Branch tags are disjoint constants, so anyOf is equivalent
                    # for the repository's small schema engine.
                    result["anyOf"] = rewrite(child)
                else:
                    result[key] = rewrite(child)
            return result
        if isinstance(value, list):
            return [rewrite(item) for item in value]
        return value

    return rewrite(prepared)


def _validate_manifest(manifest: object) -> dict[str, dict[str, object]]:
    if not isinstance(manifest, dict):
        raise ValidationError("contract manifest must be an object")
    expected_keys = {
        "schema_version",
        "trace_schema",
        "stage3_contract_schema",
        "selection_mode",
        "source_artifact_roles",
        "profiles",
        "replay_projection_excludes",
    }
    if set(manifest) != expected_keys:
        fail("manifest", "must have the exact version-1 key set")
    if (
        manifest["schema_version"] != "buffer-trace-contract-manifest/1.0.0"
        or manifest["trace_schema"] != "buffer-fixed-trace/2.0.0"
        or manifest["stage3_contract_schema"] != "timing-edit-trace/1.0.0"
        or manifest["selection_mode"] != "fixed_trace"
    ):
        fail("manifest", "version or selection mode differs from the frozen contract")
    if manifest["source_artifact_roles"] != [
        "tech_lef",
        "cell_lef",
        "liberty",
        "verilog",
        "sdc",
    ]:
        fail("manifest.source_artifact_roles", "must list the five distinct roles in frozen order")
    excluded = manifest["replay_projection_excludes"]
    if not isinstance(excluded, list) or not {
        "generation_only",
        "arrival_ns",
        "required_ns",
        "slack_ns",
        "candidate_load_pf",
        "gain_ns",
    }.issubset(excluded):
        fail("manifest.replay_projection_excludes", "must exclude generation and timing answers")
    profiles = manifest["profiles"]
    return validate_frozen_profiles(profiles, fail)


def _profile_projection(profile: dict[str, object]) -> dict[str, object]:
    return {
        key: copy.deepcopy(profile[key])
        for key in (
            "id",
            "top_k",
            "rounds",
            "max_total_insertions",
            "analysis",
            "operating_point",
            "rc_profile",
        )
    }


def _token_key(token: dict[str, object]) -> tuple[object, ...]:
    return stage3._token_key(token)


def _created_ref_token(reference: dict[str, object]) -> tuple[object, ...] | None:
    if reference["kind"] == "created":
        return _token_key(reference["creation_token"])
    return None


def _validate_sources(workload: dict[str, Any]) -> None:
    if workload["kind"] == "synthetic":
        recipe = workload["source_recipe"]
        roles = ("tech_lef", "cell_lef", "liberty", "verilog", "sdc")
        names = [recipe[role]["logical_name"] for role in roles]
        if any(name in {".", ".."} or "/" in name for name in names):
            fail("$.replay.workload.source_recipe", "logical source names must be safe Linux leaf names")
        if len(set(names)) != len(names):
            fail("$.replay.workload.source_recipe", "five logical source names must be distinct")
        if recipe["tech_lef"]["source_text"] == recipe["cell_lef"]["source_text"]:
            fail("$.replay.workload.source_recipe", "tech and cell LEF content must be distinct")
    else:
        artifacts = workload["artifacts"]
        paths = [artifacts[role]["path"] for role in ("tech_lef", "verilog", "sdc")]
        paths.extend(source["path"] for source in artifacts["cell_lefs"])
        paths.extend(source["path"] for source in artifacts["liberties"])
        if len(set(paths)) != len(paths):
            fail("$.replay.workload.artifacts", "real source paths must be distinct")


def _validate_batches(
    document: dict[str, object], profile: dict[str, object], *,
    allow_empty: bool = False
) -> set[tuple[object, ...]]:
    header = document["replay"]["header"]
    workload = document["replay"]["workload"]
    batches = workload["batches"]
    if allow_empty and batches:
        fail("$.replay.workload.batches", "generation template must not contain replay edits")
    if not allow_empty and not batches:
        fail("$.replay.workload.batches", "replay program requires at least one batch")
    if len(batches) > profile["rounds"]:
        fail("$.replay.workload.batches", "exceeds frozen profile rounds")
    if any(len(batch["edits"]) > profile["top_k"] for batch in batches):
        fail("$.replay.workload.batches", "a replay batch exceeds frozen profile top-k")
    if sum(len(batch["edits"]) for batch in batches) > profile["max_total_insertions"]:
        fail("$.replay.workload.batches", "replay edits exceed frozen insertion limit")
    if not batches:
        return set()
    baseline = workload.get("baseline", batches[0]["baseline"])
    known_tokens: set[tuple[object, ...]] = set()
    case_id: str | None = None
    seed: int | None = None
    for batch_index, plan in enumerate(batches):
        field = f"$.replay.workload.batches[{batch_index}]"
        if plan["expected_generation"] != batch_index:
            fail(f"{field}.expected_generation", "must be dense and start at generation zero")
        if plan["baseline"] != baseline:
            fail(f"{field}.baseline", "all batches must carry one exact baseline identity")
        encoded_edits: list[bytes] = []
        current_tokens: set[tuple[object, ...]] = set()
        original_nets: set[bytes] = set()
        moved_sinks: set[bytes] = set()
        for edit_index, edit in enumerate(plan["edits"]):
            edit_field = f"{field}.edits[{edit_index}]"
            stage3._validate_edit(edit, edit_field)
            encoded_edits.append(stage3._edit_bytes(edit))
            for reference_field, reference in [("original_net", edit["original_net"]), *[
                (f"moved_sinks[{index}]", sink) for index, sink in enumerate(edit["moved_sinks"])
            ]]:
                referenced = _created_ref_token(reference)
                if referenced is not None and referenced not in known_tokens:
                    fail(f"{edit_field}.{reference_field}", "created reference must resolve to an earlier batch token")
            key = _token_key(edit["creation_token"])
            if key in known_tokens or key in current_tokens:
                fail(f"{edit_field}.creation_token", "creation token must be globally unique")
            current_tokens.add(key)
            token = edit["creation_token"]
            if token["batch_ordinal"] != batch_index + 1:
                fail(f"{edit_field}.creation_token.batch_ordinal", "must equal the one-based replay batch")
            if token["accepted_ordinal"] != edit_index:
                fail(f"{edit_field}.creation_token.accepted_ordinal", "must equal canonical edit order")
            if token["profile_id"] != header["profile_id"]:
                fail(f"{edit_field}.creation_token.profile_id", "must match replay profile")
            case_id = token["case_id"] if case_id is None else case_id
            seed = token["seed"] if seed is None else seed
            if token["case_id"] != case_id or token["seed"] != seed:
                fail(f"{edit_field}.creation_token", "case id and seed must be stable")
            if workload["kind"] == "real_design":
                if token["seed"] != 0:
                    fail(f"{edit_field}.creation_token.seed", "real-design v1 seed must be zero")
                if edit["placement_inputs"] != {"kind": "unplaced"} or edit["rc_inputs"] != {"kind": "no_parasitic"}:
                    fail(edit_field, "real-design v1 requires unplaced/no-parasitic edit inputs")
            net_key = stage3._net_ref_bytes(edit["original_net"])
            if net_key in original_nets:
                fail(f"{edit_field}.original_net", "only one insertion per stable net is allowed")
            original_nets.add(net_key)
            for sink in edit["moved_sinks"]:
                sink_key = stage3._node_ref_bytes(sink)
                if sink_key in moved_sinks:
                    fail(f"{edit_field}.moved_sinks", "sink is moved more than once")
                moved_sinks.add(sink_key)
        if encoded_edits != sorted(encoded_edits):
            fail(f"{field}.edits", "must be in Stage 3 canonical byte order")
        known_tokens.update(current_tokens)
    return known_tokens


def _validate_numeric(value: dict[str, object], field: str,
                      profile: dict[str, object]) -> None:
    try:
        numbers = {key: float(item) for key, item in value.items()}
    except (OverflowError, ValueError, TypeError):
        fail(field, "numeric receipt is not representable as binary64",
             "non_finite")
        return
    if not all(math.isfinite(item) for item in numbers.values()):
        fail(field, "all numeric receipt values must be finite", "non_finite")
    physical = (
        numbers["upstream_resistance_kohm"],
        numbers["candidate_load_pf"],
        numbers["buffer_capacitance_pf"],
        numbers["buffer_resistance_kohm"],
        numbers["buffer_delay_ns"],
    )
    if any(item < 0.0 for item in physical):
        fail(field, "resistance, capacitance and delay operands must be nonnegative")
    rc_profile = profile["rc_profile"]
    if (
        numbers["upstream_resistance_kohm"]
        != float(rc_profile["upstream_resistance_kohm"])
        or numbers["buffer_resistance_kohm"]
        != float(rc_profile["buffer_resistance_kohm"])
    ):
        fail(field, "numeric resistance operands must equal the frozen RC profile")
    delta = numbers["candidate_load_pf"] - numbers["buffer_capacitance_pf"]
    benefit = numbers["upstream_resistance_kohm"] * delta
    penalty = numbers["buffer_resistance_kohm"] * numbers["buffer_capacitance_pf"]
    net_benefit = benefit - penalty
    gain = net_benefit - numbers["buffer_delay_ns"]
    if (
        numbers["required_ns"] - numbers["arrival_ns"] != numbers["slack_ns"]
        or delta != numbers["delta_capacitance_pf"]
        or benefit != numbers["benefit_ns"]
        or penalty != numbers["penalty_ns"]
        or net_benefit != numbers["benefit_minus_penalty_ns"]
        or gain != numbers["gain_ns"]
    ):
        fail(field, "must exactly reproduce the staged binary64 computation")


def _validate_generation(document: dict[str, object], replay_tokens: set[tuple[object, ...]], profiles: dict[str, dict[str, object]]) -> None:
    header = document["replay"]["header"]
    generation = document["generation_only"]
    frozen = profiles[header["profile_id"]]
    if generation["profile"] != _profile_projection(frozen):
        fail("$.generation_only.profile", "must exactly equal the frozen manifest profile")
    rounds = generation["rounds"]
    if len(rounds) > generation["profile"]["rounds"]:
        fail("$.generation_only.rounds", "exceeds configured round count")
    replay_batches = document["replay"]["workload"]["batches"]
    generation_tokens: set[tuple[object, ...]] = set()
    generated_batches: list[list[tuple[object, ...]]] = []
    accepted_count = 0
    for round_index, round_receipt in enumerate(rounds):
        field = f"$.generation_only.rounds[{round_index}]"
        if round_receipt["batch_ordinal"] != round_index + 1:
            fail(f"{field}.batch_ordinal", "must be dense and one-based")
        listed = [_token_key(token) for token in round_receipt["accepted_tokens"]]
        if len(listed) > generation["profile"]["top_k"]:
            fail(f"{field}.accepted_tokens", "exceeds frozen profile top-k")
        if len(set(listed)) != len(listed) or generation_tokens.intersection(listed):
            fail(f"{field}.accepted_tokens", "tokens must be unique within and across rounds")
        if listed:
            generated_batches.append(listed)
        elif round_index + 1 != len(rounds):
            fail(f"{field}.accepted_tokens", "only a terminal empty-stop round may omit a replay batch")
        observed: set[tuple[object, ...]] = set()
        for eval_index, evaluation in enumerate(round_receipt["evaluations"]):
            eval_field = f"{field}.evaluations[{eval_index}]"
            decision = evaluation["decision"]
            numeric = evaluation.get("numeric")
            reason = evaluation.get("rejection_reason")
            accepted_token = evaluation.get("accepted_creation_token")
            if numeric is not None:
                _validate_numeric(numeric, f"{eval_field}.numeric", frozen)
            if decision == "accepted":
                if numeric is None or numeric["gain_ns"] <= 0.0 or reason is not None or accepted_token is None:
                    fail(eval_field, "accepted decision requires positive numeric evidence and exactly one token")
                key = _token_key(accepted_token)
                if key in observed:
                    fail(f"{eval_field}.accepted_creation_token", "accepted evaluation token must be unique")
                observed.add(key)
                accepted_index = accepted_token["accepted_ordinal"]
                if round_index >= len(replay_batches) or accepted_index >= len(replay_batches[round_index]["edits"]):
                    fail(eval_field, "accepted evaluation has no same-round replay edit")
                edit = replay_batches[round_index]["edits"][accepted_index]
                if (accepted_token != edit["creation_token"] or evaluation["net"] != edit["original_net"]
                        or edit["moved_sinks"] != [evaluation["candidate_pin"]]
                        or evaluation["buffer"] != edit["library_cell"]):
                    fail(eval_field, "accepted evaluation must equal the same replay edit token/net/single sink/library")
            elif decision == "rejected":
                if reason is None or accepted_token is not None:
                    fail(eval_field, "rejected decision requires a reason and no token")
                if reason == "non_positive_gain":
                    if numeric is None or numeric["gain_ns"] > 0.0:
                        fail(eval_field, "nonpositive rejection requires nonpositive numeric evidence")
                elif numeric is not None:
                    fail(eval_field, "non-numeric rejection reason must not carry numeric evidence")
            elif numeric is None or numeric["gain_ns"] <= 0.0 or reason is not None or accepted_token is not None:
                fail(eval_field, "unselected positive candidate requires numeric evidence only")
        if set(listed) != observed:
            fail(f"{field}.accepted_tokens", "must equal accepted evaluation tokens as a duplicate-free set")
        generation_tokens.update(set(listed))
        accepted_count += len(listed)
    replay_batch_tokens = [[_token_key(edit["creation_token"]) for edit in plan["edits"]] for plan in replay_batches]
    if generated_batches != replay_batch_tokens:
        fail("$.generation_only.rounds", "each nonempty round must exactly match the same replay batch token order")
    if generation_tokens != replay_tokens:
        fail("$.generation_only", "accepted tokens must exactly equal replay edit tokens")
    if accepted_count > generation["profile"]["max_total_insertions"]:
        fail("$.generation_only.rounds", "accepted count exceeds max_total_insertions")
    stop = generation["stop_reason"]
    last_empty = not rounds[-1]["accepted_tokens"]
    if (stop == "empty_accepted_batch") != last_empty:
        fail("$.generation_only.stop_reason", "must identify exactly a terminal empty round")
    if stop == "rounds_reached" and len(rounds) != generation["profile"]["rounds"]:
        fail("$.generation_only.stop_reason", "requires the configured round count")
    reached_limit = accepted_count == generation["profile"]["max_total_insertions"]
    if (stop == "max_total_insertions_reached") != reached_limit:
        fail("$.generation_only.stop_reason", "must identify exactly reaching the insertion limit")


def validate_document(document: object, schema: object, manifest: object, *, template: bool = False) -> None:
    profiles = _validate_manifest(manifest)
    prepared_schema = _prepare_schema(schema)
    if template:
        for payload in ("syntheticPayload", "realPayload"):
            prepared_schema["$defs"][payload]["properties"]["batches"]["minItems"] = 0
    validate_schema(document, prepared_schema)
    if not isinstance(document, dict):
        raise ValidationError("root artifact must be an object")
    replay = document["replay"]
    header = replay["header"]
    workload = replay["workload"]
    if header["semantic_workload_kind"] != workload["kind"]:
        fail("$.replay.workload.kind", "must match the replay header tagged-union discriminator")
    frozen = profiles[header["profile_id"]]
    if workload["kind"] == "real_design":
        expected_status = "non_comparable"
        expected_reason = "engineering_rc_no_physical"
    else:
        expected_status = frozen["comparability_status"]
        expected_reason = frozen.get("comparability_reason")
    if (header["comparability_status"] != expected_status
            or header.get("comparability_reason") != expected_reason):
        fail("$.replay.header.comparability_status",
             "must exactly match workload kind and frozen profile")
    _validate_sources(workload)
    replay_tokens = _validate_batches(document, frozen, allow_empty=template)
    if template:
        return
    _validate_generation(document, replay_tokens, profiles)


def _valid_artifact(*, real: bool = False) -> dict[str, object]:
    source = stage3._valid_trace()
    baseline = copy.deepcopy(source["baseline"])
    edit = copy.deepcopy(source["batches"][0]["edits"][0])
    edit["creation_token"]["profile_id"] = "engineering_smoke"
    edit["creation_token"]["seed"] = 0 if real else 42
    plan = {
        "schema_version": "timing-edit-plan/1.0.0",
        "baseline": baseline,
        "expected_generation": 0,
        "edits": [edit],
    }
    if real:
        workload = {
            "kind": "real_design",
            "artifacts": {
                "tech_lef": {"path": "fixture/tech.lef"},
                "cell_lefs": [{"path": "fixture/cell.lef"}],
                "liberties": [{"path": "fixture/stage4.lib"}],
                "verilog": {"path": "fixture/design.v"},
                "sdc": {"path": "fixture/design.sdc"},
                "top": "top",
                "analysis_context": {"liberty_corner": "slow", "temperature": "25",
                                     "rc_corner": "__no_rc_corner__"},
            },
            "baseline": baseline,
            "batches": [plan],
        }
    else:
        workload = {
            "kind": "synthetic",
            "source_recipe": {
                "tech_lef": {"logical_name": "tech.lef", "source_text": "TECH LEF"},
                "cell_lef": {"logical_name": "cell.lef", "source_text": "CELL LEF"},
                "liberty": {"logical_name": "stage4.lib", "source_text": "library(stage4){}"},
                "verilog": {"logical_name": "design.v", "source_text": "module top; endmodule"},
                "sdc": {"logical_name": "design.sdc", "source_text": "create_clock -period 1 clk"},
                "top": "top",
            },
            "batches": [plan],
        }
    numeric = {
        "arrival_ns": 0.25,
        "required_ns": 0.5,
        "slack_ns": 0.25,
        "upstream_resistance_kohm": 64.0,
        "candidate_load_pf": 0.003,
        "buffer_capacitance_pf": 0.002,
        "buffer_resistance_kohm": 1.0,
        "buffer_delay_ns": 0.01,
    }
    numeric["delta_capacitance_pf"] = numeric["candidate_load_pf"] - numeric["buffer_capacitance_pf"]
    numeric["benefit_ns"] = numeric["upstream_resistance_kohm"] * numeric["delta_capacitance_pf"]
    numeric["penalty_ns"] = numeric["buffer_resistance_kohm"] * numeric["buffer_capacitance_pf"]
    numeric["benefit_minus_penalty_ns"] = numeric["benefit_ns"] - numeric["penalty_ns"]
    numeric["gain_ns"] = numeric["benefit_minus_penalty_ns"] - numeric["buffer_delay_ns"]
    evaluation = {
        "net": copy.deepcopy(edit["original_net"]),
        "driver": {"kind": "existing", "baseline_key": copy.deepcopy(baseline["canonical_pre_state"]["nodes"][0])},
        "candidate_pin": copy.deepcopy(edit["moved_sinks"][0]),
        "buffer": copy.deepcopy(edit["library_cell"]),
        "numeric": numeric,
        "decision": "accepted",
        "accepted_creation_token": copy.deepcopy(edit["creation_token"]),
    }
    profile = {
        "id": "engineering_smoke",
        "top_k": 4,
        "rounds": 2,
        "max_total_insertions": 4,
        "analysis": "cpu_full",
        "operating_point": {"input_slew_ns": 0.05, "output_load_pf": 0.01},
        "rc_profile": {"id": "engineering_rc_v1", "upstream_resistance_kohm": 64.0, "buffer_resistance_kohm": 1.0},
    }
    return {
        "replay": {
            "header": {
                "schema_version": 2,
                "selection_mode": "fixed_trace",
                "semantic_workload_kind": workload["kind"],
                "profile_id": "engineering_smoke",
                "comparability_status": "non_comparable",
                "comparability_reason": "engineering_rc_no_physical",
            },
            "workload": workload,
        },
        "generation_only": {
            "profile": profile,
            "rounds": [
                {"batch_ordinal": 1, "evaluations": [evaluation], "accepted_tokens": [copy.deepcopy(edit["creation_token"])]},
                {"batch_ordinal": 2, "evaluations": [], "accepted_tokens": []},
            ],
            "stop_reason": "empty_accepted_batch",
        },
    }


def _expect_rejected(name: str, action: Callable[[], object]) -> None:
    try:
        action()
    except ValidationError:
        return
    raise AssertionError(f"negative self-test {name!r} was accepted")


def _run_self_test(schema: object, manifest: object) -> None:
    synthetic = _valid_artifact()
    real = _valid_artifact(real=True)
    validate_document(synthetic, schema, manifest)
    validate_document(real, schema, manifest)
    profiles = _validate_manifest(manifest)
    real_paper = copy.deepcopy(real)
    real_paper["replay"]["header"]["profile_id"] = "paper_protocol_top100_x10"
    real_paper["generation_only"]["profile"] = _profile_projection(
        profiles["paper_protocol_top100_x10"]
    )
    for batch in real_paper["replay"]["workload"]["batches"]:
        for edit in batch["edits"]:
            edit["creation_token"]["profile_id"] = "paper_protocol_top100_x10"
    for round_value in real_paper["generation_only"]["rounds"]:
        for token in round_value["accepted_tokens"]:
            token["profile_id"] = "paper_protocol_top100_x10"
        for evaluation in round_value["evaluations"]:
            token = evaluation.get("accepted_creation_token")
            if token is not None:
                token["profile_id"] = "paper_protocol_top100_x10"
    validate_document(real_paper, schema, manifest)
    template = copy.deepcopy(synthetic)
    template["replay"]["workload"]["batches"] = []
    validate_document(template, schema, manifest, template=True)

    projection_before = json.dumps(synthetic["replay"], sort_keys=True, separators=(",", ":"))
    tampered_generation = copy.deepcopy(synthetic)
    tampered_generation["generation_only"]["rounds"][0]["evaluations"][0]["numeric"]["gain_ns"] += 1.0
    projection_after = json.dumps(tampered_generation["replay"], sort_keys=True, separators=(",", ":"))
    if projection_before != projection_after:
        raise AssertionError("generation-only mutation changed replay projection")
    _expect_rejected("numeric-receipt-tamper", lambda: validate_document(tampered_generation, schema, manifest))

    negatives: list[tuple[str, dict[str, object]]] = []
    unknown = copy.deepcopy(synthetic)
    unknown["replay"]["future"] = True
    negatives.append(("unknown-field", unknown))
    mismatch = copy.deepcopy(synthetic)
    mismatch["replay"]["header"]["semantic_workload_kind"] = "real_design"
    negatives.append(("tagged-union-mismatch", mismatch))
    missing_role = copy.deepcopy(synthetic)
    del missing_role["replay"]["workload"]["source_recipe"]["sdc"]
    negatives.append(("missing-source-role", missing_role))
    lef_alias = copy.deepcopy(synthetic)
    lef_alias["replay"]["workload"]["source_recipe"]["cell_lef"] = copy.deepcopy(
        lef_alias["replay"]["workload"]["source_recipe"]["tech_lef"]
    )
    negatives.append(("lef-alias", lef_alias))
    for suffix, poisoned_name in (("path", "nested/tech.lef"), ("dot", "."), ("dotdot", "..")):
        bad_leaf = copy.deepcopy(synthetic)
        bad_leaf["replay"]["workload"]["source_recipe"]["tech_lef"]["logical_name"] = poisoned_name
        negatives.append((f"logical-name-{suffix}", bad_leaf))
    bad_batch_ordinal = copy.deepcopy(synthetic)
    bad_batch_ordinal["replay"]["workload"]["batches"][0]["edits"][0]["creation_token"]["batch_ordinal"] = 2
    negatives.append(("token-batch-ordinal", bad_batch_ordinal))
    bad_accepted_ordinal = copy.deepcopy(synthetic)
    bad_accepted_ordinal["replay"]["workload"]["batches"][0]["edits"][0]["creation_token"]["accepted_ordinal"] = 1
    negatives.append(("token-accepted-ordinal", bad_accepted_ordinal))
    semantic_mismatch = copy.deepcopy(synthetic)
    semantic_mismatch["generation_only"]["rounds"][0]["evaluations"][0]["net"] = {
        "kind": "existing", "baseline_name": "other_net"
    }
    negatives.append(("accepted-evaluation-semantic-mismatch", semantic_mismatch))
    duplicate_evaluation = copy.deepcopy(synthetic)
    duplicate_evaluation["generation_only"]["rounds"][0]["evaluations"].append(
        copy.deepcopy(duplicate_evaluation["generation_only"]["rounds"][0]["evaluations"][0])
    )
    negatives.append(("duplicate-accepted-evaluation-token", duplicate_evaluation))
    csr_injection = copy.deepcopy(synthetic)
    csr_injection["replay"]["workload"]["source_recipe"]["verilog"]["csr_rows"] = []
    negatives.append(("csr-answer-injection", csr_injection))
    timing_injection = copy.deepcopy(synthetic)
    timing_injection["replay"]["workload"]["source_recipe"]["liberty"]["arrival_ns"] = 0.0
    negatives.append(("timing-answer-injection", timing_injection))
    bad_real = copy.deepcopy(real)
    bad_real["replay"]["header"]["comparability_status"] = "exact"
    negatives.append(("real-exact-claim", bad_real))
    placed_real = copy.deepcopy(real)
    placed_real["replay"]["workload"]["batches"][0]["edits"][0]["placement_inputs"] = {
        "kind": "placed", "x_microns": 0.0, "y_microns": 0.0
    }
    negatives.append(("real-placed", placed_real))
    dense_id = copy.deepcopy(synthetic)
    dense_id["replay"]["workload"]["batches"][0]["edits"][0]["moved_sinks"][0]["dense_id"] = 1
    negatives.append(("dense-id", dense_id))
    profile = copy.deepcopy(synthetic)
    profile["generation_only"]["profile"]["top_k"] = 3
    negatives.append(("profile-mutation", profile))
    real_protocol = copy.deepcopy(real_paper)
    real_protocol["replay"]["header"]["comparability_status"] = "protocol_only"
    del real_protocol["replay"]["header"]["comparability_reason"]
    negatives.append(("real-paper-protocol-only", real_protocol))
    too_many_rounds = copy.deepcopy(synthetic)
    source_plan = too_many_rounds["replay"]["workload"]["batches"][0]
    for index in (1, 2):
        extra = copy.deepcopy(source_plan)
        extra["expected_generation"] = index
        extra["edits"][0]["creation_token"]["batch_ordinal"] = index + 1
        too_many_rounds["replay"]["workload"]["batches"].append(extra)
    negatives.append(("replay-round-limit", too_many_rounds))
    too_many_in_batch = copy.deepcopy(synthetic)
    plan = too_many_in_batch["replay"]["workload"]["batches"][0]
    prototype = plan["edits"][0]
    for index in range(1, 5):
        extra = copy.deepcopy(prototype)
        extra["creation_token"]["accepted_ordinal"] = index
        extra["original_net"] = {"kind": "existing", "baseline_name": f"net_{index}"}
        extra["moved_sinks"][0]["baseline_key"]["full_name"] = f"u{index}:A"
        extra["moved_sinks"][0]["baseline_key"]["instance_name"] = f"u{index}"
        plan["edits"].append(extra)
    negatives.append(("replay-top-k-limit", too_many_in_batch))
    total_limit = copy.deepcopy(synthetic)
    first = total_limit["replay"]["workload"]["batches"][0]
    for index in (1, 2):
        extra = copy.deepcopy(first["edits"][0])
        extra["creation_token"]["accepted_ordinal"] = index
        extra["original_net"] = {"kind": "existing", "baseline_name": f"first_{index}"}
        extra["moved_sinks"][0]["baseline_key"]["full_name"] = f"f{index}:A"
        extra["moved_sinks"][0]["baseline_key"]["instance_name"] = f"f{index}"
        first["edits"].append(extra)
    second = copy.deepcopy(first)
    second["expected_generation"] = 1
    for index, edit in enumerate(second["edits"]):
        edit["creation_token"]["batch_ordinal"] = 2
        edit["original_net"] = {"kind": "existing", "baseline_name": f"second_{index}"}
        edit["moved_sinks"][0]["baseline_key"]["full_name"] = f"s{index}:A"
        edit["moved_sinks"][0]["baseline_key"]["instance_name"] = f"s{index}"
    total_limit["replay"]["workload"]["batches"].append(second)
    negatives.append(("replay-total-limit", total_limit))
    for name, field, value in (
        ("negative-resistance", "upstream_resistance_kohm", -1.0),
        ("negative-capacitance", "candidate_load_pf", -1.0),
        ("negative-delay", "buffer_delay_ns", -1.0),
        ("profile-resistance-drift", "upstream_resistance_kohm", 63.0),
    ):
        poisoned = copy.deepcopy(synthetic)
        poisoned["generation_only"]["rounds"][0]["evaluations"][0]["numeric"][field] = value
        negatives.append((name, poisoned))
    rejected_positive = copy.deepcopy(synthetic)
    evaluation = rejected_positive["generation_only"]["rounds"][0]["evaluations"][0]
    evaluation["decision"] = "rejected"
    evaluation["rejection_reason"] = "non_positive_gain"
    del evaluation["accepted_creation_token"]
    negatives.append(("nonpositive-rejection-with-positive-gain", rejected_positive))
    large_integer = copy.deepcopy(synthetic)
    large_numeric = large_integer["generation_only"]["rounds"][0]["evaluations"][0]["numeric"]
    large_numeric["candidate_load_pf"] = 2**53 + 1
    large_numeric["buffer_capacitance_pf"] = 1
    large_numeric["delta_capacitance_pf"] = 2**53
    large_numeric["benefit_ns"] = 64 * 2**53
    large_numeric["penalty_ns"] = 1
    large_numeric["benefit_minus_penalty_ns"] = 64 * 2**53 - 1
    large_numeric["gain_ns"] = 64 * 2**53 - 1 - large_numeric["buffer_delay_ns"]
    negatives.append(("integer-not-binary64", large_integer))
    huge_integer = copy.deepcopy(synthetic)
    huge_integer["generation_only"]["rounds"][0]["evaluations"][0]["numeric"]["candidate_load_pf"] = 10**400
    negatives.append(("integer-overflow-binary64", huge_integer))
    changed_top_k = copy.deepcopy(manifest)
    changed_top_k["profiles"][2]["top_k"] = 5
    _expect_rejected(
        "manifest-top-k", lambda: validate_document(
            synthetic, schema, changed_top_k))
    changed_comparability = copy.deepcopy(manifest)
    changed_comparability["profiles"][2]["comparability_status"] = "exact"
    _expect_rejected(
        "manifest-comparability", lambda: validate_document(
            synthetic, schema, changed_comparability))
    nonempty_template = copy.deepcopy(synthetic)
    _expect_rejected("nonempty-template", lambda: validate_document(nonempty_template, schema, manifest, template=True))
    for name, document in negatives:
        _expect_rejected(name, lambda document=document: validate_document(document, schema, manifest))
    for name, action in (
        ("duplicate-key", lambda: loads_json('{"replay": {}, "replay": {}}')),
        ("non-finite", lambda: loads_json('{"gain_ns": NaN}')),
    ):
        _expect_rejected(name, action)
    print(f"buffer fixed trace self-test: {9 + len(negatives) + 2} checks passed")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", type=Path, default=SCHEMA_PATH)
    parser.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    parser.add_argument("--trace", action="append", type=Path, default=[])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if not args.self_test and not args.trace:
        parser.error("provide --trace or use --self-test")
    try:
        schema = load_json(args.schema)
        manifest = load_json(args.manifest)
        if args.self_test:
            _run_self_test(schema, manifest)
        for trace_path in args.trace:
            validate_document(load_json(trace_path), schema, manifest)
            print(f"valid: {trace_path}")
    except (ValidationError, AssertionError) as error:
        print(f"validation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
