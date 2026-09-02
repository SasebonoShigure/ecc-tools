// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "DualCsrArcGroupIndex.hpp"
#include "DualSemanticReadView.hpp"
#include "NarrowTimingCaptureInternal.hpp"
#include "TimingEditResolver.hpp"

namespace ista {
class Database;
}

namespace ista::timing_dual_csr::detail {

struct NativeCandidateArcOrderFact
{
  std::size_t native_index{0};
  csr::ArcKey key;
  csr::NodeId source;
  csr::NodeId sink;
  bool is_clock_arc{false};
  bool is_disabled{false};
  bool is_loop_disabled{false};
  csr::CellTransferModel transfer;
  std::optional<csr::ArcId> assigned_id;
};

struct NativeAffectedArcRows
{
  std::vector<CapturedOutgoingRow> outgoing;
  std::vector<CapturedIncomingRow> incoming;
};

using NativeArcOrderResult
    = std::variant<NativeAffectedArcRows, NarrowCaptureError>;

[[nodiscard]] NativeArcOrderResult captureNativeAffectedArcRows(
    const ista::Database& database,
    const DualSemanticReadView& committed,
    const DualCsrArcGroupIndex& committed_index,
    const timing_edit::TimingEditResolverState& staged,
    const AffectedCapturePlan& plan,
    std::span<const NativeCandidateArcOrderFact> candidates);

}  // namespace ista::timing_dual_csr::detail
