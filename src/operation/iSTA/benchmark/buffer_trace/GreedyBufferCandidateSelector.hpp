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
#include <string>
#include <variant>
#include <vector>

#include "BufferTraceTypes.hpp"

namespace ista::buffer_trace {

struct GreedySelectionRequest
{
  SelectionProfile profile;
  std::string case_id;
  std::uint64_t seed{0};
  std::uint64_t batch_ordinal{0};
  std::uint64_t max_acceptances{0};
  std::vector<BufferCandidateObservation> candidates;
  std::vector<timing_edit::CreationToken> reserved_creation_tokens;
  std::vector<std::string> reserved_materialized_names;
};

struct SelectedBufferCandidate
{
  BufferCandidateObservation candidate;
  GainComputationReceipt numeric;
  timing_edit::CreationToken creation_token;

  friend bool operator==(const SelectedBufferCandidate&, const SelectedBufferCandidate&) = default;
};

struct GreedySelection
{
  std::vector<SelectedBufferCandidate> accepted;
  GenerationRoundReceipt receipt;

  friend bool operator==(const GreedySelection&, const GreedySelection&) = default;
};

enum class GreedySelectionErrorCode : std::uint8_t
{
  kInvalidRequest,
  kDuplicateCandidate,
  kInconsistentNetDriver,
  kAcceptedReferenceConflict,
  kCreationTokenConflict,
  kMaterializedNameConflict
};

[[nodiscard]] std::string_view token(GreedySelectionErrorCode value);

struct GreedySelectionError
{
  GreedySelectionErrorCode code{GreedySelectionErrorCode::kInvalidRequest};
  std::string field;
  std::string message;

  friend bool operator==(const GreedySelectionError&, const GreedySelectionError&) = default;
};

using GreedySelectionResult = std::variant<GreedySelection, GreedySelectionError>;

class GreedyBufferCandidateSelector final
{
 public:
  [[nodiscard]] GreedySelectionResult select(const GreedySelectionRequest& request) const;
};

}  // namespace ista::buffer_trace
