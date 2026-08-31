// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ista {
class Database;
}

namespace ista::timing_edit {

enum class NativeDatabaseValidationCode : std::uint8_t
{
  kNamedObjectMismatch,
  kMembershipMismatch,
  kTimingPointCoverage,
  kInvalidArcEndpoint,
  kInvalidCellArcPointer,
  kInvalidNetArcPointer,
  kInvalidAdjacencyIndex,
  kInvalidPredecessorIndex,
  kInvalidTimingOrder
};

struct NativeDatabaseValidationFailure
{
  NativeDatabaseValidationCode code{NativeDatabaseValidationCode::kNamedObjectMismatch};
  std::string object;
  std::string detail;

  friend bool operator==(const NativeDatabaseValidationFailure&, const NativeDatabaseValidationFailure&) = default;
};

using NativeDatabaseValidationResult = std::optional<NativeDatabaseValidationFailure>;

[[nodiscard]] NativeDatabaseValidationResult validateNativeDatabase(const Database& database);
void requireValidNativeDatabase(const Database& database);

}  // namespace ista::timing_edit
