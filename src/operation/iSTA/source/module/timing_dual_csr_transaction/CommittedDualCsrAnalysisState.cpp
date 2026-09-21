// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "CommittedDualCsrAnalysisState.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>
#include <tuple>

#include "../../data_manager/adapter/csr/DualCsrSemanticIdentity.hpp"
#include "DualCsrArcGroupIndexValidation.hpp"
#include "DualCsrArcGroupResolverValidation.hpp"
#include "DualCsrStateValidation.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "TimingEditValidation.hpp"

namespace ista::timing_dual_csr {
namespace {

void reject(DualCsrViolations& errors, DualCsrViolationCode code,
            std::string field, std::string message)
{
  errors.push_back({code, std::move(field), std::move(message)});
}

void append(DualCsrViolations& target, DualCsrViolations source)
{
  target.insert(target.end(), std::make_move_iterator(source.begin()),
                std::make_move_iterator(source.end()));
}

}  // namespace

void CommittedDualCsrAnalysisState::swap(
    CommittedDualCsrAnalysisState& other) noexcept
{
  using std::swap;
  swap(graph, other.graph);
  swap(timing, other.timing);
  swap(resolver, other.resolver);
  swap(arc_group_index, other.arc_group_index);
  swap(identity, other.identity);
  swap(generation, other.generation);
  swap(descriptor_trace, other.descriptor_trace);
  swap(semantic_trace, other.semantic_trace);
  swap(traversal_telemetry, other.traversal_telemetry);
  swap(execution_trace, other.execution_trace);
  swap(baseline, other.baseline);
}

void swap(CommittedDualCsrAnalysisState& lhs,
          CommittedDualCsrAnalysisState& rhs) noexcept
{
  lhs.swap(rhs);
}

bool operator==(const CommittedDualCsrAnalysisState& lhs, const CommittedDualCsrAnalysisState& rhs)
{
  const auto values = [](const auto& state) {
    return std::tie(state.graph, state.timing, state.resolver, state.arc_group_index, state.identity,
                    state.generation, state.descriptor_trace, state.semantic_trace, state.traversal_telemetry, state.execution_trace);
  };
  return values(lhs) == values(rhs) && bool(lhs.baseline) == bool(rhs.baseline)
         && (!lhs.baseline || *lhs.baseline == *rhs.baseline);
}

namespace {

struct AnalysisView
{
  const DualCsrState& graph;
  const DualCsrTimingResult& timing;
  const timing_edit::TimingEditResolverState& resolver;
  const DualCsrArcGroupIndex& arc_group_index;
  const std::optional<timing_edit::CommittedEditIdentity>& identity;
  timing_edit::TransactionGeneration generation;
  const csr::CsrTraversalTelemetry& traversal_telemetry;
};

DualCsrViolations validateFunctionalState(const AnalysisView& state)
{
  DualCsrViolations errors = validateDualCsrState(state.graph);
  append(errors, validateDualCsrArcGroupIndex(state.graph, state.arc_group_index));
  append(errors, validateDualCsrArcGroupResolverFull(state.arc_group_index, state.resolver));
  append(errors, validateDualCsrTimingResult(state.graph, state.timing));
  if (state.generation != state.graph.generation)
    reject(errors, DualCsrViolationCode::kGeneration, "aggregate.generation",
           "aggregate and graph generation differ");
  if ((state.identity && state.generation != state.identity->generation)
      || ((incregpusta_debug_checks_enabled || state.generation.value() == 0) && !state.identity))
    reject(errors, DualCsrViolationCode::kGeneration, "aggregate.identity",
           "identity generation differs from aggregate generation");
  if (state.resolver.highWater() != state.graph.high_water)
    reject(errors, DualCsrViolationCode::kHighWater, "aggregate.resolver",
           "resolver and graph high-water marks differ");
  if (state.traversal_telemetry != state.graph.telemetry.traversal)
    reject(errors, DualCsrViolationCode::kTimingResult,
           "aggregate.traversal_telemetry",
           "aggregate and graph traversal telemetry differ");
  if (state.identity && !timing_edit::validateCommittedEditIdentity(
           *state.identity, "aggregate.identity").empty())
    reject(errors, DualCsrViolationCode::kConflict, "aggregate.identity",
           "committed edit identity is invalid");
  if (state.identity && deriveDualCsrCommittedIdentity(state.graph, state.resolver) != *state.identity)
    reject(errors, DualCsrViolationCode::kConflict,
           "aggregate.identity",
           "rank-free graph/resolver projection differs from identity");

  return errors;
}

void validateHistoryRecord(const AnalysisView& state, const timing_edit::EditBatch& semantic, const LevelizationExecutionTrace& execution,
                           const DualCsrBatchDescriptor* descriptor, std::uint64_t index, DualCsrViolations& errors)
{
  const auto target = timing_edit::TransactionGeneration{index + 1};
  if (semantic.expected_generation != timing_edit::TransactionGeneration{index}
      || !timing_edit::validateEditBatch(semantic).empty())
    reject(errors, DualCsrViolationCode::kConflict, "aggregate.semantic_trace", "semantic record is invalid");
  if (execution.generation() != target || !validateLevelizationExecutionTraceShape(execution))
    reject(errors, DualCsrViolationCode::kGeneration, "aggregate.execution_trace", "execution record mode or generation is invalid");
  if (descriptor) {
    if (descriptor->session_token != state.graph.session_token || descriptor->expected_generation != semantic.expected_generation
        || descriptor->target_generation != target)
      reject(errors, DualCsrViolationCode::kGeneration, "aggregate.descriptor_trace", "descriptor session or generation differs");
    if (execution.levels().level_nodes.size() != descriptor->after.node_slots
        || execution.levels().node_to_level.size() != descriptor->after.node_slots)
      reject(errors, DualCsrViolationCode::kGeneration, "aggregate.execution_trace", "execution levels differ from descriptor boundary");
    if (execution.mode() == LevelizationExecutionMode::kMixedStage6) {
      const auto& mixed = execution.mixed();
      if (!mixed || mixed->n_last_nodes != descriptor->before.node_slots || mixed->n_current_nodes != descriptor->after.node_slots
          || mixed->n_new_nodes != descriptor->node_appends.size())
        reject(errors, DualCsrViolationCode::kGeneration, "aggregate.execution_trace", "mixed execution boundary differs");
    }
  }
  if (incregpusta_debug_checks_enabled && target == state.generation
      && (execution.levels().level_offsets != state.timing.values.level_offsets
          || execution.levels().level_nodes != state.timing.values.level_nodes
          || execution.levels().node_to_level != state.timing.values.node_to_level || execution.traversal() != state.traversal_telemetry))
    reject(errors, DualCsrViolationCode::kTimingResult, "aggregate.execution_trace.latest", "latest execution differs from current timing");
}

}  // namespace

DualCsrViolations validatePendingDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState& owner, const DualCsrTimingResult& timing,
    const timing_edit::TimingEditResolverState& resolver, const std::optional<timing_edit::CommittedEditIdentity>& identity,
    const DualCsrPendingBatch& batch, timing_edit::AppendOnlyHighWater before_high_water)
{
  const AnalysisView staged{owner.graph, timing, resolver, owner.arc_group_index, identity,
                            owner.graph.generation, owner.graph.telemetry.traversal};
  auto errors = validateFunctionalState(staged);
  if (!owner.baseline || staged.generation.value() != owner.generation.value() + 1)
    reject(errors, DualCsrViolationCode::kGeneration, "pending.history",
           "pending stage must advance its bound baseline by one generation");
  if (owner.baseline)
    validateHistoryRecord(staged, batch.semantic, batch.execution,
                          batch.descriptor ? &*batch.descriptor : nullptr, owner.generation.value(), errors);
  if (incregpusta_debug_checks_enabled != batch.descriptor.has_value()
      || (batch.descriptor && (batch.descriptor->before != before_high_water || batch.descriptor->after != staged.graph.high_water)))
    reject(errors, DualCsrViolationCode::kHighWater, "pending.descriptor", "pending descriptor does not bind the stage boundary");
  return errors;
}

