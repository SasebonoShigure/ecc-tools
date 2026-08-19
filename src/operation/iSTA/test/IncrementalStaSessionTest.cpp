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
#include <iostream>
#include <memory>
#include <stdexcept>

#include "IncrementalStaSession.hpp"

namespace {

void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

class FakeOperations final : public ista::StaSessionOperations
{
 public:
  ista::StaStatus initialize(uint64_t, std::map<std::string, std::any>&) override
  {
    ++initialize_calls;
    if (throw_initialize)
      throw std::runtime_error("injected initialize throw");
    return initialize_status;
  }
  ista::StaStatus runFullBaseline(uint64_t) override
  {
    ++run_calls;
    if (throw_run)
      throw std::runtime_error("injected run throw");
    return run_status;
  }
  ista::StaStatus close(uint64_t) override
  {
    ++close_calls;
    if (close_observer)
      ++*close_observer;
    if (throw_close)
      throw std::runtime_error("injected close throw");
    return close_status;
  }

  int initialize_calls = 0;
  int run_calls = 0;
  int close_calls = 0;
  int* close_observer = nullptr;
  bool throw_initialize = false;
  bool throw_run = false;
  bool throw_close = false;
  ista::StaStatus initialize_status = ista::StaStatus::success(ista::StaSessionState::kInitialized);
  ista::StaStatus run_status = ista::StaStatus::success(ista::StaSessionState::kBaselineExecuted);
  ista::StaStatus close_status = ista::StaStatus::success(ista::StaSessionState::kClosed);
};

}  // namespace

int main()
{
  using namespace ista;
  StaExecutionRequest full_request{};
  auto full_operations = std::make_unique<FakeOperations>();
  FakeOperations* full_fake = full_operations.get();
  IncrementalStaSession full_session(full_request, std::move(full_operations));
  require(full_session.initialize({}).ok(), "initialize");
  require(full_session.state() == StaSessionState::kInitialized, "initialized state");
  StaExecutionResult full = full_session.runFullBaseline();
  require(full.ok() && full.record->execution_kind == StaExecutionKind::kFullUpdate, "full record");
  require(full_session.runFullBaseline().status.code == StaSessionStatusCode::kInvalidState, "single baseline only");
  require(full_session.close().ok() && full_session.close().ok(), "idempotent close");
  require(full_fake->initialize_calls == 1 && full_fake->run_calls == 1 && full_fake->close_calls == 1, "exact operation counts");

  StaExecutionRequest incremental_request{StaBackend::kCpu, StaUpdateStrategy::kIncremental, StaFallbackPolicy::kError};
  auto incremental_operations = std::make_unique<FakeOperations>();
  IncrementalStaSession incremental_session(incremental_request, std::move(incremental_operations));
  require(incremental_session.initialize({}).ok(), "incremental initialize");
  StaExecutionResult baseline = incremental_session.runFullBaseline();
  require(baseline.ok() && baseline.record->execution_kind == StaExecutionKind::kBaselineOnly, "baseline-only record");
  require(!baseline.record->fallback_to_full, "baseline-only is not fallback");
  require(incremental_session.close().ok(), "incremental close");

  auto failed_operations = std::make_unique<FakeOperations>();
  FakeOperations* failed_fake = failed_operations.get();
  failed_fake->run_status = StaStatus::failure(StaSessionStatusCode::kRunFailed, StaSessionState::kInitialized, "injected run failure");
  IncrementalStaSession failed_session(full_request, std::move(failed_operations));
  require(failed_session.initialize({}).ok(), "failed-session initialize");
  require(!failed_session.runFullBaseline().ok() && failed_session.state() == StaSessionState::kPoisoned, "poisoned after run failure");
  require(failed_session.close().ok() && failed_fake->close_calls == 1, "poisoned session still closes");

  auto init_failure_operations = std::make_unique<FakeOperations>();
  FakeOperations* init_failure_fake = init_failure_operations.get();
  init_failure_fake->initialize_status = StaStatus::failure(StaSessionStatusCode::kInitFailed, StaSessionState::kCreated, "injected init failure");
  IncrementalStaSession init_failure_session(full_request, std::move(init_failure_operations));
  require(!init_failure_session.initialize({}).ok() && init_failure_session.state() == StaSessionState::kClosed, "init failure closes handle");
  require(init_failure_session.close().ok() && init_failure_fake->close_calls == 0, "failed init has no second cleanup attempt");

  auto cuda_operations = std::make_unique<FakeOperations>();
  FakeOperations* cuda_fake = cuda_operations.get();
  IncrementalStaSession cuda_session({StaBackend::kCuda, StaUpdateStrategy::kFull, StaFallbackPolicy::kFullWithRecord}, std::move(cuda_operations));
  StaStatus unsupported = cuda_session.initialize({});
  require(unsupported.code == StaSessionStatusCode::kUnsupported && cuda_fake->initialize_calls == 0, "unsupported before native init");
  require(cuda_session.state() == StaSessionState::kClosed, "unsupported closes handle");

  auto created_operations = std::make_unique<FakeOperations>();
  FakeOperations* created_fake = created_operations.get();
  IncrementalStaSession created_session(full_request, std::move(created_operations));
  require(created_session.close().ok() && created_fake->close_calls == 0, "created close is side-effect free");

  auto init_throw_operations = std::make_unique<FakeOperations>();
  FakeOperations* init_throw_fake = init_throw_operations.get();
  init_throw_fake->throw_initialize = true;
  IncrementalStaSession init_throw_session(full_request, std::move(init_throw_operations));
  StaStatus init_throw = init_throw_session.initialize({});
  require(init_throw.code == StaSessionStatusCode::kInitFailed, "initialize throw becomes typed init failure");
  require(init_throw_session.state() == StaSessionState::kClosed && init_throw_fake->close_calls == 1, "initialize throw performs one cleanup attempt");

  auto double_throw_operations = std::make_unique<FakeOperations>();
  FakeOperations* double_throw_fake = double_throw_operations.get();
  double_throw_fake->throw_initialize = true;
  double_throw_fake->throw_close = true;
  IncrementalStaSession double_throw_session(full_request, std::move(double_throw_operations));
  StaStatus double_throw = double_throw_session.initialize({});
  require(double_throw.code == StaSessionStatusCode::kCleanupFailed, "initialize plus close throw reports cleanup failure");
  require(double_throw_session.close().ok() && double_throw_fake->close_calls == 1, "failed initialize consumes its cleanup attempt");

  auto run_throw_operations = std::make_unique<FakeOperations>();
  FakeOperations* run_throw_fake = run_throw_operations.get();
  run_throw_fake->throw_run = true;
  IncrementalStaSession run_throw_session(full_request, std::move(run_throw_operations));
  require(run_throw_session.initialize({}).ok(), "run-throw initialize");
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
    require(destructor_session.initialize({}).ok(), "destructor-throw initialize");
  }
  std::cerr.exceptions(std::ios::goodbit);
  std::cerr.clear(previous_state);
  std::cerr.exceptions(previous_exceptions);
  require(destructor_close_calls == 1, "noexcept destructor attempts throwing close exactly once");
  return 0;
}
