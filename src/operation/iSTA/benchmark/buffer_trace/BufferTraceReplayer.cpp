// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "BufferTraceReplayer.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <ranges>
#include <utility>
#include <variant>

#include "BufferReplayContext.hpp"
#include "CsrSessionSnapshotAdapter.hpp"
#include "Database.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeDatabaseCandidate.hpp"
#include "NativeDesignEditTranslator.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista::buffer_trace {
namespace {

[[noreturn]] void fail(BufferReplayFailureCode code, std::string message)
{
  throw BufferReplayError(code, std::move(message));
}

template <typename Function>
decltype(auto) invokeTypedPhase(BufferReplayFailureCode code,
                                const char* operation, Function&& function)
{
  try {
    return std::forward<Function>(function)();
  } catch (const BufferReplayError&) {
    throw;
  } catch (const timing_edit::NativeExecutionRollbackError&) {
    throw;
  } catch (const std::exception& error) {
    fail(code, std::string(operation) + " threw: " + error.what());
  } catch (...) {
    fail(code, std::string(operation) + " threw a non-standard exception");
  }
}

bool noOpRollback(void*) noexcept
{
  return true;
}

bool failedRollback(void*) noexcept
{
  return false;
}

timing_edit::TransactionStatus rollbackCoordinator(
    timing_edit::TimingEditCoordinator& coordinator,
    bool rollback_succeeded) noexcept
{
  if (coordinator.state() == timing_edit::TimingEditCoordinatorState::kPrepared) {
    return coordinator.cancelPrepared();
  }
  bool marker = rollback_succeeded;
  const timing_edit::PrecommitRollbackSignal signal{
      &marker, rollback_succeeded ? &noOpRollback : &failedRollback};
  return coordinator.rollbackPrecommit(signal);
}

const std::vector<timing_edit::DesignEditPlan>& batches(const ReplayProgram& program)
{
  return std::visit(
      [](const auto& payload) -> const std::vector<timing_edit::DesignEditPlan>& {
        return payload.batches;
      },
      program.workload);
}

NativeDatabaseFunctionalReceipt captureNativeDatabase(Database& database)
{
  NativeDatabaseFunctionalReceipt receipt;
  receipt.design_name = database.get_design_name();
  receipt.instances.reserve(database.get_instance_map().size());
  for (const auto& [name, instance] : database.get_instance_map()) {
    NativeInstanceFunctionalRecord record{
        name, instance.get_cell_name(), instance.get_pin_name_list(),
        instance.get_is_sequential(), instance.get_is_clock_gating(),
        instance.get_is_io_cell()};
    std::ranges::sort(record.ordered_pins);
    receipt.instances.push_back(std::move(record));
  }
  receipt.pins.reserve(database.get_pin_map().size());
  for (const auto& [name, pin] : database.get_pin_map()) {
    if (name != pin.get_full_name()) {
      fail(BufferReplayFailureCode::kCsrSnapshotFailed,
           "native pin map key differs from its full name");
    }
    receipt.pins.push_back(
        {pin.get_full_name(), pin.get_pin_name(), pin.get_instance_name(),
         pin.get_net_name(), static_cast<std::int32_t>(pin.get_direction()),
         pin.get_is_port()});
  }
  receipt.nets.reserve(database.get_net_map().size());
  for (const auto& [name, net] : database.get_net_map()) {
    NativeNetFunctionalRecord record{name, net.get_driver_pin_list(),
                                     net.get_load_pin_list(),
                                     net.get_pin_name_list()};
    std::ranges::sort(record.ordered_drivers);
    std::ranges::sort(record.ordered_loads);
    std::ranges::sort(record.ordered_pins);
    receipt.nets.push_back(std::move(record));
  }
  return receipt;
}

struct CsrFunctionalState
{
  csr::CsrReferenceInput input;
  csr::CsrTimingResult timing;
};

CsrFunctionalState computeFunctionalState(
    csr::CsrCommittedSessionSnapshot& snapshot)
{
  csr::CsrComputationResult computation = invokeTypedPhase(
      BufferReplayFailureCode::kCsrComputationFailed,
      "computeCsrSessionReference", [&] {
        return csr::computeCsrSessionReference(snapshot);
      });
  auto* timing = std::get_if<csr::CsrTimingResult>(&computation.outcome);
  if (timing == nullptr || timing->source_identity != snapshot.input.source_identity
      || !snapshot.input.unsupported_reasons.empty()) {
    fail(BufferReplayFailureCode::kCsrComputationFailed,
         "CPU/full CSR did not return a supported result for the exact input");
  }
  return {snapshot.input, std::move(*timing)};
}

std::unique_ptr<csr::CsrCommittedSessionSnapshot> buildHostSnapshot(
    const Database& database, const csr::StaAnalysisContext& analysis,
    timing_edit::TimingEditSessionToken token_value)
{
  csr::CsrSessionSnapshotBuildResult built = invokeTypedPhase(
      BufferReplayFailureCode::kCsrSnapshotFailed,
      "buildCsrSessionSnapshot", [&] {
        return csr::buildCsrSessionSnapshot(database, analysis,
                                            token_value.value());
      });
  if (const auto* code = std::get_if<csr::CsrCaptureCode>(&built)) {
    std::string detail = "host CSR snapshot build rejected the native Database: "
                         + std::string(csr::token(*code));
    const csr::CsrReferenceInputExportResult exported = invokeTypedPhase(
        BufferReplayFailureCode::kCsrSnapshotFailed,
        "exportCsrReferenceInputCandidate diagnostic", [&] {
          return csr::exportCsrReferenceInputCandidate(
              database, analysis, token_value.value());
        });
    if (const auto* failure
        = std::get_if<csr::CsrReferenceInputExportFailure>(&exported)) {
      detail += " at reference export phase "
                + std::to_string(static_cast<std::uint8_t>(failure->failed_phase));
    }
    fail(BufferReplayFailureCode::kCsrSnapshotFailed,
         std::move(detail));
  }
  auto* snapshot
      = std::get_if<std::unique_ptr<csr::CsrCommittedSessionSnapshot>>(&built);
  if (snapshot == nullptr || *snapshot == nullptr)
    fail(BufferReplayFailureCode::kCsrSnapshotFailed, "host CSR snapshot is null");
  return std::move(*snapshot);
}

void requireOk(timing_edit::TransactionStatus status,
               BufferReplayFailureCode code, const char* operation)
{
  if (!status.ok()) {
    fail(code, std::string(operation) + " returned transaction status "
                   + std::to_string(static_cast<std::uint32_t>(status.code)));
  }
}

}  // namespace

