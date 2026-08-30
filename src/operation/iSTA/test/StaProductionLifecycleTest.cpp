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
#include <array>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "CsrReferenceTypes.hpp"
#include "IncrementalStaSession.hpp"
#include "STAInterface.hpp"
#include "StaProductionLifecycle.hpp"

namespace {

void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

template <typename Exception, typename Callable>
void requireThrows(Callable callable, const char* message)
{
  try {
    callable();
  } catch (const Exception&) {
    return;
  }
  throw std::runtime_error(message);
}

ista::StaExecutionRequest cpuFullRequest()
{
  return {.backend = ista::StaBackend::kCpu, .update_strategy = ista::StaUpdateStrategy::kFull, .fallback_policy = ista::StaFallbackPolicy::kError};
}

ista::csr::StaAnalysisContext testAnalysisContext()
{
  return {"slow", "25", "__no_rc_corner__"};
}

enum class ThrowKind
{
  kNone,
  kStandard,
  kUnknown
};

void throwIfRequested(ThrowKind kind, const char* operation)
{
  switch (kind) {
    case ThrowKind::kNone:
      return;
    case ThrowKind::kStandard:
      throw std::runtime_error(std::string("injected ") + operation + " throw");
    case ThrowKind::kUnknown:
      throw 17;
  }
}

class FakeProductionOperations final : public ista::StaProductionLifecycleOperations
{
 public:
  ista::StaOperationResult initialize(std::map<std::string, std::any>&) override
  {
    ++initialize_calls;
    throwIfRequested(initialize_throw, "initialize");
    return initialize_result;
  }

  ista::StaOperationResult runFullBaseline() override
  {
    ++full_calls;
    throwIfRequested(full_throw, "full baseline");
    return full_result;
  }

  ista::StaOperationResult extractLib() override
  {
    ++extract_calls;
    throwIfRequested(extract_throw, "extract");
    return extract_result;
  }

  ista::StaOperationResult updateTiming() override
  {
    ++update_calls;
    throwIfRequested(update_throw, "update");
    return update_result;
  }

  ista::StaOperationResult writeSdf(const std::string&) override
  {
    ++write_calls;
    throwIfRequested(write_throw, "write");
    return write_result;
  }

  ista::StaOperationResult reportTiming() override
  {
    ++report_calls;
    throwIfRequested(report_throw, "report");
    return report_result;
  }

  ista::StaOperationResult close() override
  {
    ++close_calls;
    throwIfRequested(close_throw, "close");
    return close_result;
  }

  void resetResults()
  {
    initialize_result = ista::StaOperationResult::success();
    full_result = ista::StaOperationResult::success();
    extract_result = ista::StaOperationResult::success();
    update_result = ista::StaOperationResult::success();
    write_result = ista::StaOperationResult::success();
    report_result = ista::StaOperationResult::success();
    close_result = ista::StaOperationResult::success();
    initialize_throw = ThrowKind::kNone;
    full_throw = ThrowKind::kNone;
    extract_throw = ThrowKind::kNone;
    update_throw = ThrowKind::kNone;
    write_throw = ThrowKind::kNone;
    report_throw = ThrowKind::kNone;
    close_throw = ThrowKind::kNone;
  }

