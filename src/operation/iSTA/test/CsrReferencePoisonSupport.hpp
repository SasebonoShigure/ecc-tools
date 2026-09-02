// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "CsrReferenceInputRawProjection.hpp"

namespace ista {
class Database;
}

namespace ista::csr::test {

inline constexpr std::array<std::string_view, 17> kReferencePoisonFamilyTokens = {
    "timing_point_level",
    "timing_point_is_clock_point",
    "timing_point_scalars",
    "timing_point_clock_arrival_maps",
    "timing_point_clock_slew_maps",
    "timing_point_clock_predecessor_maps",
    "timing_point_clock_predecessor_arc_delay_maps",
    "timing_point_clock_predecessor_transition_maps",
    "timing_point_data_slew_map",
    "timing_point_path_state_map",
    "arc_scalars",
    "arc_transition_maps",
    "arc_input_output_delay_map",
    "arc_graph_delay_map",
    "arc_timing_arc_delay_map",
    "arc_timing_arc_fallback_provenance",
    "database_timing_order",
};

struct ReferencePoisonBinding
{
  std::uint64_t repeat_index{0};
  std::string run_nonce;
  std::uint64_t session_token{0};
};

struct ReferencePoisonCanary
{
  std::string family;
  std::vector<std::uint8_t> pristine_typed_bytes;
  std::vector<std::uint8_t> before_typed_bytes;
  std::vector<std::uint8_t> after_typed_bytes;
};

[[nodiscard]] std::vector<ReferencePoisonCanary> poisonReferenceDerivedState(ista::Database& database, const CsrReferenceInputRawProjection& raw,
                                                                             const ReferencePoisonBinding& binding);

}  // namespace ista::csr::test
