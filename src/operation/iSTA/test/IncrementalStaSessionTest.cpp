// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include <any>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>

#include "CsrSnapshotObservation.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "IncrementalStaSession.hpp"
#include "StaSessionTestSync.hpp"
#include "StaCsrSessionContext.hpp"

namespace {
using namespace ista;
using ista::test::await;
using ista::test::BoundedGate;

void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

ista::csr::StaAnalysisContext testAnalysisContext()
{
  return {"slow", "25", "__no_rc_corner__"};
}

std::unique_ptr<csr::CsrSnapshotObservation> makeObservation(std::uint64_t token, csr::CsrSnapshotState state)
{
  auto observation = std::make_unique<csr::CsrSnapshotObservation>();
  observation->state = state;
  observation->identity.session_token = token;
  observation->identity.topology_fingerprint.assign(64, 'a');
  observation->identity.clock_constraint_fingerprint.assign(64, 'b');
  observation->identity.library_fingerprint.assign(64, 'c');
  observation->identity.corner_fingerprint.assign(64, 'd');
  observation->identity.parasitic_fingerprint.assign(64, 'e');
  observation->identity.timing_input_fingerprint.assign(64, 'f');
  return observation;
}
class FakeOperations final : public StaSessionOperations
{
 public:
  StaStatus initialize(std::uint64_t token, std::map<std::string, std::any>&) override
  {
    ++initialize_calls;
    initialize_token = token;
    if (throw_initialize)
      throw std::runtime_error("injected initialize throw");
    return initialize_status;
  }
  StaStatus runFullBaseline(std::uint64_t token, StaCsrSessionContext* context) override
  {
    ++run_calls;
    run_token = token;
    received_context = context != nullptr;
    if (throw_run)
      throw std::runtime_error("injected run throw");
    if (context != nullptr) {
      require(context->token() == token, "fake run received a foreign context");
    }
    if (reentrant_session != nullptr)
      reentrant_close_status = reentrant_session->close();
    return run_status;
  }
  StaStatus close(std::uint64_t token, StaCsrSessionContext* context, std::unique_ptr<csr::CsrSnapshotObservation>* released_out) override
  {
    ++close_calls;
    close_token = token;
    if (close_observer)
      ++*close_observer;
    if (context != nullptr) {
      require(context->token() == token, "fake close received a foreign context");
      require(released_out != nullptr && !*released_out, "fake close received an invalid handoff out parameter");
      if (active_observation || handoff_on_close) {
        auto released = active_observation ? std::make_unique<csr::CsrSnapshotObservation>(*active_observation) : makeObservation(token, csr::CsrSnapshotState::kReady);
        released->state = csr::CsrSnapshotState::kReleased;
        *released_out = std::move(released);
      }
    }
    if (throw_close)
      throw std::runtime_error("injected close throw after handoff");
    return close_status;
  }
  StaStatus copyObservation(std::uint64_t token, StaCsrSessionContext* context, std::unique_ptr<csr::CsrSnapshotObservation>* detached_out) override
  {
    ++copy_calls;
    require(context != nullptr && context->token() == token, "fake copy received a foreign or null context");
    require(detached_out != nullptr && !*detached_out, "fake copy received an invalid out parameter");
    if (block_copy) {
      require(copy_entered != nullptr && copy_release != nullptr, "blocking copy gates were not installed");
      copy_entered->signal();
      copy_release->wait("blocking copy release");
    }
    if (!active_observation)
      active_observation = makeObservation(token, csr::CsrSnapshotState::kReady);
    *detached_out = std::make_unique<csr::CsrSnapshotObservation>(*active_observation);
    return copy_status;
  }
  StaCsrReferenceAttempt computeCsrReferenceForTesting(std::uint64_t, StaCsrSessionContext*) override { return {.value = StaCsrSessionOperationFailure{}}; }
  StaCsrScatterAttempt scatterCsrTimingResultForTesting(std::uint64_t, StaCsrSessionContext*, const csr::CsrTimingResult&) override
  {
    return {.value = csr::CsrTimingResultScatterAttempt{}};
  }
  int initialize_calls{0};
  int run_calls{0};
  int close_calls{0};
  int copy_calls{0};
  std::uint64_t initialize_token{0};
  std::uint64_t run_token{0};
  std::uint64_t close_token{0};
  bool received_context{false};
  bool throw_initialize{false};
  bool throw_run{false};
  bool handoff_on_close{false};
  int* close_observer{nullptr};
  bool throw_close{false};
  bool block_copy{false};
  BoundedGate* copy_entered{nullptr};
  BoundedGate* copy_release{nullptr};
  IncrementalStaSession* reentrant_session{nullptr};
  std::optional<StaStatus> reentrant_close_status;
  std::unique_ptr<csr::CsrSnapshotObservation> active_observation;
  StaStatus initialize_status = StaStatus::success(StaSessionState::kInitialized);
  StaStatus run_status = StaStatus::success(StaSessionState::kBaselineExecuted);
  StaStatus close_status = StaStatus::success(StaSessionState::kClosed);
  StaStatus copy_status = StaStatus::success(StaSessionState::kBaselineExecuted);
};

StaExecutionRequest fullRequest()
{
  return {StaBackend::kCpu, StaUpdateStrategy::kFull, StaFallbackPolicy::kError};
}

void testFullSessionAndRetry()
{
  auto operations = std::make_unique<FakeOperations>();
  FakeOperations* fake = operations.get();
  IncrementalStaSession session(fullRequest(), std::move(operations));

  const StaStatus invalid = session.initialize(csr::StaAnalysisContext{}, {});
  require(invalid.code == StaSessionStatusCode::kInitFailed && session.state() == StaSessionState::kCreated,
          "invalid context did not fail preflight in created state");
  require(!session.sessionToken().has_value() && fake->initialize_calls == 0, "invalid context allocated a token or called native initialize");

  require(session.initialize(testAnalysisContext(), {}).ok(), "valid retry after context failure did not initialize");
  const auto token = session.sessionToken();
  require(token.has_value() && *token != 0 && fake->initialize_token == *token, "initialized session token was not propagated");
  require(!session.snapshotObservation(), "full session exposed an observation before run");

  const StaExecutionResult result = session.runFullBaseline();
  require(result.ok() && fake->run_calls == 1 && fake->received_context == incregpusta_debug_checks_enabled,
          "full baseline context must follow the diagnostic build mode");
  require(fake->copy_calls == 0 && !fake->active_observation, "full baseline created an unrequested observation");
  require(session.close().ok() && session.close().ok(), "full session close was not idempotent");
  require(fake->close_calls == 1 && fake->close_token == *token, "full session close count or token drifted");
  require(session.sessionToken() == token, "readonly token changed after close");
}

void testDiagnosticObservationLifecycle()
{
  auto operations = std::make_unique<FakeOperations>();
  FakeOperations* fake = operations.get();
  IncrementalStaSession session(fullRequest(), std::move(operations));
  require(session.initialize(testAnalysisContext(), {}).ok(), "diagnostic session initialize failed");
  const std::uint64_t token = *session.sessionToken();
  require(session.runFullBaseline().ok(), "diagnostic session baseline failed");

  require(fake->copy_calls == 0 && !fake->active_observation, "baseline must not capture a diagnostic observation");
  if constexpr (!incregpusta_debug_checks_enabled) {
    require(!session.snapshotObservation() && fake->copy_calls == 0, "OFF getter constructed an observation");
    require(session.close().ok() && !session.snapshotObservation() && fake->copy_calls == 0,
            "OFF close constructed an observation");
    return;
  }
  auto ready = session.snapshotObservation();
  require(ready && ready->state == csr::CsrSnapshotState::kReady && ready->identity.session_token == token,
          "diagnostic getter did not return the ready observation");
  ready->identity.topology_fingerprint = "mutated-detached-value";
  auto stable = session.snapshotObservation();
  require(stable && stable->identity.topology_fingerprint == std::string(64, 'a'), "detached observation mutation leaked into session state");

  require(session.close().ok(), "diagnostic close failed");
  auto released = session.snapshotObservation();
  require(released && released->state == csr::CsrSnapshotState::kReleased && released->identity.session_token == token,
          "closed getter did not return the released cache");
  released->state = csr::CsrSnapshotState::kReady;
  require(session.snapshotObservation()->state == csr::CsrSnapshotState::kReleased, "released getter did not return a detached copy");
  require(session.close().ok() && fake->close_calls == 1, "diagnostic double close retried cleanup");
}

void testHandoffSurvivesCloseThrow()
{
  auto operations = std::make_unique<FakeOperations>();
  FakeOperations* fake = operations.get();
  fake->throw_close = true;
  IncrementalStaSession session(fullRequest(), std::move(operations));
  require(session.initialize(testAnalysisContext(), {}).ok() && session.runFullBaseline().ok(), "handoff fixture setup failed");

  require(bool(session.snapshotObservation()) == incregpusta_debug_checks_enabled, "explicit observation request disagrees with build mode");
  const StaStatus close = session.close();
  require(close.code == StaSessionStatusCode::kCleanupFailed && session.state() == StaSessionState::kClosed,
          "throwing close did not report cleanup failure and close the session");
  auto released = session.snapshotObservation();
  require(incregpusta_debug_checks_enabled ? released && released->state == csr::CsrSnapshotState::kReleased : !released,
          "close throw returned the wrong diagnostic observation state");
  require(session.close().ok() && fake->close_calls == 1, "close throw was retried");
}

void testSameSessionReentrancy()
{
  auto operations = std::make_unique<FakeOperations>();
  FakeOperations* fake = operations.get();
  IncrementalStaSession session(fullRequest(), std::move(operations));
  fake->reentrant_session = &session;
  require(session.initialize(testAnalysisContext(), {}).ok(), "reentrant fixture initialize failed");
  require(session.runFullBaseline().ok(), "outer baseline failed");
  require(fake->reentrant_close_status.has_value() && fake->reentrant_close_status->code == StaSessionStatusCode::kInvalidState,
          "same-session reentrant close was not rejected");
  require(fake->close_calls == 0, "reentrant close entered native operations");
  require(session.close().ok() && fake->close_calls == 1, "normal close after reentrant rejection failed");
}

void testGetterVersusClose()
{
  auto operations = std::make_unique<FakeOperations>();
  FakeOperations* fake = operations.get();
  IncrementalStaSession session(fullRequest(), std::move(operations));
  require(session.initialize(testAnalysisContext(), {}).ok() && session.runFullBaseline().ok(), "concurrency fixture setup failed");

  if constexpr (!incregpusta_debug_checks_enabled) {
    auto reader = std::async(std::launch::async, [&] { return session.snapshotObservation(); });
    auto closer = std::async(std::launch::async, [&] { return session.close(); });
    const auto observation = await(reader, "OFF getter");
    const auto closed = await(closer, "OFF close");
    require(!observation && closed.ok() && fake->copy_calls == 0,
            "OFF getter/close performed diagnostic copying");
    return;
  }
  BoundedGate copy_entered;
  BoundedGate copy_release;
  fake->block_copy = true;
  fake->copy_entered = &copy_entered;
  fake->copy_release = &copy_release;

  auto reader = std::async(std::launch::async, [&] { return session.snapshotObservation(); });
  copy_entered.wait("getter entry");

  auto closer = std::async(std::launch::async, [&] { return session.close(); });
  copy_release.signal();
  auto read_result = await(reader, "getter versus close read");
  const StaStatus close_result = await(closer, "getter versus close cleanup");

  require(read_result && read_result->state == csr::CsrSnapshotState::kReady, "getter did not return a complete pre-close observation");
  require(close_result.ok(), "concurrent close failed");
  auto released = session.snapshotObservation();
  require(released && released->state == csr::CsrSnapshotState::kReleased, "post-close getter did not return released cache");
  require(fake->close_calls == 1, "concurrent close executed cleanup more than once");
}

}  // namespace

