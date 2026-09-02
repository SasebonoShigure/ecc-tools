// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrStructuredBeforeBindingInternal.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "AffectedCapturePlan.hpp"
#include "DualCsrArcGroupIndex.hpp"
#include "DualCsrStructuredCapturePlanInternal.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("dual csr structured prepare: "
                              + std::move(message));
}

}  // namespace

void requireStructuredExactBefore(
    const DualCsrState& graph,
    const timing_edit::TimingEditResolverState& resolver,
    const timing_edit::CommittedEditIdentity& identity,
    const DualCsrArcGroupIndex& arc_group_index,
    const DualSemanticReadView& view)
{
  if (graph.session_token != view.sessionToken()
      || graph.generation != view.generation()
      || graph.high_water != view.highWater()
      || graph.reference_payload != view.referencePayload()
      || resolver.highWater() != graph.high_water
      || identity.generation != graph.generation
      || arc_group_index.session_token != graph.session_token
      || arc_group_index.generation != graph.generation
      || arc_group_index.arc_high_water != graph.high_water.arc_slots
      || arc_group_index.slots.size() != graph.arc_slots.size())
    invalid("exact committed values disagree at the view boundary");
}

void requireStructuredSelectedBefore(
    const DualCsrState& graph,
    const timing_edit::TimingEditResolverState& resolver,
    const DualCsrArcGroupIndex& arc_group_index,
    const DualSemanticReadView& view,
    const AffectedCapturePlan& plan)
{
  for (const auto& request : plan.nodes) {
    const auto binding = resolver.nodeBindings().find(request.ref);
    if (binding == resolver.nodeBindings().end())
      continue;
    const auto id = binding->second.slot.value();
    if (binding->second.tombstone || id >= graph.nodes.size()
        || binding->second.native_name != request.expected_key.full_name)
      invalid("selected resolver node binding is not exact");
    const auto record = view.nodeById(csr::NodeId{id});
    if (!record || record.value() != graph.nodes[id]
        || record.value().key != request.expected_key)
      invalid("selected node differs between graph and keyed view");
  }

  for (const auto& request : plan.arcs) {
    if (!request.expected_current_key)
      continue;
    const auto binding = resolver.arcBindings().find(request.ref);
    if (binding == resolver.arcBindings().end()
        || binding->second.tombstone
        || binding->second.slot.value() >= graph.arc_slots.size())
      invalid("selected resolver arc binding is not exact");
    const csr::ArcId id{binding->second.slot.value()};
    const auto* indexed = arc_group_index.slot(id);
    const auto payload = view.arcById(id);
    if (indexed == nullptr || indexed->ref != request.ref
        || indexed->current_active_key != request.expected_current_key
        || !graph.arc_slots[id.value()].active
        || graph.arc_slots[id.value()].payload.key
               != *request.expected_current_key
        || !payload
        || payload.value() != graph.arc_slots[id.value()].payload)
      invalid("selected arc differs across resolver/index/graph/view");
  }
}

std::uint64_t structuredGraphValueCopyMinimumBytes(
    const DualCsrState& graph) noexcept
{
  return sizeof(graph)
         + graph.nodes.size() * sizeof(csr::NodeRecord)
         + graph.arc_slots.size() * sizeof(DualCsrArcSlot)
         + graph.base_out.row_offsets.size() * sizeof(std::uint64_t)
         + graph.base_out.physical_slots.size()
               * sizeof(std::optional<csr::ArcId>)
         + graph.base_in.row_offsets.size() * sizeof(std::uint64_t)
         + graph.base_in.physical_slots.size()
               * sizeof(std::optional<csr::ArcId>)
         + graph.aux_out.row_offsets.size() * sizeof(std::uint64_t)
         + graph.aux_out.row_sources.size() * sizeof(csr::NodeId)
         + graph.aux_out.arc_ids.size() * sizeof(csr::ArcId)
         + graph.aux_out.source_to_aux_row.size() * sizeof(std::int64_t);
}

}  // namespace ista::timing_dual_csr::detail
