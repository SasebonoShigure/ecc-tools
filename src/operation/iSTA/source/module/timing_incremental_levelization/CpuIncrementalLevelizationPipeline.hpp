// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <variant>

#include "ActualDagCycleClassifier.hpp"
#include "MixedLevelReconstructor.hpp"
#include "MixedLevelizationTelemetry.hpp"
#include "MixedLevelizationTypes.hpp"

namespace ista::timing_incremental_levelization {

struct MixedLevelizationSuccess
{
  csr::CsrFifoLevelization levels;
  MixedLevelizationTelemetry telemetry;

  friend bool operator==(const MixedLevelizationSuccess&,
                         const MixedLevelizationSuccess&) = default;
};

using MixedLevelizationAttempt =
    std::variant<MixedLevelizationSuccess, MixedLevelizationFailure>;

[[nodiscard]] MixedLevelizationAttempt runCpuIncrementalLevelization(
    const csr::ActiveCsrEdgeView& edges,
    const csr::CsrFifoLevelization& previous,
    const MixedLevelizationRequest& request);

}  // namespace ista::timing_incremental_levelization
