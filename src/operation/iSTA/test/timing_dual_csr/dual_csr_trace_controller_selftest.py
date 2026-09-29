#!/usr/bin/env python3
"""Executable canaries for the Stage 5 Dual-CSR trace controller."""

import json
from collections.abc import Callable

from dual_csr_timing_evidence import run_timing_evidence_self_test
from dual_csr_transaction_validator import (
    normalize_plan,
    validate_identity_sequences,
    validate_transaction,
    validate_traversal_counts,
)
from dual_csr_typed_json_validator import run_typed_validator_self_test

Require = Callable[[bool, str], None]
Difference = Callable[[object, object, str], str | None]


def run_controller_self_test(require: Require, difference: Difference,
                             error_type: type[Exception]) -> None:
    plan = {
        "baseline": {"canonical_pre_state": {"arcs": [{"key": "a"}], "nodes": []}},
        "edits": [{"rc_inputs": {"kind": "no_parasitic"}, "token": [1, 2]},
                  {"rc_inputs": {"kind": "extracted", "parasitic_policy": "keep"}}],
    }
    before = json.dumps(plan, sort_keys=True)
    normalized = normalize_plan(plan)
    require(normalized == {
        "baseline": {"canonical_pre_state": {"active_arcs": [{"key": "a"}], "nodes": []}},
        "edits": [{"rc_inputs": {"kind": "none"}, "token": [1, 2]},
                  {"rc_inputs": {"kind": "parasitic", "policy": "keep"}}],
    }, "semantic plan normalization changed unrelated fields")
    require(json.dumps(plan, sort_keys=True) == before,
            "semantic plan normalization mutated the source")
    equal = {"x": [{"kind": "finite", "value": 1.0,
                     "negative_zero": False}]}
    within = {"x": [{"kind": "finite", "value": 1.0 + 5.0e-10,
                      "negative_zero": False}]}
    require(difference(equal, within, "$") is None,
            "numeric tolerance rejected an in-range value")
    outside = json.loads(json.dumps(within))
    outside["x"][0]["value"] = 1.0 + 3.0e-9
    require(difference(equal, outside, "$")
            == "$.x[0].value: 1.0 != 1.000000003",
            "field comparator did not locate an out-of-range leaf")
    changed_presence = json.loads(json.dumps(equal))
    changed_presence["x"][0]["negative_zero"] = True
    require(difference(equal, changed_presence, "$")
            == "$.x[0].negative_zero: False != True",
            "structural boolean mismatch was hidden by numeric tolerance")
    level_nodes = {"level_nodes": [["u0:A", "u0:B"]]}
    swapped_order = {"level_nodes": [["u0:B", "u0:A"]]}
    require(difference(level_nodes, swapped_order, "$")
            == "$.level_nodes[0][0]: 'u0:A' != 'u0:B'",
            "same-level timing-order swap was hidden by the comparator")
    same_size_semantic_edit = {"edits": [{"original_net": "n0"}]}
    tampered_semantic_edit = {"edits": [{"original_net": "n1"}]}
    require(difference(same_size_semantic_edit, tampered_semantic_edit, "$")
            is not None, "same-size semantic edit tamper was hidden")
    try:
        validate_transaction(
            {}, 1, {}, {}, [], "signature-canary", require, difference,
        )
    except error_type:
        pass
    except TypeError as error:
        raise AssertionError(f"transaction validator call signature drifted: {error}") from error
    else:
        raise AssertionError("malformed transaction signature canary was accepted")

    port = {"kind": "port", "full_name": "very_long_port_name",
            "instance_name": "", "pin_name": ""}
    pin = {"kind": "instance_pin", "full_name": "a:A",
           "instance_name": "a", "pin_name": "A"}
    arc = {"source": port, "sink": pin, "kind": "net", "owner_name": "n",
           "library_source_port": "", "library_sink_port": "",
           "library_arcs": [], "parallel_ordinal": 0}
    ordering_errors: list[str] = []
    validate_identity_sequences(
        [port, pin], [arc], [pin, port], [arc], "identity-canary",
        lambda ok, message: ordering_errors.append(message) if not ok else None)
    require(not ordering_errors,
            "typed canonical identity was rejected by generic JSON ordering")
    duplicate_errors: list[str] = []
    validate_identity_sequences(
        [port, port], [arc], [port, port], [arc],
        "identity-duplicate-canary",
        lambda ok, message: duplicate_errors.append(message) if not ok else None)
    require(duplicate_errors, "duplicate semantic identity was accepted")

    structural_visits = {
        "base_out": {"physical_slots": [0, None]},
        "aux_out": {"row_arcs": [[1]]},
    }
    valid_visits = {
        "levelization": {"base_out": 1, "aux_out": 1, "base_in": 2},
        "arc_initialization": {"base_out": 1, "aux_out": 1, "base_in": 0},
        "forward_propagation": {"base_out": 0, "aux_out": 0, "base_in": 4},
        "required_propagation": {"base_out": 1, "aux_out": 1, "base_in": 0},
    }
    traversal_errors: list[str] = []
    validate_traversal_counts(
        valid_visits, structural_visits, "full_stage5", "traversal-canary",
        lambda ok, message: traversal_errors.append(message) if not ok else None)
    require(not traversal_errors, "valid structural traversal was rejected")
    mixed_visits = {stage: dict(visits)
                    for stage, visits in valid_visits.items()}
    mixed_visits["levelization"] = {
        "base_out": 0, "aux_out": 0, "base_in": 0}
    mixed_errors: list[str] = []
    validate_traversal_counts(
        mixed_visits, structural_visits, "mixed_stage6",
        "mixed-traversal-canary",
        lambda ok, message: mixed_errors.append(message) if not ok else None)
    require(not mixed_errors, "valid mixed traversal was rejected")
    errors: list[str] = []

    def collect_error(ok: object, message: str) -> None:
        if not ok:
            errors.append(message)

    for name, tampered in (
        ("all-zero", {stage: {key: 0 for key in visits}
                      for stage, visits in valid_visits.items()}),
        ("plus-one", {stage: dict(visits)
                      for stage, visits in valid_visits.items()}),
    ):
        if name == "plus-one":
            tampered["forward_propagation"]["base_in"] += 1
        errors.clear()
        validate_traversal_counts(
            tampered, structural_visits, "full_stage5",
            f"traversal-{name}-canary",
            collect_error)
        require(errors, f"{name} traversal tamper was accepted")
    mixed_full_errors: list[str] = []
    validate_traversal_counts(
        valid_visits, structural_visits, "mixed_stage6",
        "mixed-full-levelizer-canary",
        lambda ok, message:
            mixed_full_errors.append(message) if not ok else None)
    require(mixed_full_errors, "mixed mode accepted full-levelizer visits")

    run_timing_evidence_self_test(require)
    run_typed_validator_self_test(require)
