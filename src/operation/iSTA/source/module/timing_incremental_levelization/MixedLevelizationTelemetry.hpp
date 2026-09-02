// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>

namespace ista::timing_incremental_levelization {

struct MixedLevelizationTelemetry
{
  std::uint64_t n_last_nodes{0};
  std::uint64_t n_current_nodes{0};
  std::uint64_t n_new_nodes{0};
  std::uint64_t n_old_levels{0};
  std::uint64_t mixed_vertex_count{0};

  std::uint64_t actual_eligible_arc_visits{0};
  std::uint64_t old_old{0};
  std::uint64_t old_new{0};
  std::uint64_t new_old{0};
  std::uint64_t new_new{0};
  std::uint64_t mapped_actual_arc_candidates{0};
  std::uint64_t unique_actual_endpoint_pairs{0};
  std::uint64_t actual_parallel_duplicates_collapsed{0};
  std::uint64_t proxy_alias_duplicates_collapsed{0};
  std::uint64_t unique_precedence_edges{0};
  std::uint64_t virtual_chain_edges{0};
  std::uint64_t base_out{0};
  std::uint64_t aux_out{0};

  std::uint64_t kahn_processed_count{0};
  std::uint64_t max_frontier{0};
  std::uint64_t reconstructed_level_count{0};
  std::uint64_t actual_cycle_classifier_runs{0};
  std::uint64_t actual_cycle_classifier_arc_visits{0};
  std::optional<bool> legal_noncompact;
  std::uint64_t fallback_count{0};

  // Run-envelope observations only. Transaction code must not commit or use
  // these values for exact recovery equality.
  double mixed_build_ms{0.0};
  double mixed_kahn_ms{0.0};
  double reconstruct_ms{0.0};

  friend bool operator==(const MixedLevelizationTelemetry&,
                         const MixedLevelizationTelemetry&) = default;
};

[[nodiscard]] bool validateMixedTelemetryIdentities(
    const MixedLevelizationTelemetry& telemetry) noexcept;

}  // namespace ista::timing_incremental_levelization
