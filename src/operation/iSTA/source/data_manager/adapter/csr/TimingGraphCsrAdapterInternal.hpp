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

#include <exception>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "CsrGraphTypes.hpp"
#include "Database.hpp"

namespace ista::csr::detail {

class GraphExportAbort final : public std::exception
{
 public:
  explicit GraphExportAbort(CsrCaptureCode code) : _code(code) {}
  [[nodiscard]] CsrCaptureCode code() const { return _code; }
  [[nodiscard]] const char* what() const noexcept override { return "CSR graph export rejected native data"; }

 private:
  CsrCaptureCode _code;
};

[[noreturn]] inline void malformed()
{
  throw GraphExportAbort(CsrCaptureCode::kMalformedTopology);
}

[[noreturn]] inline void collision()
{
  throw GraphExportAbort(CsrCaptureCode::kSemanticKeyCollision);
}

struct GraphBuildState
{
  explicit GraphBuildState(const ista::Database& value) : database(value) {}

  const ista::Database& database;
  CsrGraphSnapshot snapshot;
  std::map<std::string, NodeId> node_ids_by_full_name;
  std::unordered_map<std::string, std::string> ordered_pin_owners;
  std::vector<ArcKey> native_arc_keys;
  std::vector<ArcId> arc_ids_by_native_rank;
};

[[nodiscard]] PinDirection convertPinDirection(ista::PinDirection direction);
void exportGraph(GraphBuildState& state);
void exportNodesAndInstances(GraphBuildState& state);
void exportArcs(GraphBuildState& state);
void exportAdjacencyAndOrder(GraphBuildState& state);

}  // namespace ista::csr::detail
