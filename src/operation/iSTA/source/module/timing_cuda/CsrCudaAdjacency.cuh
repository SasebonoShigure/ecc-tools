#pragma once
#include "CsrCudaDeviceTypes.hpp"
#include "IncreGpuStaBuildConfig.hpp"

namespace ista::csr::cuda_detail {
struct Outgoing
{
  DeviceView view;
  Index node, position, end, aux_position{0}, aux_end{0};
  const Index* aux_arcs{nullptr};
  unsigned stage;
  __device__ bool next(Index& arc)
  {
    while (position < end || aux_position < aux_end) {
      const bool aux = position >= end;
      arc = aux ? aux_arcs[aux_position++] : view.out_arcs[position++];
      if (arc == absent)
        continue;
      if (arc >= view.arc_count || !view.arcs[arc].active || view.arcs[arc].source != node || view.arcs[arc].sink >= view.node_count) {
        *view.status = Status::invariant;
        return false;
      }
      if constexpr (incregpusta_debug_checks_enabled)
        if (view.storage_visits)
          ++view.storage_visits[stage * 2 + (aux ? 1 : 0)];
      return true;
    }
    return false;
  }
};
__device__ inline Outgoing outgoing(DeviceView view, Index node, unsigned stage)
{
  Outgoing result{view, node, view.out_offsets[node], view.out_offsets[node + 1], 0, 0, nullptr, stage};
  if (view.aux_rows) {
    const auto row = view.aux_rows[node];
    if (row.count && !row.arcs)
      *view.status = Status::invariant;
    else {
      result.aux_arcs = row.arcs;
      result.aux_end = row.count;
    }
  }
  return result;
}
}  // namespace ista::csr::cuda_detail
