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

#include "../../data_manager/adapter/csr/DualCsrSemanticIdentity.hpp"
#include "DualCsrArcGroupIndexValidation.hpp"
#include "DualCsrStateValidation.hpp"
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
}

void swap(CommittedDualCsrAnalysisState& lhs,
          CommittedDualCsrAnalysisState& rhs) noexcept
{
  lhs.swap(rhs);
}

DualCsrViolations validateCommittedDualCsrAnalysisState(
    const CommittedDualCsrAnalysisState& state)
{
  DualCsrViolations errors = validateDualCsrState(state.graph);
  append(errors, validateDualCsrArcGroupIndex(state.graph, state.arc_group_index));
  append(errors, validateDualCsrTimingResult(state.graph, state.timing));
  if (state.generation != state.graph.generation)
    reject(errors, DualCsrViolationCode::kGeneration, "aggregate.generation",
           "aggregate and graph generation differ");
  if (state.generation != state.identity.generation)
    reject(errors, DualCsrViolationCode::kGeneration, "aggregate.identity",
           "identity generation differs from aggregate generation");
  if (state.resolver.highWater() != state.graph.high_water)
    reject(errors, DualCsrViolationCode::kHighWater, "aggregate.resolver",
           "resolver and graph high-water marks differ");
  if (state.traversal_telemetry != state.graph.telemetry.traversal)
    reject(errors, DualCsrViolationCode::kTimingResult,
           "aggregate.traversal_telemetry",
           "aggregate and graph traversal telemetry differ");
  if (!timing_edit::validateCommittedEditIdentity(
           state.identity, "aggregate.identity")
           .empty())
    reject(errors, DualCsrViolationCode::kConflict, "aggregate.identity",
           "committed edit identity is invalid");
  if (deriveDualCsrCommittedIdentity(state.graph, state.resolver)
      != state.identity)
    reject(errors, DualCsrViolationCode::kConflict,
           "aggregate.identity",
           "rank-free graph/resolver projection differs from identity");

  const std::uint64_t generation = state.generation.value();
  if (state.descriptor_trace.size() != generation)
    reject(errors, DualCsrViolationCode::kGeneration,
           "aggregate.descriptor_trace",
           "descriptor trace length must equal committed generation");
  if (state.semantic_trace.size() != generation)
    reject(errors, DualCsrViolationCode::kGeneration,
           "aggregate.semantic_trace",
           "semantic trace length must equal committed generation");

  std::optional<timing_edit::BaselineSemanticIdentity> trace_baseline;
  std::optional<timing_edit::AppendOnlyHighWater> prior_after;
  for (std::size_t index = 0; index < state.descriptor_trace.size(); ++index) {
    const auto& descriptor = state.descriptor_trace[index];
    if (descriptor.session_token != state.graph.session_token
        || descriptor.expected_generation
               != timing_edit::TransactionGeneration{index}
        || descriptor.target_generation
               != timing_edit::TransactionGeneration{index + 1}) {
      reject(errors, DualCsrViolationCode::kGeneration,
             "aggregate.descriptor_trace[" + std::to_string(index) + "]",
             "descriptor session or generation chain is not contiguous");
    }
    if (prior_after && descriptor.before != *prior_after)
      reject(errors, DualCsrViolationCode::kHighWater,
             "aggregate.descriptor_trace[" + std::to_string(index) + "]",
             "descriptor high-water chain is not contiguous");
    prior_after = descriptor.after;
  }
  for (std::size_t index = 0; index < state.semantic_trace.size(); ++index) {
    const auto& semantic = state.semantic_trace[index];
    if (semantic.expected_generation
            != timing_edit::TransactionGeneration{index}
        || semantic.schema_version != timing_edit::kDesignEditPlanSchema) {
      reject(errors, DualCsrViolationCode::kGeneration,
             "aggregate.semantic_trace[" + std::to_string(index) + "]",
             "semantic trace schema or generation chain is invalid");
    }
    if (!timing_edit::validateDesignEditPlan(semantic).empty())
      reject(errors, DualCsrViolationCode::kConflict,
             "aggregate.semantic_trace[" + std::to_string(index) + "]",
             "semantic trace entry is not a valid canonical edit plan");
    if (!trace_baseline)
      trace_baseline = semantic.baseline;
    else if (semantic.baseline != *trace_baseline)
      reject(errors, DualCsrViolationCode::kConflict,
             "aggregate.semantic_trace[" + std::to_string(index) + "]",
             "semantic trace changed baseline identity");
    if (index < state.descriptor_trace.size()
        && state.descriptor_trace[index].expected_generation
               != semantic.expected_generation)
      reject(errors, DualCsrViolationCode::kGeneration,
             "aggregate.trace[" + std::to_string(index) + "]",
             "descriptor and semantic generations differ");
  }
  if (prior_after && *prior_after != state.graph.high_water)
    reject(errors, DualCsrViolationCode::kHighWater,
           "aggregate.descriptor_trace",
           "last descriptor high-water does not match committed graph");
  return errors;
}

}  // namespace ista::timing_dual_csr
