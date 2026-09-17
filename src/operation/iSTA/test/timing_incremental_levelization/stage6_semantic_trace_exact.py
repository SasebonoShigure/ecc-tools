#!/usr/bin/env python3
"""Exact Stage 6 binding for the persisted normalized semantic plan."""

import copy
from collections.abc import Callable
from typing import Any

from dual_csr_transaction_validator import normalize_plan


def validate_exact_semantic_trace(value: dict[str, Any],
                                  batches: list[dict[str, Any]],
                                  exact_difference: Callable,
                                  require: Callable,
                                  label: str) -> None:
    transaction = value.get("transaction_receipt")
    require(isinstance(transaction, dict),
            f"{label}.transaction is absent")
    semantic = transaction.get("semantic_trace")
    require(isinstance(semantic, list) and len(semantic) == len(batches),
            f"{label}.semantic_trace cardinality differs")
    expected = [normalize_plan(batch) for batch in batches]
    difference = exact_difference(
        semantic, expected, f"{label}.transaction.semantic_trace")
    require(difference is None,
            f"{label} exact normalized semantic trace differs: {difference}")


def run_semantic_trace_exact_canary(exact_difference: Callable,
                                    tolerant_difference: Callable,
                                    require: Callable,
                                    error_type: type[Exception],
                                    tamper_delta: float) -> None:
    batch = {
        "baseline": {"canonical_pre_state": {
            "arcs": [{"delay": 1.0}],
        }},
        "edits": [{"rc_inputs": {"kind": "no_parasitic"}}],
    }
    expected = normalize_plan(batch)
    value = {"transaction_receipt": {
        "semantic_trace": [expected],
    }}
    validate_exact_semantic_trace(
        value, [batch], exact_difference, require, "semantic-canary")
    tampered = copy.deepcopy(value)
    tampered["transaction_receipt"]["semantic_trace"][0][
        "baseline"]["canonical_pre_state"]["active_arcs"][0][
            "delay"] += tamper_delta
    mismatch = tolerant_difference(
        tampered["transaction_receipt"]["semantic_trace"],
        [expected], "semantic-canary.tolerant")
    require(mismatch is None,
            "semantic float canary must be below Stage2 timing tolerance")
    try:
        validate_exact_semantic_trace(
            tampered, [batch], exact_difference, require,
            "semantic-float-tamper")
    except error_type:
        return
    raise error_type("semantic float tamper was accepted by exact validation")
