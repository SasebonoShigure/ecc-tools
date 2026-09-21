#pragma once
#include <limits>
#include <set>

#include "CsrCudaMemory.cuh"
#include "CsrCudaPacking.hpp"
#include "CudaDualCsrSession.hpp"

namespace ista::csr::cuda_detail {
inline Index checkedAdd(Index a, Index b)
{
  if (b > std::numeric_limits<Index>::max() - a)
    throw std::length_error("CUDA targeted size overflow");
  return a + b;
}
inline Index checkedProduct(Index a, Index b)
{
  if (a && b > std::numeric_limits<Index>::max() / a)
    throw std::length_error("CUDA targeted capacity overflow");
  return a * b;
}
inline void demand(bool value, const char* message)
{
  if (!value)
    throw std::invalid_argument(message);
}
struct SlotPatch
{
  Index row{}, slot{}, expected{absent}, replacement{absent};
};
struct PayloadPatch
{
  Index id{};
  Arc expected, replacement;
};
struct DeactivatePatch
{
  Index id{}, source{}, sink{};
};
struct AuxPatch
{
  Index source{}, expected_offset{}, expected_count{}, replacement_offset{}, replacement_count{};
};
struct AuxStorageRow
{
  DeviceBuffer<Index> buffer;
  Index count{};
};
struct AuxBinding
{
  Index source{};
  AuxRow before;
  Index replacement_count{};
};
struct PackedPatch
{
  std::vector<SlotPatch> removes, changes, reverse_changes;
  std::vector<PayloadPatch> payloads;
  std::vector<DeactivatePatch> deactivations;
  std::vector<Arc> appends;
  std::vector<AuxPatch> aux_patches;
  std::vector<Index> aux_values;
};
struct DevicePatch
{
  const SlotPatch *removes{}, *changes{}, *reverse_changes{};
  Index remove_count{}, change_count{}, reverse_count{};
  const PayloadPatch* payloads{};
  Index payload_count{};
  const DeactivatePatch* deactivations{};
  Index deactivate_count{};
  const AuxPatch* aux_patches{};
  Index aux_patch_count{};
  const Index* aux_values{};
};
struct SlotUndo
{
  Index* location{};
  Index value{};
};
struct ArcUndo
{
  Index id{};
  Arc value;
};
struct NewNodePlacement
{
  Index node, position;
};
// The one host graph used by the committed owner and its exclusive candidate.
// Result path domains belong to the double-buffered calculation, not this state.
struct HostGraphState
{
  std::vector<Index> out_offsets, in_offsets, roots, ends;
  std::vector<ArcBinding> bindings;
  std::vector<PathAdjacency> adjacency;
  ResultLayout layout;
  std::vector<NodeSeed> node_seeds;
  std::vector<PathSeed> path_seeds;
};
struct HostGraphUndo
{
  std::shared_ptr<HostGraphState> target;
  Index nodes{}, arcs{}, children{}, names{}, node_seeds{}, path_seeds{};
  std::vector<std::pair<Index, ArcBinding>> bindings;
  std::vector<std::pair<Index, ArcLayout>> layouts;
  std::vector<std::pair<Index, std::int32_t>> child_indices;
  struct RowSize { Index node, incoming, outgoing; };
  std::vector<RowSize> adjacency;
  std::optional<std::vector<Index>> roots, ends;

