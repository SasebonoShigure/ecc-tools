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
#include <optional>
#pragma once

#include <cstdint>
#include "ActiveCsrEdgeView.hpp"
#include "TimingPayloadView.hpp"

#include <vector>
#include <utility>

#include "CsrReferenceTypes.hpp"

namespace ista::csr {

struct CsrFifoLevelization
{
  CsrFifoLevelization() = default;
  CsrFifoLevelization(bool is_complete, std::vector<std::uint64_t> offsets, std::vector<NodeId> nodes, std::vector<std::uint64_t> levels)
      : complete(is_complete), level_offsets(std::move(offsets)), level_nodes(std::move(nodes)), node_to_level(std::move(levels))
  {
  }

  bool complete{false};
  std::vector<std::uint64_t> level_offsets{0};
  std::vector<NodeId> level_nodes;
  std::vector<std::uint64_t> node_to_level;
  std::optional<CsrViewError> view_error;

  friend bool operator==(const CsrFifoLevelization&, const CsrFifoLevelization&) = default;
};

// Precondition: validateReferenceInput(input) is empty. This helper deliberately
// does not guard malformed CSR indices; every production caller must visibly
[[nodiscard]] CsrFifoLevelization replayFifoLevelization(const ActiveCsrEdgeView& edges, const TimingPayloadView& payloads,
                                                         CsrTraversalTelemetry* telemetry = nullptr);
// validate the input before replay, while direct tests pass only valid inputs.
[[nodiscard]] CsrFifoLevelization replayFifoLevelization(const CsrReferenceInput& input);

}  // namespace ista::csr
