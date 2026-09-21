// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <any>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../source/data_manager/DataManager.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "CsrSnapshotObservation.hpp"
#include "StaSessionTestSync.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "STAInterface.hpp"
#include "StaCsrSessionContext.hpp"
#include "StaLifecycleGuard.hpp"
#include "StaProductionLifecycle.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista {
class StaCsrSessionContextTestAccess final
{
 public:
  static bool hasLibraryModels(const StaCsrSessionContext& context) noexcept { return bool(context._library_models); }
};
class StaInterfaceLifecycleTestAccess final
{
 public:
  static std::uint64_t constructorCount() noexcept { return STAInterface::constructorCountForTesting(); }
};
}  // namespace ista
namespace {
using namespace ista;
using ista::test::await;
using ista::test::failTimeout;
using ista::test::BoundedGate;
void require(bool condition, const std::string& message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::quick_exit(1);
  }
}
csr::StaAnalysisContext validContext()
{
  return {"slow", "25", "__no_rc_corner__"};
}
class GuardOperations final : public StaProductionLifecycleOperations
{
 public:
  StaOperationResult initialize(std::map<std::string, std::any>&) override
  {
    DataManager::initInst();
    return StaOperationResult::success();
  }
  StaOperationResult runFullBaseline() override { return invoke(run_hook, full_calls); }
  StaOperationResult extractLib() override { return StaOperationResult::success(); }
  StaOperationResult updateTiming() override { return StaOperationResult::success(); }
  StaOperationResult writeSdf(const std::string&) override { return StaOperationResult::success(); }
  StaOperationResult reportTiming() override { return StaOperationResult::success(); }
  StaOperationResult close() override { return invoke(close_hook, close_calls); }
  std::function<void()> run_hook;
  std::function<void()> close_hook;
  int full_calls{0};
  int close_calls{0};

 private:
  StaOperationResult invoke(std::function<void()>& hook, int& calls)
  {
    ++calls;
    if (hook)
      hook();
    return StaOperationResult::success();
  }
};

using DatabaseProjection = std::variant<std::vector<std::uint8_t>, csr::CsrReferenceInputExportFailure>;

DatabaseProjection projectDatabase(std::uint64_t token)
{
  auto exported = csr::exportCsrReferenceInputCandidate(STADM.getDatabase(), validContext(), token);
  if (auto* input = std::get_if<csr::CsrReferenceInput>(&exported))
    return csr::canonicalReferenceInputBytes(*input);
  return std::get<csr::CsrReferenceInputExportFailure>(std::move(exported));
}

STAInterface& install(GuardOperations& operations)
{
  STAInterface::destroyInst();
  STAInterface& interface = STAInterface::getInst();
  interface.setLifecycleOperationsForTesting(&operations);
  return interface;
}



void testSingletonLifetime()
{
  constexpr std::size_t kThreads = 8;
  BoundedGate start;
  std::vector<STAInterface*> instances(kThreads, nullptr);
  std::vector<std::future<STAInterface*>> calls;
  calls.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    calls.push_back(std::async(std::launch::async, [&] {
      start.wait("singleton start");
      return &STAInterface::getInst();
    }));
  }
  start.signal();
  for (std::size_t index = 0; index < kThreads; ++index)
    instances[index] = await(calls[index], "concurrent singleton construction");

  STAInterface* const first = instances.front();
  require(first != nullptr, "singleton construction returned a null address");
  for (STAInterface* instance : instances)
    require(instance == first, "concurrent singleton construction returned different addresses");
  require(StaInterfaceLifecycleTestAccess::constructorCount() == 1, "singleton constructor count was not exactly one");

  STAInterface::destroyInst();
  require(&STAInterface::getInst() == first, "destroyInst changed the process-lifetime singleton address");
  require(StaInterfaceLifecycleTestAccess::constructorCount() == 1, "destroyInst reconstructed the singleton");
}