const char* token(BufferReplayFailureCode code) noexcept
{
  switch (code) {
    case BufferReplayFailureCode::kInvalidProgram:
      return "invalid_replay_program";
    case BufferReplayFailureCode::kPrefixOutsideTrace:
      return "prefix_outside_trace";
    case BufferReplayFailureCode::kContextMismatch:
      return "replay_context_mismatch";
    case BufferReplayFailureCode::kGenerationMismatch:
      return "replay_generation_mismatch";
    case BufferReplayFailureCode::kPrepareRejected:
      return "prepare_rejected";
    case BufferReplayFailureCode::kTranslationRejected:
      return "translation_rejected";
    case BufferReplayFailureCode::kNativeExecutionFailed:
      return "native_execution_failed";
    case BufferReplayFailureCode::kCsrSnapshotFailed:
      return "csr_snapshot_failed";
    case BufferReplayFailureCode::kCsrComputationFailed:
      return "csr_computation_failed";
    case BufferReplayFailureCode::kPublishRejected:
      return "publish_rejected";
  }
  return "unknown_replay_failure";
}

BufferReplayError::BufferReplayError(BufferReplayFailureCode code,
                                     std::string message)
    : std::runtime_error(std::string(token(code)) + ": " + std::move(message)),
      _code(code)
{
}