  void clear() noexcept
  {
    target.reset();
    bindings.clear();
    layouts.clear();
    child_indices.clear();
    adjacency.clear();
    roots.reset();
    ends.reset();
  }
  void restore() noexcept
  {
    if (!target)
      return;
    for (const auto& [id, value] : bindings)
      target->bindings[id] = value;
    for (const auto& [id, value] : layouts)
      target->layout.arcs[id] = value;
    for (const auto& [id, value] : child_indices)
      target->layout.child_indices[id] = value;
    for (const auto& row : adjacency) {
      target->adjacency[row.node].incoming.resize(row.incoming);
      target->adjacency[row.node].outgoing.resize(row.outgoing);
    }
    target->adjacency.resize(nodes);
    target->out_offsets.resize(nodes + 1);
    target->in_offsets.resize(nodes + 1);
    target->bindings.resize(arcs);
    target->layout.arcs.resize(arcs);
    target->layout.child_indices.resize(children);
    target->layout.clock_names.resize(names);
    target->node_seeds.resize(node_seeds);
    target->path_seeds.resize(path_seeds);
    if (roots)
      target->roots.swap(*roots);
    if (ends)
      target->ends.swap(*ends);
    // Keep replacement flags until the corresponding device roots/ends have
    // been restored and all asynchronous reads of these vectors have finished.
  }
};
struct PathDomainBuffers
{
  PathDomain host;
  DeviceBuffer<Index> offsets, tags;
};
struct ResultBuffers
{
  DeviceBuffer<Index> order, node_levels, level_offsets, level_offset_count, storage_visits;
  DeviceBuffer<NodeState> nodes;
  DeviceBuffer<PathState> paths;
  DeviceBuffer<ArcState> arcs;
  DeviceBuffer<ChildState> children;
  DeviceBuffer<Status> status;
};
struct FullGraphBuffers
{
  DeviceBuffer<Index> out_offsets, in_offsets, roots, ends;
  DeviceBuffer<NodeSeed> node_seeds;
  DeviceBuffer<PathSeed> path_seeds;
  DeviceBuffer<Index> out_arcs, in_arcs;
  DeviceBuffer<AuxRow> aux_rows;
  std::vector<AuxStorageRow> aux_storage;
  DeviceBuffer<Arc> arcs;
  DeviceBuffer<Endpoint> endpoints;
};
struct DeviceStorage
{
  explicit DeviceStorage(CsrCudaTelemetry& telemetry) : memory(telemetry) {}
  Runtime memory;
  ModelStorage models;
  Index uploaded_children{}, uploaded_tables{};
  FullGraphBuffers graph, full_work;
  HostGraphUndo host_undo;
  bool full_graph_pending{false};
  DeviceBuffer<Index> indegrees;
  std::vector<AuxBinding> aux_bindings;
  DeviceBuffer<Child> children;
  DeviceBuffer<double> tables;
  ResultBuffers results[2];
  PathDomainBuffers domains[2];
  DeviceBuffer<SlotPatch> removes, changes, reverse_changes;
  DeviceBuffer<PayloadPatch> payloads;
  DeviceBuffer<DeactivatePatch> deactivations;
  DeviceBuffer<AuxPatch> aux_patches;
  DeviceBuffer<Index> aux_values;
  DeviceBuffer<SlotUndo> slot_undo;
  DeviceBuffer<ArcUndo> arc_undo;
  DeviceBuffer<NewNodePlacement> placements;
  Index slot_undo_count{}, arc_undo_count{};
  bool pending{false}, undo_captured{false}, poisoned{false}, closed{false};
};
void launchCaptureUndo(DeviceView view, SlotUndo* slots, Index slot_count, ArcUndo* arcs, Index arc_count, cudaStream_t stream);
void launchRestoreUndo(DeviceView view, const SlotUndo* slots, Index slot_count, const ArcUndo* arcs, Index arc_count, cudaStream_t stream);
void launchTargetedPatch(DeviceView view, DevicePatch patch, cudaStream_t stream);
}  // namespace ista::csr::cuda_detail

namespace ista::csr {
struct CudaDualCsrSession::Impl
{
  CudaDualCsrTelemetry telemetry;
  std::shared_ptr<cuda_detail::DeviceStorage> storage;
  cuda_detail::Runtime& memory;
  unsigned result_index{}, domain_index{};
  cuda_detail::DeviceView view;
  cuda_detail::NodeSeed* seed_nodes{};
  cuda_detail::PathSeed* seed_paths{};
  cuda_detail::Index table_count{};
  std::shared_ptr<cuda_detail::HostGraphState> host;
  timing_edit::TimingEditSessionToken token;
  timing_edit::TransactionGeneration generation;
  timing_edit::AppendOnlyHighWater high_water;
  bool runtime_owner{false};
  std::shared_ptr<Impl> origin;
  mutable bool poisoned{false};
  std::optional<CsrComputationCode> code;
  CsrFifoLevelization levels;
  std::optional<CsrTimingValues> values;

  Impl(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water, std::shared_ptr<Impl> before = {});
  Impl(std::shared_ptr<Impl> before, const CudaDualCsrPatch& patch, CudaLevelizationMode mode);
  static void validatePatch(const Impl& before, const CudaDualCsrPatch& patch);
  cuda_detail::PackedPatch prepare(const Impl& before, const CudaDualCsrPatch& patch);
  ~Impl();
  void allocate();
  void bindGraph() noexcept;
  void bindAuxRows();
  std::vector<std::vector<cuda_detail::Index>> readAuxRows();
  void cancel() noexcept;
  CsrCudaTelemetry restorePending() noexcept;
  void execute(CudaLevelizationMode mode = CudaLevelizationMode::kFull);
  const CsrTimingValues& materialize();
  std::optional<CsrFifoLevelization> reconstructMixed();
  void transferPatch(const Impl& before, const CudaDualCsrPatch& patch, const cuda_detail::PackedPatch& packed);
};
}  // namespace ista::csr
