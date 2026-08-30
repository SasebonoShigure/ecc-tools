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
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "CsrReferenceValidator.hpp"
#include "CsrTimingResultValidatorInternal.hpp"
#include "TimingResultScatterInternal.hpp"

namespace ista::csr::detail {
namespace {

[[nodiscard]] constexpr std::underlying_type_t<CsrTimingResultViolationClass> priority(CsrTimingResultViolationClass classification) noexcept
{
  return static_cast<std::underlying_type_t<CsrTimingResultViolationClass>>(classification);
}

[[nodiscard]] CsrScatterCode scatterCode(CsrTimingResultViolationClass classification)
{
  switch (classification) {
    case CsrTimingResultViolationClass::kSchema:
      return CsrScatterCode::kSchemaMismatch;
    case CsrTimingResultViolationClass::kSession:
      return CsrScatterCode::kSessionMismatch;
    case CsrTimingResultViolationClass::kGeneration:
      return CsrScatterCode::kGenerationMismatch;
    case CsrTimingResultViolationClass::kSourceIdentity:
      return CsrScatterCode::kSourceIdentityMismatch;
    case CsrTimingResultViolationClass::kShape:
      return CsrScatterCode::kShapeMismatch;
    case CsrTimingResultViolationClass::kNumeric:
      return CsrScatterCode::kResultNumericInvalid;
    case CsrTimingResultViolationClass::kIdMapping:
      return CsrScatterCode::kIdMappingMismatch;
    case CsrTimingResultViolationClass::kPredecessor:
      return CsrScatterCode::kPredecessorMismatch;
  }
  throw std::logic_error("unreachable CSR timing-result violation class");
}

}  // namespace

bool validTimingScatterGuard(const CsrReferenceInput& committed_input, std::uint64_t active_session_token, std::uint64_t active_graph_generation)
{
  if (active_session_token == 0 || committed_input.source_identity.session_token != active_session_token
      || committed_input.source_identity.graph_generation != 0 || active_graph_generation != committed_input.source_identity.graph_generation
      || !committed_input.unsupported_reasons.empty())
    return false;
  return validateReferenceInputForSession(committed_input, active_session_token).empty();
}

CsrCurrentFingerprints currentFingerprints(const CsrSnapshotIdentity& identity)
{
  return CsrCurrentFingerprints{identity.topology_fingerprint, identity.clock_constraint_fingerprint, identity.library_fingerprint,
                                identity.corner_fingerprint,   identity.parasitic_fingerprint,        identity.timing_input_fingerprint};
}

std::optional<CsrScatterCode> firstTimingResultScatterFailure(const CsrReferenceInput& committed_input, const CsrTimingResult& result)
{
  const TimingResultViolations violations = validateTimingResultClassified(committed_input, result);
  if (violations.empty())
    return std::nullopt;

  CsrTimingResultViolationClass selected = violations.front().classification;
  for (const auto& violation : violations)
    if (priority(violation.classification) < priority(selected))
      selected = violation.classification;
  return scatterCode(selected);
}

std::optional<std::uint64_t> timingScatterWriteCount(std::size_t node_count, std::size_t arc_count) noexcept
{
  constexpr std::uint64_t kNodeFamilies = 21;
  constexpr std::uint64_t kArcFamilies = 8;
  constexpr std::uint64_t kOrderFamilies = 1;
  constexpr std::uint64_t kMaximum = std::numeric_limits<std::uint64_t>::max();

  if constexpr (std::numeric_limits<std::size_t>::max() > kMaximum) {
    if (node_count > kMaximum || arc_count > kMaximum)
      return std::nullopt;
  }
  const std::uint64_t nodes = static_cast<std::uint64_t>(node_count);
  const std::uint64_t arcs = static_cast<std::uint64_t>(arc_count);
  if (nodes > (kMaximum - kOrderFamilies) / kNodeFamilies)
    return std::nullopt;
  const std::uint64_t node_writes = nodes * kNodeFamilies;
  if (arcs > (kMaximum - kOrderFamilies - node_writes) / kArcFamilies)
    return std::nullopt;
  return node_writes + arcs * kArcFamilies + kOrderFamilies;
}

}  // namespace ista::csr::detail
