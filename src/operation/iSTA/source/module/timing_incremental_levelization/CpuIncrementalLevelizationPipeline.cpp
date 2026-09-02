// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "CpuIncrementalLevelizationPipeline.hpp"

#include <chrono>
#include <string>

#include "CorrectedMixedKahnLevelizer.hpp"
#include "CsrFifoLevelizationValidation.hpp"
#include "MixedLevelizationGraphBuilder.hpp"
#include "MixedLevelizationInternal.hpp"
#include "MixedLevelizationValidation.hpp"

namespace ista::timing_incremental_levelization {
namespace {

using Clock = std::chrono::steady_clock;

double elapsedMilliseconds(Clock::time_point begin, Clock::time_point end)
{
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

MixedLevelizationFailure failure(
    MixedStageFailureCode code, MixedFailureOrigin origin,
    const MixedLevelizationRequest& request, std::string detail,
    std::optional<csr::CsrViewError> view_error = std::nullopt)
{
  return {code, origin, view_error, request.generation_after,
          std::move(detail)};
}

MixedLevelizationFailure classifyMixedFailure(
    const csr::ActiveCsrEdgeView& edges,
    MixedLevelizationTelemetry& telemetry,
    const MixedLevelizationRequest& request,
    MixedFailureOrigin conflict_origin)
{
  if (!detail::checkedIncrement(telemetry.actual_cycle_classifier_runs))
    return failure(MixedStageFailureCode::kInvariantFailure,
                   MixedFailureOrigin::kActualClassifier, request,
                   "actual classifier run counter overflow");
  const ActualDagClassification actual = classifyActualDag(edges);
  telemetry.actual_cycle_classifier_arc_visits = actual.eligible_arc_visits;
  if (actual.code == ActualDagClassificationCode::kViewError)
    return failure(MixedStageFailureCode::kViewError,
                   MixedFailureOrigin::kActualClassifier, request,
                   actual.detail, actual.view_error);
  if (actual.code == ActualDagClassificationCode::kInvariantFailure)
    return failure(MixedStageFailureCode::kInvariantFailure,
                   MixedFailureOrigin::kActualClassifier, request,
                   actual.detail);
  if (actual.code == ActualDagClassificationCode::kCycle)
    return failure(MixedStageFailureCode::kActualCycle,
                   MixedFailureOrigin::kActualClassifier, request,
                   actual.detail);
  return failure(MixedStageFailureCode::kOldLevelOrderConflict,
                 conflict_origin, request,
                 "actual graph is acyclic but violates the preserved old-level order");
}

}  // namespace

MixedLevelizationAttempt CpuIncrementalLevelizationPipeline::run(
    const csr::ActiveCsrEdgeView& edges,
    const csr::CsrFifoLevelization& previous,
    const MixedLevelizationRequest& request)
{
  const Clock::time_point build_begin = Clock::now();
  MixedGraphBuildAttempt build =
      buildMixedLevelizationGraph(edges, previous, request);
  const Clock::time_point build_end = Clock::now();
  if (auto* build_failure = std::get_if<MixedGraphBuildFailure>(&build)) {
    build_failure->telemetry.mixed_build_ms =
        elapsedMilliseconds(build_begin, build_end);
    switch (build_failure->code) {
      case MixedGraphBuildFailureCode::kOldOldOrderConflict:
        return classifyMixedFailure(
            edges, build_failure->telemetry, request,
            MixedFailureOrigin::kBuilderOldOld);
      case MixedGraphBuildFailureCode::kViewError:
        return failure(MixedStageFailureCode::kViewError,
                       MixedFailureOrigin::kViewTraversal, request,
                       build_failure->detail, build_failure->view_error);
      case MixedGraphBuildFailureCode::kMalformedPreviousLevels:
        return failure(MixedStageFailureCode::kMalformedPreviousLevels,
                       MixedFailureOrigin::kViewTraversal, request,
                       build_failure->detail);
      case MixedGraphBuildFailureCode::kMappingOverflow:
        return failure(MixedStageFailureCode::kMappingOverflow,
                       MixedFailureOrigin::kViewTraversal, request,
                       build_failure->detail);
      case MixedGraphBuildFailureCode::kInvariantFailure:
        return failure(MixedStageFailureCode::kInvariantFailure,
                       MixedFailureOrigin::kViewTraversal, request,
                       build_failure->detail);
    }
  }

  auto& graph = std::get<MixedPrecedenceGraph>(build);
  graph.telemetry.mixed_build_ms =
      elapsedMilliseconds(build_begin, build_end);
  const Clock::time_point kahn_begin = Clock::now();
  MixedKahnAttempt kahn = runCorrectedMixedKahn(graph);
  const Clock::time_point kahn_end = Clock::now();
  graph.telemetry.mixed_kahn_ms =
      elapsedMilliseconds(kahn_begin, kahn_end);
  if (const auto* kahn_failure = std::get_if<MixedKahnFailure>(&kahn)) {
    graph.telemetry.kahn_processed_count = kahn_failure->processed_count;
    graph.telemetry.max_frontier = kahn_failure->max_frontier;
    if (kahn_failure->code == MixedKahnFailureCode::kMixedCycle)
      return classifyMixedFailure(
          edges, graph.telemetry, request,
          MixedFailureOrigin::kMixedKahnQuotient);
    return failure(MixedStageFailureCode::kInvariantFailure,
                   MixedFailureOrigin::kMixedKahnQuotient, request,
                   kahn_failure->detail);
  }

  const auto& mixed = std::get<MixedKahnLevelization>(kahn);
  graph.telemetry.kahn_processed_count = mixed.processed_count;
  graph.telemetry.max_frontier = mixed.max_frontier;
  const Clock::time_point reconstruct_begin = Clock::now();
  MixedReconstructionAttempt reconstruction =
      reconstructMixedLevelization(graph.domain, previous, mixed);
  const Clock::time_point reconstruct_end = Clock::now();
  graph.telemetry.reconstruct_ms =
      elapsedMilliseconds(reconstruct_begin, reconstruct_end);
  if (const auto* reconstruct_failure =
          std::get_if<MixedReconstructionFailure>(&reconstruction)) {
    const MixedStageFailureCode code =
        reconstruct_failure->code
                == MixedReconstructionFailureCode::kMalformedPreviousLevels
            ? MixedStageFailureCode::kMalformedPreviousLevels
            : reconstruct_failure->code
                      == MixedReconstructionFailureCode::kMappingOverflow
                  ? MixedStageFailureCode::kMappingOverflow
                  : MixedStageFailureCode::kInvariantFailure;
    return failure(code, MixedFailureOrigin::kExternalLevelValidation,
                   request, reconstruct_failure->detail);
  }

  auto levels = std::get<csr::CsrFifoLevelization>(
      std::move(reconstruction));
  const csr::CsrFifoLevelizationValidationResult validation =
      csr::validateCsrFifoLevelization(edges, levels);
  if (!validation.valid()) {
    if (validation.code
        == csr::CsrFifoLevelizationValidationCode::kViewError)
      return failure(MixedStageFailureCode::kViewError,
                     MixedFailureOrigin::kExternalLevelValidation,
                     request,
                     "reconstructed-level edge traversal failed",
                     validation.view_error);
    return failure(MixedStageFailureCode::kInvariantFailure,
                   MixedFailureOrigin::kExternalLevelValidation,
                   request,
                   "reconstructed levels violate the external-level contract");
  }
  graph.telemetry.reconstructed_level_count =
      static_cast<std::uint64_t>(levels.level_offsets.size() - 1);
  if (!validateMixedTelemetryIdentities(graph.telemetry))
    return failure(MixedStageFailureCode::kInvariantFailure,
                   MixedFailureOrigin::kExternalLevelValidation,
                   request,
                   "final mixed telemetry identities are inconsistent");
  return MixedLevelizationSuccess{std::move(levels), graph.telemetry};
}

}  // namespace ista::timing_incremental_levelization
