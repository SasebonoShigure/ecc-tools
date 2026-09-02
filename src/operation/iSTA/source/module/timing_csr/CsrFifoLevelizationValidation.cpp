// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "CsrFifoLevelizationValidation.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace ista::csr {
namespace {

[[nodiscard]] CsrFifoLevelizationValidationResult failure(CsrFifoLevelizationValidationCode code,
                                                           std::optional<CsrViewError> view_error = std::nullopt)
{
  return {code, view_error};
}

}  // namespace

CsrFifoLevelizationValidationResult validateCsrFifoLevelization(const ActiveCsrEdgeView& edges,
                                                                 const CsrFifoLevelization& levels)
{
  if (levels.view_error)
    return failure(CsrFifoLevelizationValidationCode::kViewError, levels.view_error);
  if (!levels.complete)
    return failure(CsrFifoLevelizationValidationCode::kIncomplete);

  const std::size_t node_count = edges.nodeCount();
  if (node_count > std::numeric_limits<std::uint64_t>::max())
    return failure(CsrFifoLevelizationValidationCode::kMalformedOffsets);
  const std::uint64_t node_count_u64 = static_cast<std::uint64_t>(node_count);

  if (levels.level_offsets.empty() || levels.level_offsets.front() != 0 || levels.level_offsets.back() != node_count_u64)
    return failure(CsrFifoLevelizationValidationCode::kMalformedOffsets);
  if (node_count == 0 && levels.level_offsets.size() != 1)
    return failure(CsrFifoLevelizationValidationCode::kMalformedOffsets);
  if (node_count != 0 && levels.level_offsets.size() < 2)
    return failure(CsrFifoLevelizationValidationCode::kMalformedOffsets);
  if (levels.level_nodes.size() != node_count)
    return failure(CsrFifoLevelizationValidationCode::kMalformedPermutation);
  if (levels.node_to_level.size() != node_count)
    return failure(CsrFifoLevelizationValidationCode::kMalformedInverse);

  std::vector<std::uint8_t> seen(node_count, 0);
  for (std::size_t level = 0; level + 1 < levels.level_offsets.size(); ++level) {
    const std::uint64_t begin = levels.level_offsets[level];
    const std::uint64_t end = levels.level_offsets[level + 1];
    if (begin >= end || end > node_count_u64)
      return failure(CsrFifoLevelizationValidationCode::kMalformedOffsets);
    for (std::uint64_t position = begin; position < end; ++position) {
      const std::size_t node = levels.level_nodes[static_cast<std::size_t>(position)].value();
      if (node >= node_count || seen[node] != 0)
        return failure(CsrFifoLevelizationValidationCode::kMalformedPermutation);
      if (levels.node_to_level[node] != level + 1)
        return failure(CsrFifoLevelizationValidationCode::kMalformedInverse);
      seen[node] = 1;
    }
  }
  for (std::uint8_t present : seen)
    if (present != 1)
      return failure(CsrFifoLevelizationValidationCode::kMalformedPermutation);

  bool edge_order_violation = false;
  const EdgeVisitResult visit = edges.forEachActiveArc([&](const EdgeEntry& entry) {
    if (!edges.isActive(entry.arc_id))
      return EdgeVisitResult::fail(CsrViewError::kInactiveArc);
    const auto payload = edges.arc(entry.arc_id);
    if (!payload)
      return EdgeVisitResult::fail(payload.error());
    if (payload.value().source.value() >= node_count || payload.value().sink.value() >= node_count)
      return EdgeVisitResult::fail(CsrViewError::kMalformedAdjacency);
    if (!payload.value().eligibility.levelization)
      return EdgeVisitResult::continueVisit();
    const std::uint64_t source_level = levels.node_to_level[payload.value().source.value()];
    const std::uint64_t sink_level = levels.node_to_level[payload.value().sink.value()];
    if (source_level < sink_level)
      return EdgeVisitResult::continueVisit();
    edge_order_violation = true;
    return EdgeVisitResult::stopVisit();
  });
  if (edge_order_violation)
    return failure(CsrFifoLevelizationValidationCode::kEligibleEdgeOrderViolation);
  if (visit.control == EdgeVisitControl::kError)
    return failure(CsrFifoLevelizationValidationCode::kViewError, visit.error.value_or(CsrViewError::kCallbackError));
  if (visit.control != EdgeVisitControl::kContinue)
    return failure(CsrFifoLevelizationValidationCode::kViewError, CsrViewError::kCallbackError);
  return {};
}

}  // namespace ista::csr
