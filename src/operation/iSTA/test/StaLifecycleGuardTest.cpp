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
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "StaLifecycleGuard.hpp"

namespace {

void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<ista::StaLifecycleStage> makeStages(const std::vector<std::string>& names, std::vector<std::string>& calls, int fail_initialize = -1,
                                                int fail_work = -1, int fail_cleanup = -1)
{
  std::vector<ista::StaLifecycleStage> stages;
  for (int index = 0; index < static_cast<int>(names.size()); ++index) {
    const std::string name = names[index];
    stages.push_back({name,
                      [&, index, name, fail_initialize] {
                        calls.push_back(name + ".init");
                        if (index == fail_initialize)
                          throw std::runtime_error("injected init failure");
                      },
                      [&, index, name, fail_work] {
                        calls.push_back(name + ".work");
                        if (index == fail_work)
                          throw std::runtime_error("injected work failure");
                      },
                      [&, index, name, fail_cleanup] {
                        calls.push_back(name + ".cleanup");
                        if (index == fail_cleanup)
                          throw std::runtime_error("injected cleanup failure");
                      }});
  }
  return stages;
}

}  // namespace

int main()
{
  using namespace ista;
  std::vector<int> calls;
  StaLifecycleGuard guard;
  guard.add("first", [&] { calls.push_back(1); });
  guard.add("second", [&] {
    calls.push_back(2);
    throw std::runtime_error("injected cleanup failure");
  });
  guard.add("third", [&] { calls.push_back(3); });
  StaOperationResult cleanup = guard.cleanup();
  require(!cleanup.succeeded && cleanup.cleanup_failed, "cleanup failure must be classified");
  require(calls == std::vector<int>({3, 2, 1}), "all cleanup callbacks must run in reverse order");
  require(guard.cleanup().succeeded, "cleanup must be consumed exactly once");

  const std::ios::iostate previous_exceptions = std::cerr.exceptions();
  const std::ios::iostate previous_state = std::cerr.rdstate();
  std::cerr.setstate(std::ios::badbit);
  try {
    std::cerr.exceptions(std::ios::badbit);
  } catch (const std::ios_base::failure&) {
  }
  int destructor_cleanup_calls = 0;
  {
    StaLifecycleGuard destructor_guard;
    destructor_guard.add("throwing diagnostic", [&] {
      ++destructor_cleanup_calls;
      throw std::runtime_error("injected destructor cleanup failure");
    });
  }
  std::cerr.exceptions(std::ios::goodbit);
  std::cerr.clear(previous_state);
  std::cerr.exceptions(previous_exceptions);
  require(destructor_cleanup_calls == 1, "guard destructor remains no-throw when cerr throws");

  bool cleanup_called = false;
  StaOperationResult work_failure
      = executeStaLifecycleStage("injected", [] {}, [] { throw std::runtime_error("work failure"); }, [&] { cleanup_called = true; });
  require(!work_failure.succeeded && !work_failure.cleanup_failed, "work failure classification");
  require(cleanup_called, "work failure must clean initialized stage");

  cleanup_called = false;
  StaOperationResult init_failure
      = executeStaLifecycleStage("injected", [] { throw std::runtime_error("init failure"); }, [] {}, [&] { cleanup_called = true; });
  require(!init_failure.succeeded && !init_failure.cleanup_failed, "init failure classification");
  require(!cleanup_called, "failed initializer must not clean an unacquired stage");
  StaOperationResult work_cleanup_failure
      = executeStaLifecycleStage("injected", [] {}, [] { throw std::runtime_error("work failure"); }, [] { throw std::runtime_error("cleanup failure"); });
  require(!work_cleanup_failure.succeeded && work_cleanup_failure.cleanup_failed, "armed work cleanup failure classification");

  const std::vector<std::string> persistent_names{"Logger", "DataManager", "DelayCalculator"};
  for (int failure = 0; failure < static_cast<int>(persistent_names.size()); ++failure) {
    for (bool fail_during_initialize : {true, false}) {
      std::vector<std::string> persistent_calls;
      StaOperationResult persistent_failure = executeStaPersistentInitialization(
          makeStages(persistent_names, persistent_calls, fail_during_initialize ? failure : -1, fail_during_initialize ? -1 : failure));
      require(!persistent_failure.succeeded && !persistent_failure.cleanup_failed, "persistent init/work failure classification");
      const bool failed_stage_cleaned
          = std::find(persistent_calls.begin(), persistent_calls.end(), persistent_names[failure] + ".cleanup") != persistent_calls.end();
      if (fail_during_initialize) {
        require(!failed_stage_cleaned, "failed persistent initializer cleaned an unacquired stage");
        require(persistent_calls.back() == (failure == 0 ? "Logger.init" : "Logger.cleanup"), "persistent init rollback cleans only prior acquired stages");
      } else {
        require(failed_stage_cleaned, "persistent work failure did not clean the acquired current stage");
        require(persistent_calls.back() == "Logger.cleanup", "persistent work rollback cleans acquired stages in reverse");
      }
      for (int acquired = 0; acquired < failure; ++acquired) {
        require(std::find(persistent_calls.begin(), persistent_calls.end(), persistent_names[acquired] + ".cleanup") != persistent_calls.end(),
                "persistent rollback skipped a prior acquired stage");
      }
    }
  }
  std::vector<std::string> persistent_cleanup_calls;
  StaOperationResult persistent_cleanup_failure = executeStaPersistentInitialization(makeStages(persistent_names, persistent_cleanup_calls, -1, 1, 1));
  require(!persistent_cleanup_failure.succeeded && persistent_cleanup_failure.cleanup_failed, "persistent rollback cleanup failure classification");
  require(persistent_cleanup_calls.back() == "Logger.cleanup", "persistent rollback continues after cleanup failure");

  std::vector<std::string> persistent_success_calls;
  require(executeStaPersistentInitialization(makeStages(persistent_names, persistent_success_calls)).succeeded, "persistent initialization success");
  require(std::find(persistent_success_calls.begin(), persistent_success_calls.end(), "Logger.cleanup") == persistent_success_calls.end(),
          "persistent resources transfer to owner on success");

  const std::vector<std::string> close_names{"DelayCalculator runtime", "DelayCalculator", "DataManager output", "DataManager", "Logger"};
  for (int failure = 0; failure < static_cast<int>(close_names.size()); ++failure) {
    std::vector<std::string> close_calls;
    std::vector<StaCleanupAction> actions;
    for (int index = 0; index < static_cast<int>(close_names.size()); ++index) {
      const std::string name = close_names[index];
      actions.push_back({name, [&, index, name, failure] {
                           close_calls.push_back(name);
                           if (index == failure)
                             throw std::runtime_error("injected persistent cleanup failure");
                         }});
    }
    StaOperationResult close_failure = executeStaCleanupSequence(actions);
    require(!close_failure.succeeded && close_failure.cleanup_failed, "persistent close failure classification");
    require(close_calls == close_names, "persistent close attempts every cleanup action in legacy order");
  }

  const std::vector<std::string> full_names{"GraphBuilder",  "ClockPropagator", "TimingPropagator", "PowerPropagator", "TimingAnalyzer",
                                            "PowerAnalyzer", "TimingReporter",  "PowerReporter",    "SDFWriter"};
  for (int failure = 0; failure < static_cast<int>(full_names.size()); ++failure) {
    for (bool fail_during_initialize : {true, false}) {
      std::vector<std::string> pipeline_calls;
      auto stages = makeStages(full_names, pipeline_calls, fail_during_initialize ? failure : -1, fail_during_initialize ? -1 : failure);
      ista::StaLifecycleStage envelope{"DelayCalculator runtime", [&] { pipeline_calls.push_back("DelayCalculator runtime.init"); }, [] {},
                                       [&] { pipeline_calls.push_back("DelayCalculator runtime.cleanup"); }};
      StaOperationResult failure_result = executeStaLifecyclePipeline(stages, envelope);
      require(!failure_result.succeeded && !failure_result.cleanup_failed, "full pipeline injected stage failure");
      require(pipeline_calls.back() == "DelayCalculator runtime.cleanup", "full pipeline failure cleans delay runtime last");
      const std::string failed_cleanup = full_names[failure] + ".cleanup";
      const bool failed_stage_cleaned = std::find(pipeline_calls.begin(), pipeline_calls.end(), failed_cleanup) != pipeline_calls.end();
      require(failed_stage_cleaned != fail_during_initialize, "pipeline cleanup arm state does not match acquired stage");
    }
  }
  std::vector<std::string> delay_runtime_init_failure_calls;
  StaOperationResult envelope_init_failure = executeStaLifecyclePipeline(
      {}, ista::StaLifecycleStage{"DelayCalculator runtime",
                                  [&] {
                                    delay_runtime_init_failure_calls.push_back("init");
                                    throw std::runtime_error("injected envelope init failure");
                                  },
                                  [&] { delay_runtime_init_failure_calls.push_back("work"); }, [&] { delay_runtime_init_failure_calls.push_back("destroy"); }});
  require(!envelope_init_failure.succeeded && delay_runtime_init_failure_calls == std::vector<std::string>({"init"}),
          "DelayCalculator runtime init throw must not call destroy");
  std::vector<std::string> envelope_work_calls;
  StaOperationResult envelope_work_failure
      = executeStaLifecyclePipeline({}, ista::StaLifecycleStage{"DelayCalculator runtime", [&] { envelope_work_calls.push_back("init"); },
                                                                [&] {
                                                                  envelope_work_calls.push_back("work");
                                                                  throw std::runtime_error("injected envelope work failure");
                                                                },
                                                                [&] { envelope_work_calls.push_back("destroy"); }});
  require(!envelope_work_failure.succeeded && envelope_work_calls == std::vector<std::string>({"init", "work", "destroy"}),
          "envelope work failure must clean the armed resource");

  std::vector<std::string> cleanup_failure_calls;
  StaOperationResult pipeline_cleanup_failure = executeStaLifecyclePipeline(
      makeStages({"GraphBuilder", "ClockPropagator"}, cleanup_failure_calls, -1, -1, 0),
      ista::StaLifecycleStage{"DelayCalculator runtime", [] {}, [] {}, [&] { cleanup_failure_calls.push_back("DelayCalculator runtime.cleanup"); }});
  require(!pipeline_cleanup_failure.succeeded && pipeline_cleanup_failure.cleanup_failed, "pipeline cleanup failure classification");
  require(cleanup_failure_calls.back() == "DelayCalculator runtime.cleanup", "remaining envelope cleanup attempted after stage cleanup failure");

  std::vector<std::string> update_calls;
  StaOperationResult retained
      = executeStaLifecyclePipeline(makeStages({"GraphBuilder", "ClockPropagator", "TimingPropagator"}, update_calls),
                                    ista::StaLifecycleStage{"DelayCalculator runtime", [&] { update_calls.push_back("DelayCalculator runtime.init"); }, [] {},
                                                            [&] { update_calls.push_back("DelayCalculator runtime.cleanup"); }},
                                    true);
  require(retained.succeeded, "update pipeline succeeds");
  require(std::find(update_calls.begin(), update_calls.end(), "DelayCalculator runtime.cleanup") == update_calls.end(),
          "successful update retains delay runtime for persistent close");
  for (const std::vector<std::string>& stage_names : {std::vector<std::string>{"TimingAnalyzer", "TimingReporter"}, std::vector<std::string>{"SDFWriter"}}) {
    StaOperationResult followup = executeStaLifecyclePipeline(makeStages(stage_names, update_calls));
    require(followup.succeeded, "report/write after retained update runtime");
  }
  update_calls.push_back("DelayCalculator runtime.cleanup");
  require(update_calls.back() == "DelayCalculator runtime.cleanup", "persistent close releases retained runtime");

  StaOwnerLease lease;
  require(lease.acquireSession(11).succeeded, "first session lease");
  require(!lease.acquireSession(12).succeeded, "second session conflict");
  require(!lease.requireLegacyUse().succeeded, "legacy use blocked by session");
  require(lease.requireSessionUse(11).succeeded, "owner session use");
  lease.completeClose(true);
  require(lease.isNone(), "successful close releases lease");
  require(lease.acquireLegacy().succeeded, "legacy follows closed session");
  lease.quarantineCurrent();
  require(!lease.requireLegacyUse().succeeded, "quarantine blocks work");
  require(lease.requireLegacyClose().succeeded, "quarantine retains one cleanup owner");
  lease.completeClose(true);
  require(lease.isQuarantined(), "quarantine remains sticky after owner cleanup");
  require(!lease.requireLegacyClose().succeeded, "cleanup owner is consumed");
  require(!lease.acquireSession(13).succeeded, "quarantine blocks new session");

  StaOwnerLease legacy_owner;
  require(legacy_owner.acquireLegacy().succeeded, "legacy acquire");
  require(legacy_owner.requireLegacyUse().succeeded, "legacy stateful use");
  require(!legacy_owner.acquireSession(30).succeeded, "active legacy blocks typed initialize");
  require(!legacy_owner.requireSessionUse(30).succeeded, "active legacy blocks typed run");
  require(!legacy_owner.requireSessionClose(30).succeeded, "active legacy blocks typed close");
  require(!legacy_owner.isNone(), "active owner blocks destroyInst");
  legacy_owner.completeClose(true);
  require(legacy_owner.acquireSession(30).succeeded, "typed session follows normal legacy close");
  require(!legacy_owner.requireLegacyUse().succeeded, "active typed session blocks legacy stateful API");
  require(!legacy_owner.requireLegacyClose().succeeded, "active typed session blocks legacy destroy");
  require(!legacy_owner.requireSessionUse(31).succeeded, "wrong session token blocked");
  require(!legacy_owner.requireSessionClose(31).succeeded, "wrong session cleanup token blocked");
  legacy_owner.completeClose(false);
  require(legacy_owner.isQuarantined(), "persistent cleanup failure creates sticky quarantine without cleanup owner");
  require(!legacy_owner.requireSessionClose(30).succeeded, "persistent cleanup attempt is consumed");

  StaOwnerLease rollback_success;
  require(rollback_success.acquireSession(40).succeeded, "rollback-success acquire");
  rollback_success.releaseAfterRollback();
  require(rollback_success.isNone() && rollback_success.acquireLegacy().succeeded, "successful rollback permits next owner");

  StaOwnerLease rollback;
  require(rollback.acquireSession(21).succeeded, "rollback lease acquire");
  rollback.quarantineAfterRollback();
  require(rollback.isQuarantined(), "rollback cleanup failure quarantines");
  require(!rollback.requireSessionClose(21).succeeded, "rollback consumes cleanup ownership");
  return 0;
}
