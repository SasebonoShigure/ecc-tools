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
struct SeedLoadPatch
{
  Index slot{};
  double expected[4]{}, replacement[4]{};
};
struct SeedLoadUndo
{
  Index slot{};
  double value[4]{};
};
struct PackedPatch
{
  std::vector<SlotPatch> removes, changes, reverse_changes;
  std::vector<PayloadPatch> payloads;
  std::vector<SeedLoadPatch> seed_loads;
  std::vector<SeedLoadUndo> seeds;
  std::vector<DeactivatePatch> deactivations;
  std::vector<Arc> appends;
  std::vector<AuxPatch> aux_patches;
  std::vector<Index> aux_values, appended_out, appended_in;
  std::vector<Endpoint> endpoints;
  std::vector<SlotUndo> slots;
  std::vector<ArcUndo> arcs;
};
struct DevicePatch
{
  const SlotPatch *removes{}, *changes{}, *reverse_changes{};
  Index remove_count{}, change_count{}, reverse_count{};
  const PayloadPatch* payloads{};
  Index payload_count{};
  NodeSeed* seed_nodes{};
  const SeedLoadPatch* seed_loads{};
  Index seed_load_count{};
  const DeactivatePatch* deactivations{};
  Index deactivate_count{};
  const AuxPatch* aux_patches{};
  Index aux_patch_count{};
  const Index* aux_values{};
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
  RcHostState rc;
};
struct RcBindingUpdate { Index index; RcBinding value; };
struct HostGraphUndo
{
  std::shared_ptr<HostGraphState> target;
  Index nodes{}, arcs{}, children{}, names{}, node_seeds{}, path_seeds{};
  std::vector<std::pair<Index, ArcBinding>> bindings;
  std::vector<SeedLoadUndo> seed_loads;
  std::vector<std::pair<Index, ArcLayout>> layouts;
  std::vector<std::pair<Index, std::int32_t>> child_indices;
  struct RowSize { Index node, incoming, outgoing; };
  std::vector<RowSize> adjacency;
  std::optional<std::vector<Index>> roots, ends;
  bool rc_applied{false};
  Index rc_binding_count{}, rc_node_count{};
  std::map<Index, HostRcNode> rc_nodes;
  std::vector<Index> new_rc_nodes;
  std::vector<RcBindingUpdate> rc_bindings;

