// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <limits>

#include "AffectedCapturePlan.hpp"

namespace ista::timing_dual_csr {

struct NarrowCaptureAuditExpectation
{
  timing_edit::AppendOnlyHighWater after;
  std::uint64_t logical_deactivations{0};
};

namespace detail {

inline bool checkedAuditSum(std::uint64_t lhs, std::uint64_t rhs,
                            std::uint64_t& result) noexcept
{
  constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
  if (rhs > kMax - lhs)
    return false;
  result = lhs + rhs;
  return true;
}

inline bool exactAuditRecords(std::uint64_t requested,
                              std::uint64_t reads,
                              std::uint64_t accepted) noexcept
{
  return accepted == requested && reads == requested;
}

inline bool boundedAuditSuperset(std::uint64_t requested,
                                 std::uint64_t reads,
                                 std::uint64_t accepted,
                                 std::uint64_t upper) noexcept
{
  return accepted == requested && reads >= accepted && reads <= upper;
}

inline bool incidenceUpperBoundFits(std::uint64_t upper,
                                    std::uint64_t arc_slots) noexcept
{
  constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
  return arc_slots > kMax / 2 || upper <= arc_slots * 2;
}

}  // namespace detail

inline bool validateNarrowCaptureAudit(
    const NarrowCaptureAudit& audit,
    const NarrowCaptureAuditExpectation& expected) noexcept
{
  std::uint64_t captured = 0;
  std::uint64_t requests = 0;
  if (!detail::checkedAuditSum(audit.accepted_arcs,
                               audit.accepted_deactivations, captured)
      || !detail::checkedAuditSum(audit.requested_arcs,
                                  audit.requested_deactivations, requests))
    return false;
  const bool lookup_valid
      = requests == 0 ? audit.index_group_lookups == 0
                              && audit.native_arc_candidate_reads == 0
                              && audit.native_arc_candidate_upper_bound
                                     == 0
                      : audit.index_group_lookups > 0
                            && audit.index_group_lookups <= requests;
  return audit.full_export_guard_active
         && !audit.full_export_observed
         && audit.native_full_graph_exports == 0
         && audit.native_full_reference_exports == 0
         && audit.rejected_reads == 0
         && audit.captured_arc_records == captured
         && detail::boundedAuditSuperset(
                audit.requested_nodes, audit.node_reads,
                audit.accepted_nodes, expected.after.node_slots)
         && detail::exactAuditRecords(
                audit.requested_instances, audit.instance_reads,
                audit.accepted_instances)
         && detail::boundedAuditSuperset(
                audit.requested_arcs,
                audit.native_arc_candidate_reads,
                audit.accepted_arcs,
                audit.native_arc_candidate_upper_bound)
         && detail::incidenceUpperBoundFits(
                audit.native_arc_candidate_upper_bound,
                expected.after.arc_slots)
         && detail::exactAuditRecords(
                audit.requested_deactivations,
                audit.deactivated_arc_before_reads,
                audit.accepted_deactivations)
         && audit.requested_deactivations
                == expected.logical_deactivations
         && detail::exactAuditRecords(
                audit.requested_load_nets, audit.net_reads,
                audit.accepted_load_nets)
         && detail::exactAuditRecords(
                audit.requested_library_cells,
                audit.library_cell_reads,
                audit.accepted_library_cells)
         && detail::exactAuditRecords(
                audit.requested_cell_pin_capacitances,
                audit.cell_pin_capacitance_reads,
                audit.accepted_cell_pin_capacitances)
         && detail::exactAuditRecords(
                audit.requested_node_roles, audit.node_role_reads,
                audit.accepted_node_roles)
         && lookup_valid
         && audit.arc_identity_tie_breaks <= audit.accepted_arcs;
}

}  // namespace ista::timing_dual_csr
