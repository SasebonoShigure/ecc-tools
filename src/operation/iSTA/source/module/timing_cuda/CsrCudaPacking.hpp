#pragma once

#include <vector>

#include "CsrCudaDeviceTypes.hpp"
#include "CsrReferenceTypes.hpp"
#include "ActiveCsrEdgeView.hpp"

namespace ista::csr::cuda_detail {
struct ArcLayout
{
  Index child_begin{0}, child_count{0};
  bool active{true};
};
// Only identities and static ideal-clock lanes needed to decode actual results.
struct ResultLayout
{
  std::vector<ArcLayout> arcs;
  std::vector<std::int32_t> child_indices;
  std::vector<std::string> clock_names, clock_tags;
  std::vector<std::uint8_t> is_clock_point;
  ClockResultSoa effective_clock;
};
struct DownloadData
{
  std::vector<NodeState> nodes;
  std::vector<PathState> paths;
  std::vector<ArcState> arc_state;
  std::vector<ChildState> child_state;
  std::vector<Index> order, node_levels, level_offsets;
};
struct HostData : DownloadData
{
  std::vector<Arc> arcs;
  std::vector<Child> children;
  std::vector<double> table_data;
  std::vector<Index> out_offsets, out_arcs, in_offsets, in_arcs;
  std::vector<Index> roots, ends;
  std::vector<Endpoint> endpoints;
  ResultLayout layout;
  CsrSnapshotIdentity identity;
  std::string schema;
  Index tag_count{0};
  Index clock_count{0};
};

[[nodiscard]] Arc packArc(const ArcSemanticPayload& source, std::vector<Child>& children, std::vector<double>& tables,
                          Index child_base = 0, Index table_base = 0);
[[nodiscard]] HostData pack(const CsrReferenceInput& input);
[[nodiscard]] CsrTimingValues unpackValues(DownloadData&& data, const ResultLayout& layout);
[[nodiscard]] CsrTimingResult unpack(HostData&& data);

}  // namespace ista::csr::cuda_detail