int main()
{
  testFullSessionAndRetry();
  testDiagnosticObservationLifecycle();
  testHandoffSurvivesCloseThrow();
  testSameSessionReentrancy();
  testGetterVersusClose();
  using namespace ista;
  StaExecutionRequest full_request{};
  auto full_operations = std::make_unique<FakeOperations>();
  FakeOperations* full_fake = full_operations.get();
  IncrementalStaSession full_session(full_request, std::move(full_operations));
  require(full_session.initialize(testAnalysisContext(), {}).ok(), "initialize");
  require(full_session.state() == StaSessionState::kInitialized, "initialized state");
  StaExecutionResult full = full_session.runFullBaseline();
  require(full.ok() && full.record->execution_kind == StaExecutionKind::kFullUpdate, "full record");
  require(full_session.runFullBaseline().status.code == StaSessionStatusCode::kInvalidState, "single baseline only");
  require(full_session.close().ok() && full_session.close().ok(), "idempotent close");
  require(full_fake->initialize_calls == 1 && full_fake->run_calls == 1 && full_fake->close_calls == 1, "exact operation counts");

  for (const auto request : {StaExecutionRequest{StaBackend::kCpu, StaUpdateStrategy::kIncremental, StaFallbackPolicy::kError},
                             StaExecutionRequest{StaBackend::kCuda, StaUpdateStrategy::kFull, StaFallbackPolicy::kError}}) {
    auto operations = std::make_unique<FakeOperations>();
    auto* fake = operations.get();
    IncrementalStaSession session(request, std::move(operations));
    const auto status = session.initialize(testAnalysisContext(), {});
    require(status.code == StaSessionStatusCode::kUnsupported
                && status.capability_reason == StaCapabilityReason::kUnsupportedExecutionMode
                && !session.sessionToken() && fake->initialize_calls == 0 && fake->run_calls == 0,
            "retired mode must fail before token allocation or native work");
  }

  auto failed_operations = std::make_unique<FakeOperations>();
  FakeOperations* failed_fake = failed_operations.get();
  failed_fake->run_status = StaStatus::failure(StaSessionStatusCode::kRunFailed, StaSessionState::kInitialized, "injected run failure");
  IncrementalStaSession failed_session(full_request, std::move(failed_operations));
  require(failed_session.initialize(testAnalysisContext(), {}).ok(), "failed-session initialize");
  require(!failed_session.runFullBaseline().ok() && failed_session.state() == StaSessionState::kPoisoned, "poisoned after run failure");
  require(failed_session.close().ok() && failed_fake->close_calls == 1, "poisoned session still closes");

  auto init_failure_operations = std::make_unique<FakeOperations>();
  FakeOperations* init_failure_fake = init_failure_operations.get();
  init_failure_fake->initialize_status = StaStatus::failure(StaSessionStatusCode::kInitFailed, StaSessionState::kCreated, "injected init failure");
  IncrementalStaSession init_failure_session(full_request, std::move(init_failure_operations));
  require(!init_failure_session.initialize(testAnalysisContext(), {}).ok() && init_failure_session.state() == StaSessionState::kClosed,
          "init failure closes handle");
  require(init_failure_session.close().ok() && init_failure_fake->close_calls == 0, "failed init has no second cleanup attempt");

  if constexpr (!incregpusta_cuda_enabled) {
    auto cuda_operations = std::make_unique<FakeOperations>();
    FakeOperations* cuda_fake = cuda_operations.get();
    IncrementalStaSession cuda_session({StaBackend::kCuda, StaUpdateStrategy::kIncremental, StaFallbackPolicy::kFullWithRecord}, std::move(cuda_operations));
    StaStatus unsupported = cuda_session.initialize(testAnalysisContext(), {});
    require(unsupported.code == StaSessionStatusCode::kUnsupported && cuda_fake->initialize_calls == 0, "unsupported before native init");
    require(cuda_session.state() == StaSessionState::kClosed, "unsupported closes handle");
  }

  if constexpr (incregpusta_cuda_enabled) {
      for (bool fail : {false, true}) {
        auto operations = std::make_unique<FakeOperations>();
        auto* fake = operations.get();
        fake->throw_run = fail;
        IncrementalStaSession session({StaBackend::kCuda, StaUpdateStrategy::kIncremental, StaFallbackPolicy::kError}, std::move(operations));
        require(session.initialize(testAnalysisContext(), {}).ok(), "CUDA baseline initializes on an available device");
        const auto result = session.runFullBaseline();
        require(fake->received_context, "CUDA baseline receives its owned session context");
        if (fail) {
          require(!result.ok() && session.state() == StaSessionState::kPoisoned, "CUDA run failure poisons the session");
          require(session.runFullBaseline().status.code == StaSessionStatusCode::kInvalidState, "poisoned CUDA session cannot run again");
        } else {
          require(
              result.ok() && result.record->executed_backend == StaBackend::kCuda
                  && result.record->execution_kind == StaExecutionKind::kBaselineOnly
                  && !result.record->fallback_to_full,
              "CUDA baseline records the actual backend without fallback");
        }
        require(session.close().ok() && fake->close_calls == 1, "CUDA closes after success or failure");
      }
  }

  auto created_operations = std::make_unique<FakeOperations>();
  FakeOperations* created_fake = created_operations.get();
  IncrementalStaSession created_session(full_request, std::move(created_operations));
  require(created_session.close().ok() && created_fake->close_calls == 0, "created close is side-effect free");

  auto init_throw_operations = std::make_unique<FakeOperations>();
  FakeOperations* init_throw_fake = init_throw_operations.get();
  init_throw_fake->throw_initialize = true;
  IncrementalStaSession init_throw_session(full_request, std::move(init_throw_operations));
  StaStatus init_throw = init_throw_session.initialize(testAnalysisContext(), {});
  require(init_throw.code == StaSessionStatusCode::kInitFailed, "initialize throw becomes typed init failure");
  require(init_throw_session.state() == StaSessionState::kClosed && init_throw_fake->close_calls == 1, "initialize throw performs one cleanup attempt");

  auto double_throw_operations = std::make_unique<FakeOperations>();
  FakeOperations* double_throw_fake = double_throw_operations.get();
  double_throw_fake->throw_initialize = true;
  double_throw_fake->throw_close = true;
  double_throw_fake->handoff_on_close = true;
  IncrementalStaSession double_throw_session(full_request, std::move(double_throw_operations));
  StaStatus double_throw = double_throw_session.initialize(testAnalysisContext(), {});
  require(double_throw.code == StaSessionStatusCode::kCleanupFailed, "initialize plus close throw reports cleanup failure");
  auto double_throw_released = double_throw_session.snapshotObservation();
  require(incregpusta_debug_checks_enabled
              ? double_throw_released && double_throw_released->state == csr::CsrSnapshotState::kReleased && double_throw_session.sessionToken().has_value()
                    && double_throw_released->identity.session_token == *double_throw_session.sessionToken()
              : !double_throw_released,
          "initialize cleanup throw returned the wrong diagnostic handoff");
  require(double_throw_session.close().ok() && double_throw_fake->close_calls == 1, "failed initialize consumes its cleanup attempt");

  auto run_throw_operations = std::make_unique<FakeOperations>();
  FakeOperations* run_throw_fake = run_throw_operations.get();
  run_throw_fake->throw_run = true;
  IncrementalStaSession run_throw_session(full_request, std::move(run_throw_operations));
  require(run_throw_session.initialize(testAnalysisContext(), {}).ok(), "run-throw initialize");
  StaExecutionResult run_throw = run_throw_session.runFullBaseline();
  require(run_throw.status.code == StaSessionStatusCode::kRunFailed && run_throw_session.state() == StaSessionState::kPoisoned, "run throw poisons session");
  require(run_throw_session.close().ok() && run_throw_fake->close_calls == 1, "run throw remains explicitly closeable");

  int destructor_close_calls = 0;
  const std::ios::iostate previous_exceptions = std::cerr.exceptions();
  const std::ios::iostate previous_state = std::cerr.rdstate();
  std::cerr.setstate(std::ios::badbit);
  try {
    std::cerr.exceptions(std::ios::badbit);
  } catch (const std::ios_base::failure&) {
  }
  {
    auto destructor_operations = std::make_unique<FakeOperations>();
    destructor_operations->throw_close = true;
    destructor_operations->close_observer = &destructor_close_calls;
    IncrementalStaSession destructor_session(full_request, std::move(destructor_operations));
    require(destructor_session.initialize(testAnalysisContext(), {}).ok(), "destructor-throw initialize");
  }
  std::cerr.exceptions(std::ios::goodbit);
  std::cerr.clear(previous_state);
  std::cerr.exceptions(previous_exceptions);
  require(destructor_close_calls == 1, "noexcept destructor attempts throwing close exactly once");
  return 0;
}
