// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "CsrCpuReferenceBackend.hpp"
#include "../CsrCpuReferenceTestFixture.hpp"
#include "CsrFifoLevelizationValidation.hpp"
#include "FlatCsrEdgeView.hpp"
#include "FlatTimingPayloadView.hpp"

namespace ista::csr::test {
namespace {

static_assert(!std::is_default_constructible_v<CsrFullLevelizerEntryDelta>);
static_assert(!std::is_copy_constructible_v<CsrFullLevelizerEntryDelta>);
static_assert(!std::is_copy_constructible_v<CsrFullLevelizerOperationGuard>);

const CsrTimingResult& requireTiming(const CsrComputationResult& computation, const std::string& name)
{
  const auto* timing = std::get_if<CsrTimingResult>(&computation.outcome);
  requireCpuReference(timing != nullptr, name + " did not produce timing");
  return *timing;
}

void requireInvariantFailure(const CsrComputationWithTelemetry& output, const std::string& name)
{
  const auto* code = std::get_if<CsrComputationCode>(&output.computation.outcome);
  requireCpuReference(code != nullptr && *code == CsrComputationCode::kInvariantViolation,
                      name + " did not fail with invariant violation");
}

void requireValidationCode(const ActiveCsrEdgeView& edges, const CsrFifoLevelization& levels,
                           CsrFifoLevelizationValidationCode code, const std::string& name)
{
  requireCpuReference(validateCsrFifoLevelization(edges, levels).code == code,
                      name + " was not rejected with the expected validation code");
}

CsrFifoLevelization suppliedLevels(const CsrTimingResult& timing)
{
  return {true, timing.level_offsets, timing.level_nodes, timing.node_to_level};
}

CsrFifoLevelization reverseLevels(const CsrFifoLevelization& supplied)
{
  CsrFifoLevelization reversed = supplied;
  std::reverse(reversed.level_nodes.begin(), reversed.level_nodes.end());
  std::fill(reversed.node_to_level.begin(), reversed.node_to_level.end(), 0);
  for (std::size_t level = 0; level + 1 < reversed.level_offsets.size(); ++level)
    for (std::uint64_t position = reversed.level_offsets[level]; position < reversed.level_offsets[level + 1]; ++position)
      reversed.node_to_level[reversed.level_nodes[static_cast<std::size_t>(position)].value()] = level + 1;
  return reversed;
}

void runExternalLevelBackendContracts()
{
  const CsrReferenceInput input = makeCpuReferenceNetFixture();
  FlatCsrEdgeView edges(input);
  FlatTimingPayloadView payloads(input);
  CsrCpuReferenceBackend backend;

  auto flat_guard = beginCsrFullLevelizerOperation();
  const CsrFifoLevelization flat_levels = replayFifoLevelization(input);
  const auto flat_delta = std::move(flat_guard).finish();
  requireCpuReference(flat_delta.value() == 1, "flat FIFO overload double-counted or missed its generic full-levelizer entry");
  requireCpuReference(validateCsrFifoLevelization(edges, flat_levels).valid(), "flat FIFO overload produced invalid levels");

  auto full_guard = beginCsrFullLevelizerOperation();
  const CsrComputationWithTelemetry full = backend.computeWithTelemetry(edges, payloads);
  const auto full_delta = std::move(full_guard).finish();
  requireCpuReference(full_delta.value() == 1, "full backend did not enter the generic full levelizer exactly once");
  requireCpuReference(full.telemetry.levelization.base_in > 0 && full.telemetry.levelization.base_out > 0,
                      "full backend did not record FIFO traversal");

  const CsrFifoLevelization supplied = suppliedLevels(requireTiming(full.computation, "full backend"));
  requireCpuReference(validateCsrFifoLevelization(edges, supplied).valid(), "full backend levels failed the public validator");

  auto external_guard = beginCsrFullLevelizerOperation();
  const CsrComputationWithTelemetry external = backend.computeWithTelemetry(edges, payloads, supplied);
  const auto external_delta = std::move(external_guard).finish();
  requireCpuReference(external_delta.value() == 0, "external-level backend entered the full levelizer");
  requireCpuReference(external.phase_timings.levelization_ms == 0.0, "external-level backend reported levelization time");
  requireCpuReference(external.telemetry.levelization == CsrStageStorageVisits{}, "external-level backend forged FIFO traversal telemetry");
  requireExactComputationResult(external.computation, full.computation, "external levels versus full backend");

  CsrFifoLevelization embedded_error = supplied;
  embedded_error.view_error = CsrViewError::kCallbackError;
  const auto embedded_validation = validateCsrFifoLevelization(edges, embedded_error);
  requireCpuReference(embedded_validation.code == CsrFifoLevelizationValidationCode::kViewError
                          && embedded_validation.view_error == CsrViewError::kCallbackError,
                      "complete external levels with a view error were accepted");
  auto embedded_guard = beginCsrFullLevelizerOperation();
  const auto embedded_output = backend.computeWithTelemetry(edges, payloads, embedded_error);
  const auto embedded_delta = std::move(embedded_guard).finish();
  requireInvariantFailure(embedded_output, "embedded view-error levels");
  requireCpuReference(embedded_delta.value() == 0, "embedded view-error rejection entered the full levelizer");

  CsrFifoLevelization incomplete = supplied;
  incomplete.complete = false;
  requireValidationCode(edges, incomplete, CsrFifoLevelizationValidationCode::kIncomplete, "incomplete external levels");

  CsrFifoLevelization malformed_offsets = supplied;
  malformed_offsets.level_offsets.back() -= 1;
  requireValidationCode(edges, malformed_offsets, CsrFifoLevelizationValidationCode::kMalformedOffsets,
                        "malformed external offsets");

  CsrFifoLevelization malformed_permutation = supplied;
  malformed_permutation.level_nodes.back() = malformed_permutation.level_nodes.front();
  requireValidationCode(edges, malformed_permutation, CsrFifoLevelizationValidationCode::kMalformedPermutation,
                        "malformed external permutation");

  CsrFifoLevelization malformed_inverse = supplied;
  malformed_inverse.node_to_level[malformed_inverse.level_nodes.front().value()] = 0;
  requireValidationCode(edges, malformed_inverse, CsrFifoLevelizationValidationCode::kMalformedInverse,
                        "malformed external inverse");

  auto malformed_guard = beginCsrFullLevelizerOperation();
  const auto malformed_output = backend.computeWithTelemetry(edges, payloads, malformed_offsets);
  const auto malformed_delta = std::move(malformed_guard).finish();
  requireInvariantFailure(malformed_output, "malformed offsets");
  requireCpuReference(malformed_delta.value() == 0, "malformed external offsets entered the full levelizer");

  const CsrFifoLevelization reversed = reverseLevels(supplied);
  requireCpuReference(validateCsrFifoLevelization(edges, reversed).code
                          == CsrFifoLevelizationValidationCode::kEligibleEdgeOrderViolation,
                      "eligible edge order violation was not classified");
  auto reversed_guard = beginCsrFullLevelizerOperation();
  const auto reversed_output = backend.computeWithTelemetry(edges, payloads, reversed);
  const auto reversed_delta = std::move(reversed_guard).finish();
  requireInvariantFailure(reversed_output, "reversed levels");
  requireCpuReference(reversed_delta.value() == 0, "edge-order rejection entered the full levelizer");
}

}  // namespace
}  // namespace ista::csr::test

int main()
{
  try {
    ista::csr::test::runExternalLevelBackendContracts();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