FunctionalBatchReceipt BufferTraceReplayer::applyNextBatch(
    BufferReplayContext& context, const timing_edit::DesignEditPlan& plan,
    std::size_t batch_ordinal)
{
  invokeTypedPhase(BufferReplayFailureCode::kContextMismatch,
                   "BufferReplayContext::requireActive", [&] {
                     context.requireActive();
                   });
  auto& coordinator = context.coordinator();
  if (coordinator.state() != timing_edit::TimingEditCoordinatorState::kIdle
      || plan.baseline != coordinator.baseline()
      || plan.expected_generation != coordinator.committedIdentity().generation) {
    fail(BufferReplayFailureCode::kGenerationMismatch,
         "batch baseline/generation differs before the first write");
  }

  const timing_edit::TimingEditPrepareContext prepare_context{
      coordinator.sessionToken(), coordinator.baseline(),
      coordinator.committedIdentity(), context.analysisContext()};
  timing_edit::PreparedBatch prepared = invokeTypedPhase(
      BufferReplayFailureCode::kPrepareRejected,
      "NativeTimingEditPreparer::prepare", [&] {
        return timing_edit::NativeTimingEditPreparer::prepare(
            context.database(), plan, prepare_context,
            coordinator.resolverState());
      });

  timing_edit::TimingEditResolverState staged;
  try {
    invokeTypedPhase(BufferReplayFailureCode::kPrepareRejected,
                     "beginPrepare", [&] {
                       requireOk(coordinator.beginPrepare(prepared),
                                 BufferReplayFailureCode::kPrepareRejected,
                                 "beginPrepare");
                     });
    invokeTypedPhase(BufferReplayFailureCode::kPrepareRejected,
                     "resolver staging", [&] {
                       staged = coordinator.resolverState().stage(prepared);
                     });
    invokeTypedPhase(BufferReplayFailureCode::kPrepareRejected,
                     "markStaged", [&] {
                       requireOk(coordinator.markStaged(),
                                 BufferReplayFailureCode::kPrepareRejected,
                                 "markStaged");
                     });
    timing_edit::NativeDatabaseCandidate candidate = invokeTypedPhase(
        BufferReplayFailureCode::kTranslationRejected,
        "NativeDatabaseCandidate::create", [&] {
          return timing_edit::NativeDatabaseCandidate::create(
              context.database());
        });
    const timing_edit::NativeTranslationResult translated = invokeTypedPhase(
        BufferReplayFailureCode::kTranslationRejected,
        "NativeDesignEditTranslator::applyNormalizedBatch", [&] {
          return timing_edit::NativeDesignEditTranslator::applyNormalizedBatch(
              candidate.database(), prepared, staged);
        });
    if (!translated.applied() || !translated.reasons.empty()) {
      rollbackCoordinator(coordinator, true);
      fail(BufferReplayFailureCode::kTranslationRejected,
           "normalized Stage 3 translation was not exactly supported");
    }
    invokeTypedPhase(BufferReplayFailureCode::kNativeExecutionFailed,
                     "markExecuting", [&] {
                       requireOk(coordinator.markExecuting(),
                                 BufferReplayFailureCode::kNativeExecutionFailed,
                                 "markExecuting");
                     });

    timing_edit::InstalledNativeDatabase installed = invokeTypedPhase(
        BufferReplayFailureCode::kNativeExecutionFailed,
        "NativeCandidateExecutor::installAndExecute", [&] {
          return timing_edit::NativeCandidateExecutor::installAndExecute(
              context.authority(), std::move(candidate));
        });
    bool installed_ownership_transferred = false;
    try {
      auto host_snapshot = buildHostSnapshot(
          installed.database(), context.analysisContext(),
          coordinator.sessionToken());
      CsrFunctionalState csr_state = computeFunctionalState(*host_snapshot);
      const timing_edit::CommittedEditIdentity post_identity = invokeTypedPhase(
          BufferReplayFailureCode::kCsrSnapshotFailed,
          "captureCurrentIdentity", [&] {
            return timing_edit::NativeTimingEditPreparer::captureCurrentIdentity(
                installed.database(), prepared.targetGeneration(),
                context.analysisContext(), coordinator.sessionToken());
          });

      FunctionalBatchReceipt receipt = invokeTypedPhase(
          BufferReplayFailureCode::kPublishRejected,
          "functional receipt capture", [&] {
            return FunctionalBatchReceipt{
                .batch_ordinal = batch_ordinal,
                .expected_generation = prepared.expectedGeneration(),
                .target_generation = prepared.targetGeneration(),
                .normalized_edits = prepared.normalizedEdits(),
                .forward = prepared.forwardDelta(),
                .reverse = prepared.reverseDelta(),
                .materializations = prepared.tokenMaterializations(),
                .post_identity = post_identity,
                .csr_input = std::move(csr_state.input),
                .csr_timing = std::move(csr_state.timing),
                .native_database = captureNativeDatabase(installed.database())};
          });
      timing_edit::CumulativeEditTrace cumulative = invokeTypedPhase(
          BufferReplayFailureCode::kPublishRejected,
          "cumulative trace construction", [&] {
            timing_edit::CumulativeEditTrace value = coordinator.cumulativeTrace();
            value.batches.push_back(timing_edit::DesignEditPlan{
                .baseline = coordinator.baseline(),
                .expected_generation = prepared.expectedGeneration(),
                .edits = prepared.normalizedEdits()});
            return value;
          });
      PendingVerifiedBatch pending(
          std::move(installed), std::move(host_snapshot), post_identity,
          std::move(prepared), std::move(cumulative), std::move(staged));
      installed_ownership_transferred = true;
      invokeTypedPhase(BufferReplayFailureCode::kPublishRejected,
                       "proveSealAndPublish", [&] {
                         context.proveSealAndPublish(std::move(pending));
                       });
      if (coordinator.state() != timing_edit::TimingEditCoordinatorState::kIdle
          || coordinator.committedIdentity() != receipt.post_identity) {
        fail(BufferReplayFailureCode::kPublishRejected,
             "verified publish did not install the exact post identity");
      }
      return receipt;
    } catch (...) {
      // Before PendingVerifiedBatch takes ownership, InstalledNativeDatabase
      // must be rolled back explicitly; its destructor is diagnostic only.
      if (installed_ownership_transferred) {
        throw;
      }
      const std::exception_ptr primary_failure = std::current_exception();
      const bool native_rollback_succeeded = installed.rollback();
      const timing_edit::TransactionStatus transaction_rollback
          = rollbackCoordinator(coordinator, native_rollback_succeeded);
      if (!native_rollback_succeeded) {
        if (transaction_rollback.code
                != timing_edit::TransactionStatusCode::kRollbackFailed
            || transaction_rollback.state
                   != timing_edit::TimingEditCoordinatorState::kPoisoned
            || coordinator.state()
                   != timing_edit::TimingEditCoordinatorState::kPoisoned) {
          fail(BufferReplayFailureCode::kNativeExecutionFailed,
               "prepublish native rollback failure did not poison the coordinator exactly");
        }
        fail(BufferReplayFailureCode::kNativeExecutionFailed,
             "post-execution verification failed; exact native rollback failed and session was poisoned");
      }
      if (!transaction_rollback.ok()
          || transaction_rollback.state
                 != timing_edit::TimingEditCoordinatorState::kIdle
          || coordinator.state() != timing_edit::TimingEditCoordinatorState::kIdle) {
        fail(BufferReplayFailureCode::kNativeExecutionFailed,
             "successful native rollback did not restore the idle coordinator");
      }
      std::rethrow_exception(primary_failure);
    }
  } catch (const timing_edit::NativeExecutionRollbackError& error) {
    const timing_edit::TransactionStatus rollback
        = rollbackCoordinator(coordinator, false);
    if (rollback.code != timing_edit::TransactionStatusCode::kRollbackFailed
        || rollback.state != timing_edit::TimingEditCoordinatorState::kPoisoned
        || coordinator.state()
               != timing_edit::TimingEditCoordinatorState::kPoisoned) {
      fail(BufferReplayFailureCode::kNativeExecutionFailed,
           "native rollback failure did not poison the coordinator exactly");
    }
    fail(BufferReplayFailureCode::kNativeExecutionFailed,
         std::string("native execution failed; exact rollback failed and session was poisoned: ")
             + error.what());
  } catch (const BufferReplayError&) {
    if (coordinator.state() == timing_edit::TimingEditCoordinatorState::kPrepared
        || coordinator.state() == timing_edit::TimingEditCoordinatorState::kStaged
        || coordinator.state() == timing_edit::TimingEditCoordinatorState::kExecuting
        || coordinator.state() == timing_edit::TimingEditCoordinatorState::kVerified) {
      rollbackCoordinator(coordinator, true);
    }
    throw;
  } catch (const std::exception& error) {
    if (coordinator.state() == timing_edit::TimingEditCoordinatorState::kPrepared
        || coordinator.state() == timing_edit::TimingEditCoordinatorState::kStaged
        || coordinator.state() == timing_edit::TimingEditCoordinatorState::kExecuting
        || coordinator.state() == timing_edit::TimingEditCoordinatorState::kVerified) {
      rollbackCoordinator(coordinator, true);
    }
    fail(BufferReplayFailureCode::kPublishRejected,
         std::string("unclassified post-prepare replay failure: ")
             + error.what());
  } catch (...) {
    if (coordinator.state() == timing_edit::TimingEditCoordinatorState::kPrepared
        || coordinator.state() == timing_edit::TimingEditCoordinatorState::kStaged
        || coordinator.state() == timing_edit::TimingEditCoordinatorState::kExecuting
        || coordinator.state() == timing_edit::TimingEditCoordinatorState::kVerified) {
      rollbackCoordinator(coordinator, true);
    }
    fail(BufferReplayFailureCode::kPublishRejected,
         "unclassified post-prepare replay failure was non-standard");
  }
}