  void clear() noexcept
  {
    target.reset();
    bindings.clear();
    seed_loads.clear();
    layouts.clear();
    child_indices.clear();
    adjacency.clear();
    roots.reset();
    ends.reset();
    rc_applied = false;
    rc_nodes.clear();
    new_rc_nodes.clear();
    rc_bindings.clear();
  }
  void restore() noexcept
  {
    if (!target)
      return;
    if (rc_applied) {
      for (const auto& [id, value] : rc_nodes) target->rc.nodes.at(id) = value;
      for (const auto id : new_rc_nodes) target->rc.nodes.erase(id);
      for (const auto& [id, value] : rc_bindings) target->rc.bindings[id] = value;
      target->rc.bindings.resize(rc_binding_count);
      target->rc.net_by_node.resize(rc_node_count);
      rc_applied = false;
    }
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
    for (const auto& saved : seed_loads)
      for (unsigned lane = 0; lane < 4; ++lane)
        target->node_seeds[saved.slot].state.sequential.output_load[lane] = saved.value[lane];
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
struct RcNetBuffers
{
  std::shared_ptr<const CsrParasiticTree> owner;
  DeviceBuffer<RcModel> models;
  DeviceBuffer<RcTerminal> terminals;
  DeviceBuffer<CsrRcTimingRow> rows;
  DeviceBuffer<CsrRcTimingRecord> records;
  DeviceBuffer<CsrRcDriverLane> drivers;
  DeviceBuffer<Index> initialize_arcs;
};
struct RcNetUpdate { Index index; RcNet value; };
struct RcPatchStorage
{
  std::map<Index, HostRcNet> nets;
  std::vector<RcBindingUpdate> bindings;
  std::vector<RcNetUpdate> views, undo_views;
  std::vector<RcNetBuffers> work_nets;
  std::vector<RcNet> work_views;
  DeviceBuffer<RcBindingUpdate> binding_upload;
  DeviceBuffer<RcNetUpdate> view_upload;
  bool buffers_swapped{false}, uploaded{false};
  Index old_net_count{0};
  void clear() noexcept
  {
    nets.clear();
    bindings.clear();
    views.clear();
    undo_views.clear();
    buffers_swapped = uploaded = false;
  }
};
struct GraphBuffers
{
  DeviceBuffer<Index> out_offsets, in_offsets, roots, ends;
  DeviceBuffer<NodeSeed> node_seeds;
  DeviceBuffer<PathSeed> path_seeds;
  DeviceBuffer<Index> out_arcs, in_arcs;
  DeviceBuffer<AuxRow> aux_rows;
  std::vector<AuxStorageRow> aux_storage;
  DeviceBuffer<Arc> arcs;
  DeviceBuffer<Endpoint> endpoints;
  std::vector<RcNetBuffers> rc_nets;
  std::vector<RcNet> rc_upload;
  DeviceBuffer<RcNet> rc_views;
  DeviceBuffer<RcBinding> rc_bindings;
  DeviceBuffer<Index> rc_net_by_node;
};
struct ClockWorkItem { Index node, owner, level; };
struct DeviceStorage
{
  explicit DeviceStorage(CsrCudaTelemetry& telemetry) : memory(telemetry) {}
  Runtime memory;
  unsigned kernel_threads{256};  // Test friend may vary launch shape; no runtime mode.
  ModelStorage models;
  Index uploaded_children{}, uploaded_tables{}, uploaded_rc_load_cells{};
  GraphBuffers graph;
  HostGraphUndo host_undo;
  PackedPatch patch_uploads;
  RcPatchStorage rc_patch;
  DeviceBuffer<Index> indegrees, frontier_offsets, frontier_candidates, wake_keys;
  DeviceBuffer<int> frontier_count;
  DeviceBuffer<std::byte> algorithm_scratch;
  std::vector<ClockWorkItem> clock_work;
  DeviceBuffer<ClockWorkItem> clock_work_device;
  DeviceBuffer<double> end_arrivals, default_required;
  std::vector<AuxBinding> aux_bindings;
  DeviceBuffer<AuxBinding> aux_binding_upload;
  DeviceBuffer<Child> children;
  DeviceBuffer<RcLoadCell> rc_load_cells;
  DeviceBuffer<double> tables;
  DeviceBuffer<BufferCandidateTiming> candidate_timing;
  ResultBuffers results[2];
  PathDomainBuffers domains[2];
  DeviceBuffer<SlotPatch> removes, changes, reverse_changes;
  DeviceBuffer<PayloadPatch> payloads;
  DeviceBuffer<SeedLoadPatch> seed_loads;
  DeviceBuffer<SeedLoadUndo> seed_undo;
  DeviceBuffer<DeactivatePatch> deactivations;
  DeviceBuffer<AuxPatch> aux_patches;
  DeviceBuffer<Index> aux_values;
  DeviceBuffer<SlotUndo> slot_undo;
  DeviceBuffer<ArcUndo> arc_undo;
  DeviceBuffer<NewNodePlacement> placements;
  Index slot_undo_count{}, arc_undo_count{}, seed_undo_count{};
  bool pending{false}, undo_captured{false}, poisoned{false}, closed{false};
};
void launchAuxBindings(DeviceView view, const AuxBinding* bindings, Index count, Index capacity, cudaStream_t stream, unsigned threads);
void launchCaptureUndo(DeviceView view, SlotUndo* slots, Index slot_count, ArcUndo* arcs, Index arc_count, NodeSeed* seeds, SeedLoadUndo* loads, Index load_count, cudaStream_t stream, unsigned threads);
void launchRestoreUndo(DeviceView view, const SlotUndo* slots, Index slot_count, const ArcUndo* arcs, Index arc_count, NodeSeed* seeds, const SeedLoadUndo* loads, Index load_count, cudaStream_t stream, unsigned threads);
std::uint64_t launchTargetedPatch(DeviceView view, DevicePatch patch, cudaStream_t stream, unsigned threads);
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
  std::optional<std::vector<BufferCandidateTiming>> candidate_timing;

  void initializeFull(cuda_detail::HostData& packed, const InputPortRcThresholds& thresholds);
  Impl(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water);
  Impl(std::shared_ptr<Impl> before, const CudaDualCsrPatch& patch, CudaLevelizationMode mode);
  static void validatePatch(const Impl& before, const CudaDualCsrPatch& patch);
  static void validateRcPatch(const Impl& before, const CudaDualCsrPatch& patch);
  void prepareRcPatch(const Impl& before, const CudaDualCsrPatch& patch, cuda_detail::PackedPatch& packed);
  void transferRcPatch(const Impl& before);
  void restoreRcPatch(CsrCudaTelemetry& cleanup);
  void clearRcPatch() noexcept;
  cuda_detail::PackedPatch& prepare(const Impl& before, const CudaDualCsrPatch& patch);
  ~Impl();
  void allocate();
  void bindGraph() noexcept;
  void transferRc(cuda_detail::HostData& packed);
  std::uint64_t bindAuxRows();
  std::vector<std::vector<cuda_detail::Index>> readAuxRows();
  void cancel() noexcept;
  CsrCudaTelemetry restorePending() noexcept;
  void levelizeFull();
  void propagate();
  void execute(CudaLevelizationMode mode = CudaLevelizationMode::kFull);
  const CsrTimingValues& materialize();
  const std::vector<BufferCandidateTiming>& readCandidateTiming();
  std::optional<CsrFifoLevelization> reconstructMixed();
  void transferPatch(const Impl& before, const CudaDualCsrPatch& patch, cuda_detail::PackedPatch& packed);
};
}  // namespace ista::csr
