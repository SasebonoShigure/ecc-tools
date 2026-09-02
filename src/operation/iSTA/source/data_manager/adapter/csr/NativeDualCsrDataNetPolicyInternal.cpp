// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeDualCsrDataNetPolicyInternal.hpp"

#include <algorithm>
#include <string>

#include "Database.hpp"

namespace ista::timing_dual_csr::detail {

std::optional<NarrowCaptureError> validateNativeDataOnlyNets(
    const ista::Database& database, const AffectedCapturePlan& plan)
{
  for (const auto& edit : plan.normalized_edits) {
    const AffectedNetCapture* requested = nullptr;
    for (const auto& net : plan.load_nets) {
      if (net.ref != edit.original_net)
        continue;
      if (requested != nullptr)
        return NarrowCaptureError::kDuplicateRequest;
      requested = &net;
    }
    if (requested == nullptr)
      return NarrowCaptureError::kUnplannedDependency;
    const auto found = database.get_net_map().find(
        requested->expected_name);
    if (found == database.get_net_map().end()
        || found->second.get_net_name() != requested->expected_name)
      return NarrowCaptureError::kNativeNetNotFound;
    const auto& members = found->second.get_pin_name_list();
    for (const auto& [clock_name, clock] :
         database.get_timing_constraint().get_clock_map()) {
      static_cast<void>(clock_name);
      if (std::ranges::any_of(
              clock.get_source_list(), [&](const std::string& source) {
                return std::ranges::find(members, source)
                       != members.end();
              }))
        return NarrowCaptureError::kUnsupportedPolicy;
    }
    for (const auto& pin_name : members) {
      const auto pin = database.get_pin_map().find(pin_name);
      if (pin == database.get_pin_map().end()
          || pin->second.get_net_name() != requested->expected_name)
        return NarrowCaptureError::kMalformedNativeRecord;
      if (pin->second.get_is_port())
        continue;
      const auto instance = database.get_instance_map().find(
          pin->second.get_instance_name());
      if (instance == database.get_instance_map().end())
        return NarrowCaptureError::kMalformedNativeRecord;
      const auto cell = database.get_timing_library().get_cell_map().find(
          instance->second.get_cell_name());
      if (cell == database.get_timing_library().get_cell_map().end())
        return NarrowCaptureError::kMalformedNativeRecord;
      const auto port = cell->second.get_port_map().find(
          pin->second.get_pin_name());
      if (port == cell->second.get_port_map().end())
        return NarrowCaptureError::kMalformedNativeRecord;
      if (port->second.get_is_clock())
        return NarrowCaptureError::kUnsupportedPolicy;
    }
  }
  return std::nullopt;
}

}  // namespace ista::timing_dual_csr::detail