  ista::StaOperationResult initialize_result;
  ista::StaOperationResult full_result;
  ista::StaOperationResult extract_result;
  ista::StaOperationResult update_result;
  ista::StaOperationResult write_result;
  ista::StaOperationResult report_result;
  ista::StaOperationResult close_result;
  ThrowKind initialize_throw = ThrowKind::kNone;
  ThrowKind full_throw = ThrowKind::kNone;
  ThrowKind extract_throw = ThrowKind::kNone;
  ThrowKind update_throw = ThrowKind::kNone;
  ThrowKind write_throw = ThrowKind::kNone;
  ThrowKind report_throw = ThrowKind::kNone;
  ThrowKind close_throw = ThrowKind::kNone;
  int initialize_calls = 0;
  int full_calls = 0;
  int extract_calls = 0;
  int update_calls = 0;
  int write_calls = 0;
  int report_calls = 0;
  int close_calls = 0;
};

ista::STAInterface& install(FakeProductionOperations& operations)
{
  ista::STAInterface& interface = ista::STAInterface::getInst();
  requireThrows<std::invalid_argument>([&] { interface.setLifecycleOperationsForTesting(nullptr); }, "null test operations accepted");
  interface.setLifecycleOperationsForTesting(&operations);
  return interface;
}

enum class LegacyPipeline
{
  kRun,
  kExtract,
  kUpdate,
  kReport,
  kWrite
};

int pipelineCalls(const FakeProductionOperations& operations, LegacyPipeline pipeline)
{
  switch (pipeline) {
    case LegacyPipeline::kRun:
      return operations.full_calls;
    case LegacyPipeline::kExtract:
      return operations.extract_calls;
    case LegacyPipeline::kUpdate:
      return operations.update_calls;
    case LegacyPipeline::kReport:
      return operations.report_calls;
    case LegacyPipeline::kWrite:
      return operations.write_calls;
  }
  throw std::logic_error("unknown legacy pipeline");
}

void setPipelineResult(FakeProductionOperations& operations, LegacyPipeline pipeline, ista::StaOperationResult result)
{
  switch (pipeline) {
    case LegacyPipeline::kRun:
      operations.full_result = std::move(result);
      break;
    case LegacyPipeline::kExtract:
      operations.extract_result = std::move(result);
      break;
    case LegacyPipeline::kUpdate:
      operations.update_result = std::move(result);
      break;
    case LegacyPipeline::kReport:
      operations.report_result = std::move(result);
      break;
    case LegacyPipeline::kWrite:
      operations.write_result = std::move(result);
      break;
  }
}

void setPipelineThrow(FakeProductionOperations& operations, LegacyPipeline pipeline, ThrowKind kind)
{
  switch (pipeline) {
    case LegacyPipeline::kRun:
      operations.full_throw = kind;
      break;
    case LegacyPipeline::kExtract:
      operations.extract_throw = kind;
      break;
    case LegacyPipeline::kUpdate:
      operations.update_throw = kind;
      break;
    case LegacyPipeline::kReport:
      operations.report_throw = kind;
      break;
    case LegacyPipeline::kWrite:
      operations.write_throw = kind;
      break;
  }
}

void prepareLegacyPipeline(ista::STAInterface& interface, LegacyPipeline pipeline)
{
  if (pipeline != LegacyPipeline::kReport && pipeline != LegacyPipeline::kWrite)
    return;
  std::string error;
  require(interface.updateTiming(error), "report/write prerequisite update failed");
}

void requireLegacyPipelineFailure(ista::STAInterface& interface, LegacyPipeline pipeline)
{
  std::string error;
  switch (pipeline) {
    case LegacyPipeline::kRun:
      requireThrows<std::runtime_error>([&] { interface.runSTA(); }, "legacy run failure was not reported");
      return;
    case LegacyPipeline::kExtract:
      requireThrows<std::runtime_error>([&] { interface.extractLib(); }, "legacy extract failure was not reported");
      return;
    case LegacyPipeline::kUpdate:
      require(!interface.updateTiming(error), "legacy update failure was not reported");
      return;
    case LegacyPipeline::kReport:
      require(!interface.reportTiming(error), "legacy report failure was not reported");
      return;
    case LegacyPipeline::kWrite:
      require(!interface.writeSDF("test.sdf", error), "legacy write failure was not reported");
      return;
  }
}

void ownerInterop()
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  ista::IncrementalStaSession owner(cpuFullRequest());
  require(owner.initialize(testAnalysisContext(), {}).ok(), "session initialize");
  require(operations.initialize_calls == 1, "session init callback count");

