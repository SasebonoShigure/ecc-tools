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
#include "NativeEditTransaction.hpp"
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

const std::vector<timing_edit::EditBatch>& batches(const ReplayProgram& program)
{
  return std::visit(
      [](const auto& payload) -> const std::vector<timing_edit::EditBatch>& {
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
    BufferReplayContext& context, const timing_edit::EditBatch& plan,
    std::size_t batch_ordinal)
{
  context.requireActive();
  if (plan.expected_generation != context._identity.generation)
    fail(BufferReplayFailureCode::kGenerationMismatch,
         "batch generation differs before the first write");
  const timing_edit::TimingEditPrepareContext prepare_context{
      context.authority().sessionToken(), context._baseline,
      context._identity, context.analysisContext()};
  auto prepared = invokeTypedPhase(
      BufferReplayFailureCode::kPrepareRejected, "prepare", [&] {
        return timing_edit::NativeTimingEditPreparer::prepare(
            context.database(), plan, prepare_context, context._resolver);
      });
  timing_edit::PreparedTimingEditResolverPatch resolver_patch(
      context._resolver, context._resolver.prepare(prepared));
  auto installed = invokeTypedPhase(
      BufferReplayFailureCode::kTranslationRejected, "prepareFull", [&] {
        return timing_edit::NativeEditTransaction::prepareFull(
            context.authority(), prepared, resolver_patch.view());
      });
  invokeTypedPhase(BufferReplayFailureCode::kNativeExecutionFailed,
                   "executeFull", [&] {
                     timing_edit::NativeCandidateExecutor::executeFull(installed);
                   });
  resolver_patch.apply();
  auto host = buildHostSnapshot(installed.database(), context.analysisContext(),
                                context.authority().sessionToken());
  CsrFunctionalState csr_state = computeFunctionalState(*host);
  invokeTypedPhase(BufferReplayFailureCode::kCsrComputationFailed,
                   "native/CSR comparison", [&] {
                     context._compare_timing(csr_state.input, csr_state.timing,
                                             installed.database());
                   });
  auto post_identity = timing_edit::NativeTimingEditPreparer::captureCurrentIdentity(
      installed.database(), prepared.targetGeneration(),
      context.analysisContext(), context.authority().sessionToken());
  const auto& batch = prepared;
  FunctionalBatchReceipt receipt{
      .batch_ordinal = batch_ordinal,
      .expected_generation = batch.expectedGeneration(),
      .target_generation = batch.targetGeneration(),
      .normalized_edits = batch.normalizedEdits(),
      .forward = batch.forwardDelta(),
      .reverse = batch.reverseDelta(),
      .materializations = batch.tokenMaterializations(),
      .post_identity = post_identity,
      .csr_input = std::move(csr_state.input),
      .csr_timing = std::move(csr_state.timing),
      .native_database = captureNativeDatabase(installed.database())};
  // All diagnostic allocations finish before these no-throw publications.
  installed.commit();
  resolver_patch.commit();
  std::swap(context._identity, post_identity);
  return receipt;
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
  if (context._identity.generation != timing_edit::TransactionGeneration{0}) {
    fail(BufferReplayFailureCode::kContextMismatch,
         "replay requires a generation-zero context from the fresh runner");
  }
  if (program.baseline() != *context._baseline)
    fail(BufferReplayFailureCode::kContextMismatch,
         "replay baseline differs from the fresh context before the first write");
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
      || context._identity.generation.value() != expected_prefix) {
    fail(BufferReplayFailureCode::kGenerationMismatch,
         "functional batch receipts differ from the committed trace prefix");
  }
  auto final_snapshot = buildHostSnapshot(
      context.database(), context.analysisContext(),
      context.authority().sessionToken());
  CsrFunctionalState final_csr = computeFunctionalState(*final_snapshot);
  return ReplayReceipt{
      .header = program.header,
      .prefix_batches = expected_prefix,
      .batches = std::move(receipts),
      .committed_identity = context._identity,
      .resolver_state = context._resolver,
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
