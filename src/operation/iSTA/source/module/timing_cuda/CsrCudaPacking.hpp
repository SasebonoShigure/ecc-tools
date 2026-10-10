#pragma once

#include <array>
#include <string_view>
#include <span>
#include <map>
#include <unordered_map>
#include <vector>

#include "CsrCudaDeviceTypes.hpp"
#include "CsrReferenceTypes.hpp"
#include "ActiveCsrEdgeView.hpp"
#include "CsrRcDriverCache.hpp"
#include "DualCsrState.hpp"
#include "TimingPayloadView.hpp"

namespace ista::csr::cuda_detail {
struct ArcLayout
{
  Index child_begin{0}, child_count{0};
  bool active{true};
};
struct ArcBinding
{
  Index source{}, sink{};
  bool active{true};
  bool levelization{false};
  EdgeStorageKind storage_kind{EdgeStorageKind::kBaseOut};
  std::uint8_t path_transitions{0};
  Index rc_binding{absent};
};
struct PathDomain
{
  std::vector<Index> offsets, tags;
  friend bool operator==(const PathDomain&, const PathDomain&) = default;
};
struct PathAdjacency
{
  std::vector<Index> incoming, outgoing;
};
[[nodiscard]] std::vector<PathAdjacency> buildPathAdjacency(Index nodes, std::span<const ArcBinding> arcs);
void updatePathDomain(std::span<const ArcBinding> arcs, std::span<const PathAdjacency> adjacency,
                      std::span<const PathSeed> seeds, const PathDomain& previous,
                      std::span<const Index> changed_nodes, PathDomain& result);
[[nodiscard]] std::uint8_t pathTransitions(ArcKind kind, const CellTransferModel& transfer, bool data_arrival);
[[nodiscard]] PathDomain buildPathDomain(std::span<const ArcBinding> arcs, std::span<const PathAdjacency> adjacency, std::span<const PathSeed> seeds);
// Stable identities needed to decode the actual device results.
struct ResultLayout
{
  std::vector<ArcLayout> arcs;
  std::vector<std::int32_t> child_indices;
  std::vector<std::string> clock_names, clock_tags;
};
struct DownloadData
{
  std::vector<NodeState> nodes;
  std::vector<PathState> paths;
  std::vector<ArcState> arc_state;
  std::vector<ChildState> child_state;
};
// Exact value interning; signatures alone do not identify a Liberty model.
struct ModelStorage
{
  std::vector<Child> children;
  std::vector<double> tables;
  std::vector<RcLoadCell> rc_load_cells;
  [[nodiscard]] Index libraryIndex(std::string_view name);
  [[nodiscard]] Index internLoadCell(const ReferencedLibraryCellModel& cell);
  [[nodiscard]] Index intern(const std::shared_ptr<const std::vector<ChildTimingArcModel>>& owner);

 private:
  struct Entry
  {
    std::shared_ptr<const std::vector<ChildTimingArcModel>> source;
    Index begin;
  };
  std::map<std::string, Index, std::less<>> _libraries;
  std::map<std::pair<std::string, std::string>, Index> _load_cells;
  std::vector<Entry> _entries;
  std::map<std::shared_ptr<const std::vector<ChildTimingArcModel>>, Index, std::owner_less<>> _owners;
  std::unordered_multimap<std::size_t, Index> _lookup;
};
struct PackedRcTree
{
  std::array<Index, 4> lanes{};
  std::vector<RcModel> models;
  std::vector<RcTerminal> terminals;
};
// Caller keeps this staging alive through its upload. Reuse capacity for changed
// nets; do not append old revisions to the immutable Liberty table pool.
void packRcTree(const CsrParasiticTree& source, PackedRcTree& target);

struct RcChildGroup { Index begin, count, model; };
struct HostRcNode
{
  CsrParasiticBinding binding;
  bool port{false};
  Index load_cell{absent};
};
struct HostRcNet
{
  std::shared_ptr<const CsrParasiticTree> owner;
  PackedRcTree tree;
  std::vector<Index> initialize_arcs;
  std::vector<CsrRcTimingRow> rows;
  Index child_count{0}, record_count{0};
  std::map<std::pair<bool, Index>, RcChildGroup> groups;
};
[[nodiscard]] RcBinding packRcBinding(HostRcNet& net, Index net_index, Index terminal,
                                      bool instance, Index ordinal, Index child_begin, Index child_count);
void finishRcNet(HostRcNet& net);

struct RcHostState
{
  std::map<Index, HostRcNode> nodes;
  std::vector<RcBinding> bindings;
  std::vector<Index> net_by_node;
};

struct HostData
{
  std::vector<Arc> arcs;
  std::vector<HostRcNet> rc_nets;
  RcHostState rc;
  std::vector<PathSeed> path_seeds;
  std::vector<NodeSeed> node_seeds;
  Index node_count{};
  Index child_state_count{};
  std::vector<Index> out_offsets, out_arcs, in_offsets, in_arcs;
  std::vector<Index> roots, ends;
  std::vector<Endpoint> endpoints;
  ResultLayout layout;
  std::vector<ArcBinding> bindings;
  std::vector<PathAdjacency> adjacency;
  PathDomain paths;
  CsrSnapshotIdentity identity;
  std::string schema;
  Index tag_count{0};
  Index clock_count{0};
};

void packRcGraph(const CsrReferenceInput& input, HostData& data, ModelStorage& models);
void packRcGraph(const timing_dual_csr::DualCsrState& input, HostData& data, ModelStorage& models);
void packTimingPayload(const TimingPayloadView& input, HostData& data, ModelStorage& models);
void finishPacking(HostData& data);

[[nodiscard]] Arc packArc(const ArcSemanticPayload& source, ModelStorage& models);
[[nodiscard]] HostData pack(const CsrReferenceInput& input, ModelStorage& models);
// Internal full rebuild from stable slots; inactive IDs remain tombstones.
[[nodiscard]] HostData pack(const timing_dual_csr::DualCsrState& input, ModelStorage& models);
[[nodiscard]] CsrTimingValues unpackValues(DownloadData&& data, const ResultLayout& layout, const PathDomain& domain);

}  // namespace ista::csr::cuda_detail