  ista::IncrementalStaSession overlap(cpuFullRequest());
  ista::StaStatus overlap_status = overlap.initialize(testAnalysisContext(), {});
  require(overlap_status.code == ista::StaSessionStatusCode::kOwnerConflict, "second session not blocked");
  requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "legacy init not blocked by session");
  requireThrows<std::logic_error>([&] { interface.setLifecycleOperationsForTesting(&operations); }, "active setter accepted");
  requireThrows<std::runtime_error>([&] { interface.runSTA(); }, "legacy run not blocked by session");
  requireThrows<std::runtime_error>([&] { interface.extractLib(); }, "legacy extract not blocked by session");
  std::string error;
  std::vector<std::string> resolved;
  require(!interface.updateTiming(error), "legacy update not blocked by session");
  require(!interface.reportTiming(error), "legacy report not blocked by session");
  require(!interface.writeSDF("test.sdf", error), "legacy write not blocked by session");
  require(!interface.createClock("clock", 1.0, 0.0, 0.5, {"in"}, error), "legacy createClock not blocked by session");
  require(!interface.setPropagatedClock({"clock"}, error), "legacy setPropagatedClock not blocked by session");
  require(!interface.getPorts({"in"}, resolved, error), "legacy getPorts not blocked by session");
  require(!interface.getClocks({"clock"}, resolved, error), "legacy getClocks not blocked by session");
  requireThrows<std::runtime_error>([&] { interface.destroySTA(); }, "legacy destroy not blocked by session");
  requireThrows<std::runtime_error>([] { ista::STAInterface::destroyInst(); }, "destroyInst not blocked by session");
  require(operations.initialize_calls == 1 && operations.full_calls == 0 && operations.extract_calls == 0 && operations.update_calls == 0
              && operations.report_calls == 0 && operations.write_calls == 0 && operations.close_calls == 0,
          "blocked legacy/session calls reached production callbacks");

  require(owner.runFullBaseline().ok(), "owner session damaged by blocked calls");
  require(owner.close().ok(), "owner session close");
  ista::IncrementalStaSession next(cpuFullRequest());
  require(next.initialize(testAnalysisContext(), {}).ok(), "next session after close");
  require(next.close().ok(), "next session close");

  interface.initSTA({});
  const int before_conflicts = operations.initialize_calls;
  ista::IncrementalStaSession blocked(cpuFullRequest());
  require(blocked.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kOwnerConflict, "typed init not blocked by legacy");
  requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "second legacy init not blocked");
  require(operations.initialize_calls == before_conflicts, "owner conflicts reached init callback");
  interface.destroySTA();
  ista::STAInterface::destroyInst();
}

void initializationWorkFailure()
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  operations.initialize_result = ista::StaOperationResult::workFailure("injected init failure");
  ista::IncrementalStaSession failed(cpuFullRequest());
  require(failed.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kInitFailed, "typed init work status");
  require(interface.lifecycleStateForTesting() == "none", "typed init work rollback did not release owner");
  require(failed.close().ok() && operations.close_calls == 0, "failed typed init performed persistent close");
  operations.resetResults();
  ista::IncrementalStaSession next(cpuFullRequest());
  require(next.initialize(testAnalysisContext(), {}).ok() && next.close().ok(), "typed re-init after work rollback");

  operations.initialize_result = ista::StaOperationResult::workFailure("injected legacy init failure");
  requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "legacy init work failure not reported");
  const int close_before = operations.close_calls;
  interface.destroySTA();
  require(operations.close_calls == close_before, "legacy rollback invoked persistent close");
  operations.resetResults();
  interface.initSTA({});
  interface.destroySTA();
  ista::STAInterface::destroyInst();
}