ReplayReceipt BufferTraceReplayer::replayPrefix(
    BufferReplayContext& context, const ReplayProgram& program,
    std::size_t prefix_batches)
{
  if (const auto violations = validateReplayProgram(program);
      !violations.empty()) {
    fail(BufferReplayFailureCode::kInvalidProgram,
         violations.front().field + ": " + violations.front().message);
  }
  invokeTypedPhase(BufferReplayFailureCode::kContextMismatch,
                   "BufferReplayContext::requireActive", [&] {
                     context.requireActive();
                   });
  const auto& coordinator = context.coordinator();
  if (coordinator.state() != timing_edit::TimingEditCoordinatorState::kIdle
      || coordinator.committedIdentity().generation
             != timing_edit::TransactionGeneration{0}
      || !coordinator.cumulativeTrace().batches.empty()
      || coordinator.sessionToken() != context.authority().sessionToken()) {
    fail(BufferReplayFailureCode::kContextMismatch,
         "replay requires an idle generation-zero context from the fresh runner");
  }
  const auto& trace_batches = batches(program);
  if (prefix_batches > trace_batches.size()) {
    fail(BufferReplayFailureCode::kPrefixOutsideTrace,
         "requested cumulative prefix exceeds the fixed trace");
  }
  std::vector<FunctionalBatchReceipt> receipts;
  receipts.reserve(prefix_batches);
  for (std::size_t index = 0; index < prefix_batches; ++index) {
    receipts.push_back(applyNextBatch(context, trace_batches[index], index));
  }
  return finishCurrent(context, program, prefix_batches, std::move(receipts));
}

