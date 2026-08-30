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
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "CsrSnapshotObservationValidator.hpp"

namespace ista::csr {

enum class CsrObservationActionKind : std::uint8_t
{
  kRead,
  kReferenceMeasurement,
  kScatterRejection,
  kScatterSuccess,
  kInvalidation,
  kRelease
};

struct ReadAction
{
  friend bool operator==(const ReadAction&, const ReadAction&) = default;
};

struct ReferenceMeasurementAction
{
  double duration_ms{0.0};
  friend bool operator==(const ReferenceMeasurementAction&, const ReferenceMeasurementAction&) = default;
};

struct ScatterRejectionAction
{
  CsrScatterCode reason{CsrScatterCode::kShapeMismatch};
  std::optional<CsrSnapshotIdentity> source;
  double duration_ms{0.0};
  friend bool operator==(const ScatterRejectionAction&, const ScatterRejectionAction&) = default;
};

struct ScatterSuccessAction
{
  double duration_ms{0.0};
  friend bool operator==(const ScatterSuccessAction&, const ScatterSuccessAction&) = default;
};

struct InvalidationAction
{
  CsrCurrentFingerprints current_fingerprints;
  double duration_ms{0.0};
  friend bool operator==(const InvalidationAction&, const InvalidationAction&) = default;
};

struct ReleaseAction
{
  friend bool operator==(const ReleaseAction&, const ReleaseAction&) = default;
};

using CsrObservationAction
    = std::variant<ReadAction, ReferenceMeasurementAction, ScatterRejectionAction, ScatterSuccessAction, InvalidationAction, ReleaseAction>;
using CsrObservationActionKindResult = std::variant<CsrObservationActionKind, std::vector<CsrContractViolation>>;

[[nodiscard]] std::string_view token(CsrObservationActionKind kind);
[[nodiscard]] CsrObservationActionKindResult observationActionKind(const CsrObservationAction& action);

[[nodiscard]] CsrOptionalObservationResult reduceSnapshotObservation(const CsrOptionalObservation& before, const CsrObservationAction& action);
[[nodiscard]] std::vector<CsrContractViolation> validateSnapshotObservationTransition(const CsrOptionalObservation& before, const CsrOptionalObservation& after,
                                                                                      const CsrObservationAction& action);

}  // namespace ista::csr