void initializationCleanupFailure(bool legacy)
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  operations.initialize_result = ista::StaOperationResult::cleanupFailure("injected init cleanup failure");
  if (legacy) {
    requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "legacy init cleanup failure not reported");
  } else {
    ista::IncrementalStaSession failed(cpuFullRequest());
    require(failed.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kCleanupFailed, "typed init cleanup status");
    require(failed.close().ok(), "closed failed typed handle is not idempotent");
  }
  require(interface.lifecycleStateForTesting() == "quarantined(none)", "init cleanup failure not sticky");
  const int init_before = operations.initialize_calls;
  const int close_before = operations.close_calls;
  ista::IncrementalStaSession next(cpuFullRequest());
  require(next.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kQuarantined, "typed init bypassed rollback quarantine");
  requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "legacy init bypassed rollback quarantine");
  requireThrows<std::runtime_error>([&] { interface.destroySTA(); }, "legacy destroy bypassed rollback quarantine");
  requireThrows<std::runtime_error>([] { ista::STAInterface::destroyInst(); }, "destroyInst bypassed rollback quarantine");
  require(operations.initialize_calls == init_before && operations.close_calls == close_before, "rollback quarantine triggered another native callback");
}

void typedRunFailure(bool cleanup_failure)
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  ista::IncrementalStaSession session(cpuFullRequest());
  require(session.initialize(testAnalysisContext(), {}).ok(), "typed run setup");
  operations.full_result = cleanup_failure ? ista::StaOperationResult::cleanupFailure("injected run cleanup failure")
                                           : ista::StaOperationResult::workFailure("injected run work failure");
  ista::StaExecutionResult result = session.runFullBaseline();
  require(result.status.code == (cleanup_failure ? ista::StaSessionStatusCode::kCleanupFailed : ista::StaSessionStatusCode::kRunFailed),
          "typed run failure status");
  require(session.state() == ista::StaSessionState::kPoisoned, "typed run failure did not poison handle");
  if (!cleanup_failure) {
    require(session.close().ok(), "typed run work failure close");
    operations.resetResults();
    ista::IncrementalStaSession next(cpuFullRequest());
    require(next.initialize(testAnalysisContext(), {}).ok() && next.close().ok(), "typed re-init after run work failure");
    ista::STAInterface::destroyInst();
    return;
  }

  require(interface.lifecycleStateForTesting().starts_with("quarantined(session("), "typed cleanup failure lost cleanup token");
  const int init_before = operations.initialize_calls;
  ista::IncrementalStaSession blocked(cpuFullRequest());
  require(blocked.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kQuarantined, "new typed init bypassed current quarantine");
  requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "legacy init bypassed current quarantine");
  require(operations.initialize_calls == init_before, "quarantined init reached callback");
  require(session.close().ok(), "cleanup owner persistent close");
  require(interface.lifecycleStateForTesting() == "quarantined(none)", "typed cleanup close downgraded quarantine");
  const int close_after = operations.close_calls;
  require(session.close().ok() && operations.close_calls == close_after, "typed close repeated persistent cleanup");
  require(blocked.close().ok() && operations.close_calls == close_after, "non-owner handle performed cleanup");
  ista::IncrementalStaSession final_attempt(cpuFullRequest());
  require(final_attempt.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kQuarantined, "sticky quarantine accepted typed init");
  requireThrows<std::runtime_error>([&] { interface.destroySTA(); }, "sticky quarantine accepted legacy destroy");
  require(operations.initialize_calls == init_before && operations.close_calls == close_after, "sticky quarantine called native operations");
}

void legacyWorkFailures()
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  for (LegacyPipeline pipeline : {LegacyPipeline::kRun, LegacyPipeline::kExtract, LegacyPipeline::kUpdate, LegacyPipeline::kReport, LegacyPipeline::kWrite}) {
    operations.resetResults();
    interface.initSTA({});
    prepareLegacyPipeline(interface, pipeline);
    const int calls_before = pipelineCalls(operations, pipeline);
    setPipelineResult(operations, pipeline, ista::StaOperationResult::workFailure("injected legacy work failure"));
    requireLegacyPipelineFailure(interface, pipeline);
    require(pipelineCalls(operations, pipeline) == calls_before + 1, "legacy work callback count");
    require(interface.lifecycleStateForTesting() == "legacy", "legacy work failure lost owner");
    interface.destroySTA();
    operations.resetResults();
    interface.initSTA({});
    interface.destroySTA();
  }
  ista::STAInterface::destroyInst();
}