ReplayReceipt BufferTraceReplayer::finishCurrent(
    BufferReplayContext& context, const ReplayProgram& program,
    std::size_t expected_prefix,
    std::vector<FunctionalBatchReceipt> receipts)
{
  invokeTypedPhase(BufferReplayFailureCode::kContextMismatch,
                   "BufferReplayContext::requireActive", [&] {
                     context.requireActive();
                   });
  if (expected_prefix > batches(program).size()) {
    fail(BufferReplayFailureCode::kPrefixOutsideTrace,
         "finished prefix exceeds the fixed trace");
  }
  if (receipts.size() != expected_prefix
      || context.coordinator().committedIdentity().generation.value() != expected_prefix
      || context.coordinator().cumulativeTrace().batches.size() != expected_prefix) {
    fail(BufferReplayFailureCode::kGenerationMismatch,
         "functional batch receipts differ from the committed trace prefix");
  }
  auto final_snapshot = buildHostSnapshot(
      context.database(), context.analysisContext(),
      context.coordinator().sessionToken());
  CsrFunctionalState final_csr = computeFunctionalState(*final_snapshot);
  return ReplayReceipt{
      .header = program.header,
      .prefix_batches = expected_prefix,
      .batches = std::move(receipts),
      .committed_identity = context.coordinator().committedIdentity(),
      .cumulative_trace = context.coordinator().cumulativeTrace(),
      .resolver_state = context.coordinator().resolverState(),
      .final_database = captureNativeDatabase(context.database()),
      .final_csr_input = std::move(final_csr.input),
      .final_csr_timing = std::move(final_csr.timing)};
}

ReplayReceipt replayTracePrefix(BufferReplayContext& context,
                                const ReplayProgram& program,
                                std::size_t prefix_batches)
{
  return BufferTraceReplayer::replayPrefix(context, program, prefix_batches);
}

}  // namespace ista::buffer_trace
