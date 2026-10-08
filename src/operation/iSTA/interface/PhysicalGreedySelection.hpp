// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once
#include "TimingEditTypes.hpp"
#include <array>
#include <span>
namespace ista {
class Database;
namespace csr::detail { class NativeLibraryModels; }
namespace timing_edit { class TimingEditResolverState; }
struct PhysicalGreedyCandidate {
  timing_edit::BufferInsertionMutation edit;
  std::array<std::int32_t, 2> target_dbu;
  double predicted_gain_ns, slack_ns;
};
struct PhysicalGreedySelection {
  std::uint64_t generation;
  std::vector<PhysicalGreedyCandidate> candidates;
};
// Inputs are borrowed only while the session operation guard is held.
PhysicalGreedySelection selectPhysicalBuffers(const Database& database,
    csr::detail::NativeLibraryModels& models, const timing_edit::TimingEditResolverState& resolver,
    std::uint64_t generation, std::span<const std::string> buffer_cells,
    std::uint64_t top_k, std::uint64_t max_buffers, std::int32_t dbu_per_um);
}  // namespace ista
