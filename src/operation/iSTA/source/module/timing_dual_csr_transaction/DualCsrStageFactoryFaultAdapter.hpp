// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "TimingEditDualCsrDescriptorBuilder.hpp"

namespace ista::timing_dual_csr::test {

// Test-only input adapter. It can only corrupt a detached, move-only batch;
// production still receives the original two-argument public factory call and
// every failure is produced by the real validator/updater/backend path.
class DualCsrStageFactoryFaultAdapter final
{
 public:
  static void corruptIndexDelta(DualCsrBatchDescriptor& descriptor)
  {
    ArcGroupIndexDelta invalid;
    invalid.id = csr::ArcId{descriptor.after.arc_slots};
    descriptor.arc_group_index_delta.push_back(std::move(invalid));
  }

  static void corruptIndexDelta(VerifiedStructuredBatch& batch)
  {
    corruptIndexDelta(batch._descriptor);
  }

  [[nodiscard]] static bool corruptUpdaterDescriptor(
      DualCsrBatchDescriptor& descriptor)
  {
    if (descriptor.node_appends.empty())
      return false;
    descriptor.node_appends.front().id
        = csr::NodeId{descriptor.after.node_slots};
    return true;
  }

  [[nodiscard]] static bool corruptUpdaterDescriptor(
      VerifiedStructuredBatch& batch)
  {
    return corruptUpdaterDescriptor(batch._descriptor);
  }

  [[nodiscard]] static bool corruptIntrinsicTimingInput(
      VerifiedStructuredBatch& batch, const DualCsrState& before)
  {
    auto descriptor = batch._descriptor;
    if (!before.reference_payload.clocks.empty())
      return false;

    std::optional<csr::NodeId> endpoint;
    for (const auto id : before.reference_payload.ordered_end_ids) {
      if (id.value() >= before.nodes.size())
        continue;
      const auto& node = before.nodes[id.value()];
      if (node.is_port
          && (node.direction == csr::PinDirection::kOutput
              || node.direction == csr::PinDirection::kInout)) {
        endpoint = id;
        break;
      }
    }
    if (!endpoint)
      return false;

    const double maximum = std::numeric_limits<double>::max();
    auto expected_clocks = before.reference_payload.clocks;
    auto replacement_clocks = expected_clocks;
    csr::ClockConstraintModel clock;
    clock.map_key = "clk";
    for (const auto& seed : before.reference_payload.start_seeds)
      if (!seed.clock_tag.empty()) {
        clock.map_key = seed.clock_tag;
        break;
      }
    clock.period = maximum;
    replacement_clocks.push_back(std::move(clock));

    auto expected_ports = before.reference_payload.port_constraints;
    auto replacement_ports = expected_ports;
    const auto existing_port = std::ranges::find(
        replacement_ports, *endpoint,
        &csr::PortConstraintModel::resolved_node);
    if (existing_port == replacement_ports.end()) {
      const auto& node = before.nodes[endpoint->value()];
      csr::PortConstraintModel added;
      added.map_key = node.key.full_name;
      added.embedded_port_name = node.key.pin_name;
      added.resolved_node = *endpoint;
      replacement_ports.push_back(std::move(added));
    }
    const auto port = std::ranges::find(
        replacement_ports, *endpoint,
        &csr::PortConstraintModel::resolved_node);
    if (port == replacement_ports.end())
      return false;
    for (auto& value : port->output_delay)
      value = -maximum;
    std::ranges::sort(replacement_ports, {},
                      &csr::PortConstraintModel::map_key);

    auto expected_required = before.reference_payload.endpoint_required;
    auto replacement_required = expected_required;
    const auto required = std::ranges::find(
        replacement_required, *endpoint,
        &csr::EndpointRequiredModel::node);
    if (required == replacement_required.end())
      return false;
    for (auto& row : replacement_required)
      row.clock_period = maximum;
    for (auto& value : required->output_delay)
      value = -maximum;

    descriptor.reference_payload.clocks
        = ValueReplacement<std::vector<csr::ClockConstraintModel>>{
            std::move(expected_clocks), std::move(replacement_clocks)};
    descriptor.reference_payload.port_constraints
        = ValueReplacement<std::vector<csr::PortConstraintModel>>{
            std::move(expected_ports), std::move(replacement_ports)};
    descriptor.reference_payload.endpoint_required
        = ValueReplacement<std::vector<csr::EndpointRequiredModel>>{
            std::move(expected_required), std::move(replacement_required)};
    batch._descriptor = std::move(descriptor);
    return true;
  }
};

}  // namespace ista::timing_dual_csr::test