void testTokenAllocator()
{
  StaSessionTokenAllocator exhausted(std::numeric_limits<std::uint64_t>::max());
  require(exhausted.allocate() == std::numeric_limits<std::uint64_t>::max(), "allocator did not issue UINT64_MAX exactly once");
  require(!exhausted.allocate().has_value() && !exhausted.allocate().has_value(), "allocator exhaustion was not sticky");

  StaSessionTokenAllocator zero(0);
  require(!zero.allocate().has_value() && !zero.allocate().has_value(), "zero-initialized allocator was not exhausted");

  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kTokensPerThread = 128;
  StaSessionTokenAllocator concurrent;
  BoundedGate start;
  std::vector<std::vector<std::uint64_t>> per_thread(kThreads);
  std::vector<std::future<void>> calls;
  calls.reserve(kThreads);
  for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
    calls.push_back(std::async(std::launch::async, [&, thread_index] {
      start.wait("token allocator start");
      auto& local = per_thread[thread_index];
      local.reserve(kTokensPerThread);
      for (std::size_t index = 0; index < kTokensPerThread; ++index) {
        const auto token = concurrent.allocate();
        require(token.has_value() && *token != 0, "concurrent allocator returned exhaustion or zero");
        local.push_back(*token);
      }
    }));
  }
  start.signal();
  for (auto& call : calls)
    await(call, "concurrent token allocation");

  std::vector<std::uint64_t> tokens;
  tokens.reserve(kThreads * kTokensPerThread);
  for (const auto& local : per_thread)
    tokens.insert(tokens.end(), local.begin(), local.end());
  std::ranges::sort(tokens);
  require(tokens.size() == kThreads * kTokensPerThread, "concurrent allocator returned the wrong token count");
  for (std::size_t index = 0; index < tokens.size(); ++index)
    require(tokens[index] == index + 1, "concurrent allocator returned a duplicate or skipped token");
}

void testOwnerLease()
{
  StaOwnerLease lease;
  require(!lease.acquireSession(0).succeeded, "lease accepted zero token");
  require(!lease.requireSessionUse(0).succeeded && !lease.requireSessionClose(0).succeeded, "lease accepted zero-token use or close");
  require(lease.acquireSession(41).succeeded, "lease did not acquire a session owner");
  require(lease.requireSessionUse(41).succeeded, "lease rejected its active owner");
  require(!lease.requireSessionUse(42).succeeded, "lease accepted a foreign owner");
  require(lease.requireSessionClose(41).succeeded, "lease rejected close by its owner");
  lease.completeClose(true);
  require(lease.isNone() && !lease.isQuarantined(), "successful close did not release the lease");
}





