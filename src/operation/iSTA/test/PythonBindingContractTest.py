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

import argparse
import itertools

import ecc_py

RECORD_FIELDS = {
    "requested_backend",
    "requested_update_strategy",
    "fallback_policy",
    "executed_backend",
    "executed_update_strategy",
    "execution_kind",
    "propagation_scope",
    "incremental_batch_executed",
    "fallback_to_full",
    "fallback_reason",
}

VALID_CONTEXT = {
    "liberty_corner": "slow",
    "temperature": "25",
    "rc_corner": "__no_rc_corner__",
}


def make_request(backend, update_strategy, fallback_policy):
    return {
        "backend": backend,
        "update_strategy": update_strategy,
        "fallback_policy": fallback_policy,
    }


def require_sta_error(callable_, request, state):
    try:
        callable_()
    except ecc_py.StaSessionError as error:
        assert error.status_code == "unsupported"
        assert error.capability_reason == "backend_unavailable"
        assert error.state == state
        assert error.request == request
        return
    raise AssertionError("expected StaSessionError")


def check_record(record, request):
    assert len(record) == 10
    assert set(record) == RECORD_FIELDS
    assert record["requested_backend"] == request["backend"]
    assert record["requested_update_strategy"] == request["update_strategy"]
    assert record["fallback_policy"] == request["fallback_policy"]
    assert record["executed_backend"] == request["backend"]
    assert record["executed_update_strategy"] == "full"
    assert record["propagation_scope"] == "full_graph"
    assert record["incremental_batch_executed"] is False
    assert record["fallback_to_full"] is False
    if request["update_strategy"] == "full":
        assert record["execution_kind"] == "full_update"
        assert record["fallback_reason"] is None
    else:
        assert record["execution_kind"] == "baseline_only"
        assert record["fallback_reason"] == "not_applicable_no_mutation"


def check_matrix(cuda_available):
    for backend, strategy, policy in itertools.product(
        ("cpu", "cuda"),
        ("full", "incremental"),
        ("error", "full_with_record"),
    ):
        request = make_request(backend, strategy, policy)
        if backend == "cpu" or cuda_available:
            check_record(ecc_py.resolve_sta_execution(request), request)
        else:
            require_sta_error(
                lambda request=request: ecc_py.resolve_sta_execution(request),
                request,
                "created",
            )
        session = ecc_py.create_incremental_sta_session(request)
        assert session.state == "created"
        session.close()
        session.close()
        assert session.state == "closed"


def check_initialize_error(cuda_available):
    request = make_request("cuda", "full", "error")
    session = ecc_py.create_incremental_sta_session(request)
    assert session.state == "created"
    assert session.session_token is None
    assert session.csr_snapshot_observation is None

    invalid_context = dict(VALID_CONTEXT)
    invalid_context["liberty_corner"] = ""
    try:
        session.initialize(
            analysis_context=invalid_context,
            config="/definitely/missing/step7-sta-config.json",
            config_dict={},
        )
    except ValueError:
        pass
    else:
        raise AssertionError("invalid analysis context accepted")
    assert session.state == "created"
    assert session.session_token is None
    assert session.csr_snapshot_observation is None

    if cuda_available:
        session.close()
    else:
        require_sta_error(
            lambda: session.initialize(
                analysis_context=VALID_CONTEXT,
                config="",
                config_dict={},
            ),
            request,
            "closed",
        )
    assert session.state == "closed"
    assert session.session_token is None
    assert session.csr_snapshot_observation is None
    session.close()


def check_invalid_requests():
    valid = make_request("cpu", "full", "error")
    invalid = [
        {"backend": "cpu", "update_strategy": "full"},
        {**valid, "extra": "forbidden"},
        {**valid, "backend": "CPU"},
        {**valid, "update_strategy": "increment"},
        {**valid, "fallback_policy": "full"},
    ]
    for request in invalid:
        for callable_ in (
            lambda request=request: ecc_py.resolve_sta_execution(request),
            lambda request=request: ecc_py.create_incremental_sta_session(request),
        ):
            try:
                callable_()
            except ValueError:
                continue
            raise AssertionError(f"invalid request accepted: {request}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Real native mode and lifecycle contract")
    parser.add_argument("--expect-cuda", type=int, choices=(0, 1), required=True)
    parser.add_argument("--expect-debug-checks", type=int, choices=(0, 1), required=True)
    parser.add_argument("--expect-unavailable", action="store_true")
    args = parser.parse_args()
    assert ecc_py.incregpusta_cuda_enabled is bool(args.expect_cuda)
    assert ecc_py.incregpusta_debug_checks_enabled is bool(args.expect_debug_checks)
    cuda_available = ecc_py.incregpusta_cuda_enabled and not args.expect_unavailable
    check_matrix(cuda_available)
    check_initialize_error(cuda_available)
    check_invalid_requests()
    print("iSTA Python binding contract passed")