void legacyCleanupFailure(LegacyPipeline pipeline)
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  interface.initSTA({});
  prepareLegacyPipeline(interface, pipeline);
  setPipelineResult(operations, pipeline, ista::StaOperationResult::cleanupFailure("injected legacy cleanup failure"));
  requireLegacyPipelineFailure(interface, pipeline);
  require(interface.lifecycleStateForTesting() == "quarantined(legacy)", "legacy cleanup failure lost cleanup owner");
  const int init_before = operations.initialize_calls;
  ista::IncrementalStaSession typed(cpuFullRequest());
  require(typed.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kQuarantined, "typed init bypassed legacy quarantine");
  requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "legacy init bypassed current quarantine");
  require(operations.initialize_calls == init_before, "legacy quarantine init reached callback");
  interface.destroySTA();
  require(interface.lifecycleStateForTesting() == "quarantined(none)", "legacy close downgraded quarantine");
  const int close_after = operations.close_calls;
  requireThrows<std::runtime_error>([&] { interface.destroySTA(); }, "sticky legacy close unexpectedly succeeded");
  requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "sticky quarantine accepted legacy init");
  require(operations.initialize_calls == init_before && operations.close_calls == close_after, "sticky legacy quarantine triggered callbacks");
}

void closeCleanupFailure(bool legacy)
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  operations.close_result = ista::StaOperationResult::cleanupFailure("injected persistent close failure");
  if (legacy) {
    interface.initSTA({});
    requireThrows<std::runtime_error>([&] { interface.destroySTA(); }, "legacy close cleanup failure not reported");
  } else {
    ista::IncrementalStaSession session(cpuFullRequest());
    require(session.initialize(testAnalysisContext(), {}).ok(), "typed close setup");
    require(session.close().code == ista::StaSessionStatusCode::kCleanupFailed, "typed close cleanup status");
    const int close_after = operations.close_calls;
    require(session.close().ok() && operations.close_calls == close_after, "typed persistent close repeated");
  }
  require(interface.lifecycleStateForTesting() == "quarantined(none)", "close cleanup failure not sticky");
  const int init_before = operations.initialize_calls;
  const int close_before = operations.close_calls;
  ista::IncrementalStaSession next(cpuFullRequest());
  require(next.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kQuarantined, "typed init bypassed close quarantine");
  requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "legacy init bypassed close quarantine");
  requireThrows<std::runtime_error>([&] { interface.destroySTA(); }, "legacy destroy repeated close cleanup");
  require(operations.initialize_calls == init_before && operations.close_calls == close_before, "close quarantine triggered callbacks");
}

