// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "BufferFixedTraceGenerator.hpp"

#include <array>
#include <algorithm>
#include <utility>
#include <variant>

#include "BufferReplayContext.hpp"
#include "BufferTraceReplayer.hpp"
#include "Database.hpp"
#include "GreedyBufferCandidateSelector.hpp"
#include "NativeBufferObservationExtractor.hpp"

namespace ista::buffer_trace {
namespace {

[[noreturn]] void fail(BufferGenerationFailureCode code, std::string message)
{
  throw BufferGenerationError(code, std::move(message));
}

std::vector<timing_edit::EditBatch>& mutableBatches(ReplayProgram& program)
{
  return std::visit(
      [](auto& payload) -> std::vector<timing_edit::EditBatch>& {
        return payload.batches;
      },
      program.workload);
}

const std::vector<timing_edit::EditBatch>& replayBatches(
    const ReplayProgram& program)
{
  return std::visit(
      [](const auto& payload) -> const std::vector<timing_edit::EditBatch>& {
        return payload.batches;
      },
      program.workload);
}

void bindGenerationBaseline(
    ReplayProgram& program,
    const timing_edit::BaselineSemanticIdentity& actual_baseline)
{
  std::visit([&](auto& payload) { payload.baseline = actual_baseline; }, program.workload);
}

std::vector<timing_edit::CreationToken> reservedTokens(
    const timing_edit::TimingEditResolverState& resolver)
{
  std::vector<timing_edit::CreationToken> result;
  result.reserve(resolver.materializations().size());
  for (const auto& [token_value, materialization] : resolver.materializations()) {
    static_cast<void>(materialization);
    result.push_back(token_value);
  }
  return result;
}

std::vector<std::string> reservedNames(
    const Database& database,
    const timing_edit::TimingEditResolverState& resolver)
{
  std::vector<std::string> result;
  result.reserve(database.get_instance_map().size()
                 + resolver.nodeBindings().size()
                 + resolver.netBindings().size()
                 + resolver.materializations().size() * 4);
  for (const auto& [name, instance] : database.get_instance_map()) {
    static_cast<void>(instance);
    if (!name.empty()) {
      result.push_back(name);
    }
  }
  for (const auto& [ref, binding] : resolver.nodeBindings()) {
    static_cast<void>(ref);
    if (!binding.native_name.empty()) {
      result.push_back(binding.native_name);
    }
  }
  for (const auto& [ref, binding] : resolver.netBindings()) {
    static_cast<void>(ref);
    if (!binding.native_name.empty()) {
      result.push_back(binding.native_name);
    }
  }
  for (const auto& [token_value, materialization] : resolver.materializations()) {
    static_cast<void>(token_value);
    const std::array<std::string_view, 4> names{
        materialization.instance_name, materialization.output_net_name,
        materialization.input_pin_name, materialization.output_pin_name};
    for (const std::string_view name : names) {
      if (!name.empty()) result.emplace_back(name);
    }
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

timing_edit::EditBatch makePlan(
    timing_edit::TransactionGeneration generation,
    const GreedySelection& selection)
{
  timing_edit::EditBatch plan{.expected_generation
                                   = generation,
                                   .edits = {}};
  plan.edits.reserve(selection.accepted.size());
  for (const SelectedBufferCandidate& selected : selection.accepted) {
    plan.edits.push_back(timing_edit::BufferInsertionMutation{
        .original_net = selected.candidate.net,
        .moved_sinks = selected.candidate.moved_sinks,
        .library_cell = selected.candidate.buffer,
        .placement_inputs = selected.candidate.placement,
        .rc_inputs = selected.candidate.rc,
        .creation_token = selected.creation_token});
  }
  return plan;
}

void requireInitialTemplate(const BufferTraceGenerationRequest& request)
{
  if (const auto violations = validateReplayTemplate(request.replay_template);
      !violations.empty()) {
    fail(BufferGenerationFailureCode::kInvalidRequest,
         violations.front().field + ": " + violations.front().message);
  }
  if (request.case_id.empty() || request.case_id.find('\0') != std::string::npos) {
    fail(BufferGenerationFailureCode::kInvalidRequest,
         "case_id must be nonempty and contain no NUL");
  }
  if (request.replay_template.header.profile_id != request.profile.id) {
    fail(BufferGenerationFailureCode::kInvalidRequest,
         "replay header profile differs from the generation profile");
  }
  if (!replayBatches(request.replay_template).empty()) {
    fail(BufferGenerationFailureCode::kInvalidRequest,
         "generation replay template must start with no batches");
  }
  if (const auto violations = validateSelectionProfile(request.profile);
      !violations.empty()) {
    fail(BufferGenerationFailureCode::kInvalidRequest,
         violations.front().field + ": " + violations.front().message);
  }
  if (request.replay_template.header.workload_kind
          == SemanticWorkloadKind::kRealDesign
      && request.seed != 0) {
    fail(BufferGenerationFailureCode::kInvalidRequest,
         "real-design generation seed must be zero");
  }
}

}  // namespace

const char* token(BufferGenerationFailureCode code) noexcept
{
  switch (code) {
    case BufferGenerationFailureCode::kInvalidRequest:
      return "invalid_generation_request";
    case BufferGenerationFailureCode::kObservationFailed:
      return "native_observation_failed";
    case BufferGenerationFailureCode::kSelectionFailed:
      return "greedy_selection_failed";
    case BufferGenerationFailureCode::kReplayFailed:
      return "generation_replay_failed";
    case BufferGenerationFailureCode::kInvalidArtifact:
      return "invalid_generated_artifact";
  }
  return "unknown_generation_failure";
}

BufferGenerationError::BufferGenerationError(BufferGenerationFailureCode code,
                                             std::string message)
    : std::runtime_error(std::string(token(code)) + ": " + std::move(message)),
      _code(code)
{
}

BufferTraceGenerationResult BufferFixedTraceGenerator::generate(
    FreshBaselineRunner& runner, BufferTraceGenerationRequest request)
{
  requireInitialTemplate(request);
  BufferTraceArtifact artifact{.replay = std::move(request.replay_template),
                               .generation_only
                               = GenerationReceipt{.profile = request.profile,
                                                   .rounds = {},
                                                   .stop_reason = GenerationStopReason::kEmptyAcceptedBatch}};
  std::vector<FunctionalBatchReceipt> batch_receipts;
  batch_receipts.reserve(request.profile.rounds);
  mutableBatches(artifact.replay).reserve(request.profile.rounds);
  artifact.generation_only.rounds.reserve(request.profile.rounds);
  ReplayReceipt functional = runner.withFreshGenerationBaseline(
      artifact.replay, [&](BufferReplayContext& context) {
        try {
        bindGenerationBaseline(artifact.replay,
                                   *context._baseline);
        std::uint64_t inserted = 0;
        bool stopped = false;
        for (std::uint64_t round = 1; round <= request.profile.rounds; ++round) {
          SelectionProfile round_profile = request.profile;
          const std::uint64_t remaining
              = request.profile.max_total_insertions - inserted;
          std::vector<BufferCandidateObservation> observations;
          try {
            observations = NativeBufferObservationExtractor::extract(
                context.database(), context._resolver,
                round_profile);
          } catch (const NativeObservationError& error) {
            fail(BufferGenerationFailureCode::kObservationFailed, error.what());
          }
          GreedySelectionRequest selection_request{
              .profile = round_profile,
              .case_id = request.case_id,
              .seed = request.seed,
              .batch_ordinal = round,
              .max_acceptances = remaining,
              .candidates = std::move(observations),
              .reserved_creation_tokens
              = reservedTokens(context._resolver),
              .reserved_materialized_names
              = reservedNames(context.database(),
                              context._resolver)};
          GreedySelectionResult selected
              = GreedyBufferCandidateSelector{}.select(selection_request);
          if (const auto* error = std::get_if<GreedySelectionError>(&selected)) {
            fail(BufferGenerationFailureCode::kSelectionFailed,
                 error->field + ": " + error->message);
          }
          GreedySelection selection
              = std::get<GreedySelection>(std::move(selected));
          artifact.generation_only.rounds.push_back(selection.receipt);
          if (selection.accepted.empty()) {
            if (batch_receipts.empty()) {
              fail(BufferGenerationFailureCode::kInvalidArtifact,
                   "empty_accepted_batch: no replayable batch was generated");
            }
            artifact.generation_only.stop_reason
                = GenerationStopReason::kEmptyAcceptedBatch;
            stopped = true;
            break;
          }
          timing_edit::EditBatch plan
              = makePlan(context._identity.generation, selection);
          FunctionalBatchReceipt applied
              = BufferTraceReplayer::applyNextBatch(context, plan, round - 1);
          mutableBatches(artifact.replay).push_back(std::move(plan));
          inserted += selection.accepted.size();
          batch_receipts.push_back(std::move(applied));
          if (inserted >= request.profile.max_total_insertions) {
            artifact.generation_only.stop_reason
                = GenerationStopReason::kMaxTotalInsertionsReached;
            stopped = true;
            break;
          }
        }
        if (!stopped) {
          artifact.generation_only.stop_reason
              = GenerationStopReason::kRoundsReached;
        }
        const std::size_t generated_batches = replayBatches(artifact.replay).size();
        return BufferTraceReplayer::finishCurrent(
            context, artifact.replay, generated_batches, std::move(batch_receipts));
        } catch (const BufferReplayError& error) {
          fail(BufferGenerationFailureCode::kReplayFailed, error.what());
        }
      });

  if (const auto violations = validateBufferTraceArtifact(artifact);
      !violations.empty()) {
    fail(BufferGenerationFailureCode::kInvalidArtifact,
         violations.front().field + ": " + violations.front().message);
  }
  return {std::move(artifact), std::move(functional)};
}

}  // namespace ista::buffer_trace