DualCsrViolations validateCommittedDualCsrAnalysisState(const CommittedDualCsrAnalysisState& state)
{
  const AnalysisView view{state.graph, state.timing, state.resolver, state.arc_group_index,
                          state.identity, state.generation, state.traversal_telemetry};
  auto errors = validateFunctionalState(view);
  const auto generation = state.generation.value();
  if (state.semantic_trace.size() != generation || state.execution_trace.size() != generation
      || (incregpusta_debug_checks_enabled ? state.descriptor_trace.size() != generation : !state.descriptor_trace.empty())
      || !state.baseline)
    reject(errors, DualCsrViolationCode::kGeneration, "aggregate.history", "history must cover committed generations");
  if (!state.baseline)
    return errors;
  if (generation == 0
      && (!state.identity || state.baseline->fingerprints != state.identity->current_fingerprints
          || state.baseline->canonical_pre_state != state.identity->canonical_pre_state))
    reject(errors, DualCsrViolationCode::kConflict, "aggregate.baseline", "generation-zero baseline differs from committed identity");
  if (!timing_edit::validateBaselineSemanticIdentity(*state.baseline).empty())
    reject(errors, DualCsrViolationCode::kConflict, "aggregate.baseline", "history baseline is invalid");
  for (std::size_t index = 0; index < state.semantic_trace.size() && index < state.execution_trace.size(); ++index) {
    const auto* descriptor = index < state.descriptor_trace.size() ? &state.descriptor_trace[index] : nullptr;
    validateHistoryRecord(view, state.semantic_trace[index], state.execution_trace[index], descriptor, index, errors);
    if (descriptor && ((index != 0 && descriptor->before != state.descriptor_trace[index - 1].after)
                       || (index + 1 == generation && descriptor->after != state.graph.high_water)))
      reject(errors, DualCsrViolationCode::kHighWater, "aggregate.descriptor_trace", "descriptor high-water chain differs");
  }
  return errors;
}

}  // namespace ista::timing_dual_csr