void initializationThrow(bool legacy, bool cleanup_throws)
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  operations.initialize_throw = legacy ? ThrowKind::kUnknown : ThrowKind::kStandard;
  operations.close_throw = cleanup_throws ? (legacy ? ThrowKind::kStandard : ThrowKind::kUnknown) : ThrowKind::kNone;
  if (legacy) {
    requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "legacy initialize throw was not converted");
    if (!cleanup_throws)
      interface.destroySTA();
  } else {
    ista::IncrementalStaSession failed(cpuFullRequest());
    ista::StaStatus status = failed.initialize(testAnalysisContext(), {});
    require(status.code == (cleanup_throws ? ista::StaSessionStatusCode::kCleanupFailed : ista::StaSessionStatusCode::kInitFailed),
            "typed initialize throw status");
    require(failed.close().ok(), "typed failed initialize close is idempotent");
  }
  require(operations.initialize_calls == 1 && operations.close_calls == 1, "initialize throw rollback must run exactly once");
  if (cleanup_throws) {
    require(interface.lifecycleStateForTesting() == "quarantined(none)", "throwing init rollback must quarantine without owner");
    const int initialize_before = operations.initialize_calls;
    const int close_before = operations.close_calls;
    ista::IncrementalStaSession blocked(cpuFullRequest());
    require(blocked.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kQuarantined, "typed init bypassed throw rollback quarantine");
    requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "legacy init bypassed throw rollback quarantine");
    require(operations.initialize_calls == initialize_before && operations.close_calls == close_before, "throw rollback quarantine reached native callbacks");
    return;
  }
  require(interface.lifecycleStateForTesting() == "none", "successful throw rollback did not release owner");
  operations.resetResults();
  if (legacy) {
    interface.initSTA({});
    interface.destroySTA();
  } else {
    ista::IncrementalStaSession next(cpuFullRequest());
    require(next.initialize(testAnalysisContext(), {}).ok() && next.close().ok(), "typed re-init after thrown initialize rollback");
  }
  ista::STAInterface::destroyInst();
}

void typedRunThrow()
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  ista::IncrementalStaSession session(cpuFullRequest());
  require(session.initialize(testAnalysisContext(), {}).ok(), "typed run throw setup");
  operations.full_throw = ThrowKind::kUnknown;
  ista::StaExecutionResult result = session.runFullBaseline();
  require(result.status.code == ista::StaSessionStatusCode::kCleanupFailed && session.state() == ista::StaSessionState::kPoisoned,
          "unconfirmed typed run cleanup must poison and report cleanup failure");
  require(interface.lifecycleStateForTesting().starts_with("quarantined(session("), "typed run throw did not retain cleanup owner");
  const int initialize_before = operations.initialize_calls;
  ista::IncrementalStaSession blocked(cpuFullRequest());
  require(blocked.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kQuarantined, "typed init bypassed run throw quarantine");
  require(session.close().ok(), "typed run throw owner close");
  require(interface.lifecycleStateForTesting() == "quarantined(none)", "typed run throw close downgraded quarantine");
  const int close_after = operations.close_calls;
  require(session.close().ok() && operations.close_calls == close_after, "typed run throw repeated persistent close");
  ista::IncrementalStaSession final_attempt(cpuFullRequest());
  require(final_attempt.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kQuarantined, "sticky run throw quarantine accepted init");
  require(operations.initialize_calls == initialize_before, "run throw quarantine reached initialize callback");
}

void legacyPipelineThrow(LegacyPipeline pipeline)
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  interface.initSTA({});
  prepareLegacyPipeline(interface, pipeline);
  const int calls_before = pipelineCalls(operations, pipeline);
  setPipelineThrow(operations, pipeline, pipeline == LegacyPipeline::kExtract ? ThrowKind::kUnknown : ThrowKind::kStandard);
  requireLegacyPipelineFailure(interface, pipeline);
  require(pipelineCalls(operations, pipeline) == calls_before + 1, "legacy throwing pipeline callback count");
  require(interface.lifecycleStateForTesting() == "quarantined(legacy)", "legacy pipeline throw did not retain cleanup owner");
  const int initialize_before = operations.initialize_calls;
  ista::IncrementalStaSession blocked(cpuFullRequest());
  require(blocked.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kQuarantined, "typed init bypassed legacy throw quarantine");
  interface.destroySTA();
  require(interface.lifecycleStateForTesting() == "quarantined(none)", "legacy throw close downgraded quarantine");
  const int close_after = operations.close_calls;
  requireThrows<std::runtime_error>([&] { interface.destroySTA(); }, "sticky legacy throw close unexpectedly succeeded");
  requireThrows<std::runtime_error>([&] { interface.initSTA({}); }, "sticky legacy throw quarantine accepted init");
  require(operations.initialize_calls == initialize_before && operations.close_calls == close_after, "legacy throw quarantine reached native callbacks");
}

