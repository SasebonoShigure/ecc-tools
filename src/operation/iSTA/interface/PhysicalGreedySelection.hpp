// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "TimingEditTypes.hpp"
#include <array>
#include <span>
#include "CsrBufferCandidateTiming.hpp"
namespace idb { class IdbDesign; }
namespace ista {
class Database;
namespace csr::detail { class NativeLibraryModels; }
namespace timing_dual_csr { struct DualCsrState; struct DualCsrArcGroupIndex; }
namespace timing_edit { class TimingEditResolverState; }
struct PhysicalGreedyCandidate {
  timing_edit::BufferInsertionMutation edit;
  std::array<std::int32_t, 2> target_dbu;
  double predicted_gain_ns, slack_ns;
  friend bool operator==(const PhysicalGreedyCandidate&, const PhysicalGreedyCandidate&) = default;
};
struct PhysicalGreedySelection {
  std::uint64_t generation;
  std::vector<PhysicalGreedyCandidate> candidates;
  friend bool operator==(const PhysicalGreedySelection&, const PhysicalGreedySelection&) = default;
};
// Inputs are borrowed only while the session operation guard is held.
PhysicalGreedySelection selectPhysicalBuffers(const Database& database,
    csr::detail::NativeLibraryModels& models, const timing_edit::TimingEditResolverState& resolver,
    std::uint64_t generation, std::span<const std::string> buffer_cells,
    std::uint64_t top_k, std::uint64_t max_buffers, std::int32_t dbu_per_um);
PhysicalGreedySelection selectCsrPhysicalBuffers(const Database& library, csr::detail::NativeLibraryModels& models,
    idb::IdbDesign& design, const timing_dual_csr::DualCsrState& graph,
    const timing_dual_csr::DualCsrArcGroupIndex& index, const timing_edit::TimingEditResolverState& resolver,
    std::span<const csr::BufferCandidateTiming> timing, std::span<const std::string> buffer_cells,
    std::uint64_t top_k, std::uint64_t max_buffers, std::int32_t dbu_per_um);
}  // namespace ista
