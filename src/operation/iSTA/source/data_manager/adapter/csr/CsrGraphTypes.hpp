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

#include "CsrReferenceTypes.hpp"

namespace ista::csr {

inline constexpr auto kCsrGraphSnapshotSchema = "ista-csr-graph-snapshot/1.0.0";

struct CsrGraphArcRecord
{
  ArcId id;
  ArcKey key;
  NodeId source;
  NodeId sink;
  ArcKind kind{ArcKind::kNet};
  std::uint64_t native_traversal_rank{0};
  bool is_clock_arc{false};
  bool is_disabled{false};
  bool is_loop_disabled{false};

  friend bool operator==(const CsrGraphArcRecord&, const CsrGraphArcRecord&) = default;
};

struct CsrGraphSnapshot
{
  std::string schema{kCsrGraphSnapshotSchema};
  std::vector<NodeRecord> nodes;
  std::vector<NodeIdLookupEntry> node_ids_by_key;
  std::vector<InstanceRecord> instances;
  std::vector<CsrGraphArcRecord> arcs;
  std::vector<ArcIdLookupEntry> arc_ids_by_key;
  std::vector<std::uint64_t> out_row_offsets;
  std::vector<ArcId> out_arc_ids;
  std::vector<std::uint64_t> in_row_offsets;
  std::vector<ArcId> in_arc_ids;
  std::vector<NodeId> ordered_root_ids;
  std::vector<NodeId> ordered_start_ids;
  std::vector<NodeId> ordered_end_ids;

  friend bool operator==(const CsrGraphSnapshot&, const CsrGraphSnapshot&) = default;
};

struct CsrGraphContractViolation
{
  CsrCaptureCode code{CsrCaptureCode::kMalformedTopology};
  std::string field;
  std::string message;

  friend bool operator==(const CsrGraphContractViolation&, const CsrGraphContractViolation&) = default;
};

struct CsrGraphExportFailure
{
  CsrCaptureCode code{CsrCaptureCode::kMalformedTopology};

  friend bool operator==(const CsrGraphExportFailure&, const CsrGraphExportFailure&) = default;
};

using CsrGraphExportResult = std::variant<CsrGraphSnapshot, CsrGraphExportFailure>;

}  // namespace ista::csr