void closeThrow(bool legacy)
{
  FakeProductionOperations operations;
  ista::STAInterface& interface = install(operations);
  operations.close_throw = legacy ? ThrowKind::kUnknown : ThrowKind::kStandard;
  if (legacy) {
    interface.initSTA({});
    requireThrows<std::runtime_error>([&] { interface.destroySTA(); }, "legacy close throw was not converted");
  } else {
    ista::IncrementalStaSession session(cpuFullRequest());
    require(session.initialize(testAnalysisContext(), {}).ok(), "typed close throw setup");
    require(session.close().code == ista::StaSessionStatusCode::kCleanupFailed, "typed close throw status");
    const int close_after = operations.close_calls;
    require(session.close().ok() && operations.close_calls == close_after, "typed close throw repeated cleanup");
  }
  require(interface.lifecycleStateForTesting() == "quarantined(none)", "close throw must consume owner into sticky quarantine");
  const int initialize_before = operations.initialize_calls;
  const int close_before = operations.close_calls;
  ista::IncrementalStaSession blocked(cpuFullRequest());
  require(blocked.initialize(testAnalysisContext(), {}).code == ista::StaSessionStatusCode::kQuarantined, "typed init bypassed close throw quarantine");
  requireThrows<std::runtime_error>([&] { interface.destroySTA(); }, "sticky close throw repeated cleanup");
  require(operations.initialize_calls == initialize_before && operations.close_calls == close_before, "close throw quarantine reached callbacks");
}

LegacyPipeline parsePipeline(std::string_view name)
{
  if (name == "run")
    return LegacyPipeline::kRun;
  if (name == "extract")
    return LegacyPipeline::kExtract;
  if (name == "update")
    return LegacyPipeline::kUpdate;
  if (name == "report")
    return LegacyPipeline::kReport;
  if (name == "write")
    return LegacyPipeline::kWrite;
  throw std::invalid_argument("unknown legacy pipeline scenario");
}

}  // namespace

int main(int argc, char* argv[])
{
  if (argc != 2)
    throw std::invalid_argument("expected exactly one production lifecycle scenario");
  const std::string_view scenario = argv[1];
  if (scenario == "owner_interop")
    ownerInterop();
  else if (scenario == "init_work_failure")
    initializationWorkFailure();
  else if (scenario == "typed_init_cleanup_failure")
    initializationCleanupFailure(false);
  else if (scenario == "legacy_init_cleanup_failure")
    initializationCleanupFailure(true);
  else if (scenario == "typed_run_work_failure")
    typedRunFailure(false);
  else if (scenario == "typed_run_cleanup_failure")
    typedRunFailure(true);
  else if (scenario == "legacy_work_failures")
    legacyWorkFailures();
  else if (scenario.starts_with("legacy_cleanup_"))
    legacyCleanupFailure(parsePipeline(scenario.substr(15)));
  else if (scenario == "typed_close_cleanup_failure")
    closeCleanupFailure(false);
  else if (scenario == "legacy_close_cleanup_failure")
    closeCleanupFailure(true);
  else if (scenario == "typed_init_throw")
    initializationThrow(false, false);
  else if (scenario == "legacy_init_throw")
    initializationThrow(true, false);
  else if (scenario == "typed_init_throw_cleanup_throw")
    initializationThrow(false, true);
  else if (scenario == "legacy_init_throw_cleanup_throw")
    initializationThrow(true, true);
  else if (scenario == "typed_run_throw")
    typedRunThrow();
  else if (scenario.starts_with("legacy_throw_"))
    legacyPipelineThrow(parsePipeline(scenario.substr(13)));
  else if (scenario == "typed_close_throw")
    closeThrow(false);
  else if (scenario == "legacy_close_throw")
    closeThrow(true);
  else
    throw std::invalid_argument("unknown production lifecycle scenario");
  return 0;
}