void requireInvalid(const StaStatus& status, const std::string& operation)
{
  require(status.code == StaSessionStatusCode::kInvalidState, operation + " did not return invalid-state");
}
void requireInvalid(const StaCsrScatterAttempt& attempt, const std::string& operation)
{
  require(attempt.value.scatter_result.code == csr::CsrScatterCode::kInvalidState && attempt.value.scatter_result.writes_applied == 0,
          operation + " did not return zero-write invalid_state");
}
void testLibraryModelsClose()
{
  GuardOperations operations;
  STAInterface& interface = install(operations);
  constexpr std::uint64_t token = 697;
  StaCsrSessionContext owner(validContext(), token), foreign(validContext(), token + 1);
  std::map<std::string, std::any> config;
  require(interface.initializeSession(token, config).ok(), "model owner initialize");
  auto* models = &owner.libraryModels(STADM.getDatabase());
  require(StaCsrSessionContextTestAccess::hasLibraryModels(owner), "session did not retain its models");
  std::unique_ptr<csr::CsrSnapshotObservation> released;
  requireInvalid(interface.closeSession(token, &foreign, &released), "foreign model owner close");
  require(&owner.libraryModels(STADM.getDatabase()) == models, "foreign close discarded the active model owner");
  operations.close_hook = [&] {
    require(!StaCsrSessionContextTestAccess::hasLibraryModels(owner), "native close retained its library model owner");
  };
  require(interface.closeSession(token, &owner, &released).ok(), "model owner close");
  require(!released && !StaCsrSessionContextTestAccess::hasLibraryModels(owner), "close retained unrequested state");
  require(!interface.closeSession(token, &owner, &released).ok() && operations.close_calls == 1, "closed owner repeated native cleanup");
  STAInterface::destroyInst();
}
void testForeignContextAndSameTokenScatter()
{
  GuardOperations operations;
  STAInterface& interface = install(operations);
  constexpr std::uint64_t kOwner = 701;
  constexpr std::uint64_t kForeign = 702;
  StaCsrSessionContext owner(validContext(), kOwner);
  StaCsrSessionContext foreign(validContext(), kForeign);
  std::map<std::string, std::any> config;
  require(interface.initializeSession(kOwner, config).ok() && interface.runSessionBaseline(kOwner, &owner).ok(), "authority fixture capture failed");
  auto reference = interface.computeSessionCsrReferenceForTesting(kOwner, &owner);
  if constexpr (!incregpusta_debug_checks_enabled) {
    const auto* failure = std::get_if<StaCsrSessionOperationFailure>(&reference.value);
    require(failure && failure->writes_applied == 0, "OFF diagnostic propagation executed");
    requireInvalid(interface.runSessionBaseline(kOwner, &foreign), "OFF foreign-context run");
    std::unique_ptr<csr::CsrSnapshotObservation> out;
    requireInvalid(interface.copySessionCsrSnapshotObservation(kOwner, &foreign, &out), "OFF foreign-context copy");
    requireInvalid(interface.closeSession(kOwner, &foreign, &out), "OFF foreign-context close");
    require(interface.closeSession(kOwner, &owner, &out).ok() && !out && operations.close_calls == 1,
            "OFF rejection changed ownership or created a close observation");
    STAInterface::destroyInst();
    return;
  }
  auto* computation = std::get_if<csr::CsrComputationResult>(&reference.value);
  auto* timing = computation ? std::get_if<csr::CsrTimingResult>(&computation->outcome) : nullptr;
  require(timing, "authority fixture did not produce timing");
  require(interface.scatterSessionCsrTimingResultForTesting(kOwner, &owner, *timing).value.scatter_result.code == csr::CsrScatterCode::kSuccess,
          "first same-token scatter failed");
  std::unique_ptr<csr::CsrSnapshotObservation> before;
  require(interface.copySessionCsrSnapshotObservation(kOwner, &owner, &before).ok() && before, "pre-concurrent observation read failed");
  BoundedGate run_entered, run_release, scatter_started;
  operations.run_hook = [&] {
    run_entered.signal();
    run_release.wait("run-versus-scatter release");
  };
  auto concurrent_run = std::async(std::launch::async, [&] { return interface.runSessionBaseline(kOwner, nullptr); });
  run_entered.wait("run-versus-scatter entry");
  auto reader = std::async(std::launch::async, [&] {
    std::unique_ptr<csr::CsrSnapshotObservation> value;
    StaStatus status = interface.copySessionCsrSnapshotObservation(kOwner, &owner, &value);
    return std::make_pair(std::move(status), std::move(value));
  });
  auto scatter = std::async(std::launch::async, [&] {
    scatter_started.signal();
    return interface.scatterSessionCsrTimingResultForTesting(kOwner, &owner, *timing);
  });
  scatter_started.wait("run-versus-scatter start");
  require(scatter.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout, "scatter bypassed guarded run");
  run_release.signal();
  require(await(concurrent_run, "run versus scatter").ok(), "guarded concurrent run failed");
  auto read = await(reader, "read versus scatter");
  auto second = await(scatter, "second same-token scatter");
  std::unique_ptr<csr::CsrSnapshotObservation> after;
  require(interface.copySessionCsrSnapshotObservation(kOwner, &owner, &after).ok() && after, "post-concurrent observation read failed");
  require(read.first.ok() && read.second && (*read.second == *before || *read.second == *after), "read returned a torn observation");
  require(second.value.scatter_result.code == csr::CsrScatterCode::kSuccess, "second same-token scatter failed");
  std::vector<DatabaseProjection> database;
  operations.run_hook = [&] { database.push_back(projectDatabase(kOwner)); };
  require(interface.runSessionBaseline(kOwner, nullptr).ok(), "pre-foreign Database projection failed");
  std::unique_ptr<csr::CsrSnapshotObservation> owner_before;
  require(interface.copySessionCsrSnapshotObservation(kOwner, &owner, &owner_before).ok() && owner_before, "pre-foreign observation failed");
  requireInvalid(interface.runSessionBaseline(kOwner, &foreign), "foreign-context run");
  std::unique_ptr<csr::CsrSnapshotObservation> foreign_copy;
  requireInvalid(interface.copySessionCsrSnapshotObservation(kOwner, &foreign, &foreign_copy), "foreign-context copy");
  auto foreign_reference = interface.computeSessionCsrReferenceForTesting(kOwner, &foreign);
  const auto* failure = std::get_if<StaCsrSessionOperationFailure>(&foreign_reference.value);
  require(failure && failure->writes_applied == 0, "foreign-context reference wrote state");
  requireInvalid(interface.scatterSessionCsrTimingResultForTesting(kOwner, &foreign, csr::CsrTimingResult{}), "foreign-context scatter");
  std::unique_ptr<csr::CsrSnapshotObservation> released;
  requireInvalid(interface.closeSession(kOwner, &foreign, &released), "foreign-context close");
  require(interface.runSessionBaseline(kOwner, nullptr).ok(), "foreign close left pending set");
  std::unique_ptr<csr::CsrSnapshotObservation> owner_after;
  require(interface.copySessionCsrSnapshotObservation(kOwner, &owner, &owner_after).ok() && owner_after && *owner_after == *owner_before,
          "foreign-context operations changed owner observation");
  require(database.size() == 2 && database[0] == database[1] && !foreign_copy && !released && operations.full_calls == 4 && operations.close_calls == 0,
          "foreign-context operations changed Database, output, or native calls");
  require(interface.lifecycleStateForTesting() == "session(701)", "foreign-context operations changed owner/pending");
  require(interface.closeSession(kOwner, &owner, &released).ok() && released && operations.close_calls == 1,
          "normal close after foreign rejection did not hand off exact-once");
  std::unique_ptr<csr::CsrSnapshotObservation> second_out;
  require(!interface.closeSession(kOwner, &owner, &second_out).ok() && operations.close_calls == 1, "close after handoff retried cleanup");
  require(interface.initializeSession(kForeign, config).ok(), "foreign observation owner switch failed");
  std::unique_ptr<csr::CsrSnapshotObservation> foreign_after;
  require(interface.copySessionCsrSnapshotObservation(kForeign, &foreign, &foreign_after).ok() && !foreign_after,
          "foreign-context operations changed foreign observation");
  std::unique_ptr<csr::CsrSnapshotObservation> foreign_released;
  require(interface.closeSession(kForeign, nullptr, &foreign_released).ok(), "foreign owner cleanup failed");
  STAInterface::destroyInst();
}
void testQueuedAuthorityAndDestroy()
{
  GuardOperations operations;
  STAInterface& interface = install(operations);
  constexpr std::uint64_t kToken = 721;
  StaCsrSessionContext context(validContext(), kToken);
  std::map<std::string, std::any> config;
  require(interface.initializeSession(kToken, config).ok(), "queued authority fixture initialize failed");
  BoundedGate destroy_entered;
  BoundedGate destroy_release;
  operations.run_hook = [&] {
    destroy_entered.signal();
    destroy_release.wait("destroy-versus-operation release");
  };
  auto first_run = std::async(std::launch::async, [&] { return interface.runSessionBaseline(kToken, nullptr); });
  destroy_entered.wait("destroy-versus-operation entry");
  BoundedGate destroy_started;
  auto destroy = std::async(std::launch::async, [&] {
    destroy_started.signal();
    try {
      STAInterface::destroyInst();
    } catch (const std::runtime_error&) {
      return true;
    }
    return false;
  });
  destroy_started.wait("queued destroy start");
  require(destroy.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout, "destroy bypassed active operation guard");
  destroy_release.signal();
  require(await(first_run, "destroy-versus-operation run").ok() && await(destroy, "queued destroy"), "queued destroy was not rejected");
  BoundedGate run_entered;
  BoundedGate run_release;
  DatabaseProjection database_before;
  operations.run_hook = [&] {
    database_before = projectDatabase(kToken);
    run_entered.signal();
    run_release.wait("close-pending long-run release");
  };
  auto long_run = std::async(std::launch::async, [&] { return interface.runSessionBaseline(kToken, nullptr); });
  run_entered.wait("close-pending long-run entry");
  std::unique_ptr<csr::CsrSnapshotObservation> queued_observation;
  BoundedGate read_started, scatter_started;
  auto queued_read = std::async(std::launch::async, [&] {
    read_started.signal();
    return interface.copySessionCsrSnapshotObservation(kToken, &context, &queued_observation);
  });
  auto queued_scatter = std::async(std::launch::async, [&] {
    scatter_started.signal();
    return interface.scatterSessionCsrTimingResultForTesting(kToken, &context, csr::CsrTimingResult{});
  });
  read_started.wait("queued read start");
  scatter_started.wait("queued scatter start");
  require(queued_read.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout
              && queued_scatter.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout,
          "typed use did not queue behind long run");
  std::unique_ptr<csr::CsrSnapshotObservation> released;
  BoundedGate close_started;
  auto close = std::async(std::launch::async, [&] {
    close_started.signal();
    return interface.closeSession(kToken, nullptr, &released);
  });
  close_started.wait("close-pending start");
  std::vector<std::future<StaStatus>> probes;
  bool pending_seen = false;
  for (int attempt = 0; attempt < 8 && !pending_seen; ++attempt) {
    probes.push_back(std::async(std::launch::async, [&] {
      std::unique_ptr<csr::CsrSnapshotObservation> out;
      return interface.copySessionCsrSnapshotObservation(kToken, &context, &out);
    }));
    if (probes.back().wait_for(std::chrono::milliseconds(25)) == std::future_status::ready) {
      requireInvalid(probes.back().get(), "close-pending probe");
      probes.pop_back();
      pending_seen = true;
    }
  }
  if (!pending_seen)
    failTimeout("close-pending publication");
  requireInvalid(interface.scatterSessionCsrTimingResultForTesting(kToken, &context, csr::CsrTimingResult{}), "close-pending scatter");
  run_release.signal();
  require(await(long_run, "close-versus-long-run").ok(), "long run failed");
  const StaStatus read = await(queued_read, "queued read second validation");
  const StaCsrScatterAttempt scattered = await(queued_scatter, "queued scatter second validation");
  const StaStatus closed = await(close, "queued close");
  for (auto& probe : probes)
    require(!await(probe, "queued pending probe").ok(), "queued pending probe touched context");
  require(!read.ok() && !queued_observation, "queued read touched context");
  requireInvalid(scattered, "queued scatter");
  require(closed.ok() && !released && operations.close_calls == 1, "queued close was not exact-once");
  require(!interface.closeSession(kToken, nullptr, &released).ok() && operations.close_calls == 1, "second queued close retried cleanup");
  require(interface.initializeSession(kToken, config).ok(), "post-queue inspection initialize failed");
  DatabaseProjection database_after;
  operations.run_hook = [&] { database_after = projectDatabase(kToken); };
  require(interface.runSessionBaseline(kToken, nullptr).ok() && database_before == database_after, "queued uses changed Database");
  std::unique_ptr<csr::CsrSnapshotObservation> observation_after;
  require(interface.copySessionCsrSnapshotObservation(kToken, &context, &observation_after).ok() && !observation_after,
          "queued uses changed context observation");
  require(interface.closeSession(kToken, nullptr, &released).ok(), "post-queue cleanup failed");
  STAInterface::destroyInst();
}
void testRecursiveCallbacks()
{
  GuardOperations operations;
  STAInterface& interface = install(operations);
  constexpr std::uint64_t kToken = 731;
  StaCsrSessionContext context(validContext(), kToken);
  std::map<std::string, std::any> config;
  require(interface.initializeSession(kToken, config).ok(), "recursive callback fixture initialize failed");
  std::optional<StaStatus> nested_use;
  std::optional<StaStatus> nested_close;
  std::unique_ptr<csr::CsrSnapshotObservation> nested_out;
  bool destroy_rejected = false;
  auto recurse = [&] {
    nested_use = interface.runSessionBaseline(kToken, nullptr);
    nested_close = interface.closeSession(kToken, nullptr, &nested_out);
    try {
      STAInterface::destroyInst();
    } catch (const std::runtime_error&) {
      destroy_rejected = true;
    }
  };
  auto requireRejected = [&] {
    require(nested_use && nested_close, "recursive callback did not execute");
    requireInvalid(*nested_use, "recursive use");
    requireInvalid(*nested_close, "recursive close");
    require(!nested_out && destroy_rejected, "recursive callback wrote output or destroyed singleton");
  };
  operations.run_hook = recurse;
  require(interface.runSessionBaseline(kToken, nullptr).ok(), "outer callback run failed");
  requireRejected();
  std::unique_ptr<csr::CsrSnapshotObservation> live;
  require(interface.copySessionCsrSnapshotObservation(kToken, &context, &live).ok() && !live, "operation callback changed owner, pending, or observation");
  std::unique_ptr<csr::CsrSnapshotObservation> released;
  require(interface.closeSession(kToken, nullptr, &released).ok() && operations.close_calls == 1, "normal close after operation callback was not exact-once");
  require(interface.initializeSession(kToken, config).ok(), "cleanup callback fixture reinitialize failed");
  destroy_rejected = false;
  operations.run_hook = {};
  operations.close_hook = recurse;
  require(interface.closeSession(kToken, nullptr, &released).ok(), "outer cleanup callback close failed");
  requireRejected();
  require(operations.close_calls == 2 && interface.lifecycleStateForTesting() == "none", "cleanup callback changed owner/pending or reentered cleanup");
  require(!interface.closeSession(kToken, nullptr, &released).ok() && operations.close_calls == 2, "second callback close retried cleanup");
  STAInterface::destroyInst();
}
void testExceptionReleasesAuthority()
{
  GuardOperations operations;
  STAInterface& interface = install(operations);
  constexpr std::uint64_t kToken = 741;
  std::map<std::string, std::any> config;
  require(interface.initializeSession(kToken, config).ok(), "exception fixture initialize failed");
  operations.run_hook = [] { throw std::runtime_error("injected guarded operation throw"); };
  const StaStatus run = interface.runSessionBaseline(kToken, nullptr);
  require(run.code == StaSessionStatusCode::kCleanupFailed && interface.lifecycleStateForTesting() == "quarantined(session(741))",
          "operation exception did not retain cleanup authority");
  std::unique_ptr<csr::CsrSnapshotObservation> released;
  auto cleanup = std::async(std::launch::async, [&] { return interface.closeSession(kToken, nullptr, &released); });
  require(await(cleanup, "close after operation exception").ok() && operations.close_calls == 1, "exception did not release operation guard");
  require(interface.lifecycleStateForTesting() == "quarantined(none)", "exception cleanup changed sticky quarantine");
  require(interface.closeSession(kToken, nullptr, &released).code == StaSessionStatusCode::kQuarantined && operations.close_calls == 1,
          "second exception close retried cleanup");
}
}  // namespace
int main()
{
  try {
    testSingletonLifetime();
    testTokenAllocator();
    testOwnerLease();
    testLibraryModelsClose();
    testForeignContextAndSameTokenScatter();
    testQueuedAuthorityAndDestroy();
    testRecursiveCallbacks();
    testExceptionReleasesAuthority();
    DataManager::destroyInst();
    std::cout << "STA session lifecycle contracts passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
