// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CsrCudaPacking.hpp"

#include <limits>
#include <stdexcept>

#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"

namespace ista::csr::cuda_detail {
HostData pack(const timing_dual_csr::DualCsrState& input, ModelStorage& models)
{
  if (input.high_water.node_slots != input.nodes.size() || input.high_water.arc_slots != input.arc_slots.size()
      || input.nodes.size() == std::numeric_limits<std::size_t>::max()
      || !input.reference_payload.unsupported_reasons.empty())
    throw std::invalid_argument("CUDA full CSR rebuild has invalid extents or unsupported semantics");
  HostData data;
  data.arcs.resize(input.arc_slots.size());
  data.layout.arcs.resize(input.arc_slots.size());
  data.bindings.resize(input.arc_slots.size());
  for (std::size_t id = 0; id < input.arc_slots.size(); ++id) {
    const auto& slot = input.arc_slots[id];
    auto& arc = data.arcs[id];
    arc.active = slot.active;
    data.layout.arcs[id].active = slot.active;
    data.bindings[id].active = slot.active;
    if (!slot.active)
      continue;
    const auto& source = slot.payload;
    if (source.source.value() >= input.nodes.size() || source.sink.value() >= input.nodes.size())
      throw std::invalid_argument("CUDA full CSR rebuild has an invalid arc endpoint");
    arc = packArc(source, models);
    arc.child_state_begin = data.child_state_count;
    if (arc.child_count > absent - data.child_state_count)
      throw std::length_error("CUDA full CSR child state capacity overflow");
    data.child_state_count += arc.child_count;
    data.layout.arcs[id] = {arc.child_state_begin, arc.child_count, true};
    for (const auto& child : source.transfer.children())
      data.layout.child_indices.push_back(child.signature.arc_index);
    data.bindings[id] = {arc.source, arc.sink, true, arc.levelization, EdgeStorageKind::kBaseOut,
                        pathTransitions(source.kind, source.transfer, source.eligibility.data_arrival), absent};
  }
  // Rebuild complete compact Base rows, retaining each active stable ArcId.
  // This is full graph work; it neither exports native objects nor patches the
  // preceding device adjacency.
  const timing_dual_csr::DualCsrEdgeViewImpl edges(input);
  data.out_offsets.reserve(input.nodes.size() + 1);
  data.in_offsets.reserve(input.nodes.size() + 1);
  data.out_offsets.push_back(0);
  data.in_offsets.push_back(0);
  for (std::size_t node = 0; node < input.nodes.size(); ++node) {
    const auto append = [](std::vector<Index>& values) {
      return [&values](const EdgeEntry& edge) {
        values.push_back(edge.arc_id.value());
        return EdgeVisitResult::continueVisit();
      };
    };
    const auto out = edges.forEachOutgoing(NodeId{node}, append(data.out_arcs));
    const auto in = edges.forEachIncoming(NodeId{node}, append(data.in_arcs));
    if (out.control != EdgeVisitControl::kContinue || in.control != EdgeVisitControl::kContinue || out.error || in.error)
      throw std::invalid_argument("CUDA full CSR rebuild has malformed adjacency");
    data.out_offsets.push_back(data.out_arcs.size());
    data.in_offsets.push_back(data.in_arcs.size());
  }
  packTimingPayload(timing_dual_csr::DualCsrTimingPayloadViewImpl(input), data, models);
  packRcGraph(input, data, models);
  finishPacking(data);
  return data;
}
}  // namespace ista::csr::cuda_detail
