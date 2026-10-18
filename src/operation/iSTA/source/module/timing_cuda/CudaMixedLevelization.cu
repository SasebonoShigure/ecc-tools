#include <algorithm>
#include <chrono>

#include "CudaDualCsrInternal.cuh"
#include "CsrCudaKernels.cuh"
#include "MixedLevelReconstructor.hpp"
#include "MixedTopologyInternal.hpp"

namespace ista::csr {
using namespace cuda_detail;
namespace mixed = timing_incremental_levelization;
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct OldLevelCopy
{
  Index begin, count, destination;
};
__device__ void fail(DeviceView view)
{
  recordFailure(view.status, Status::invariant);
}
__global__ void insertNewLevelNodes(DeviceView view, const NewNodePlacement* nodes, Index count)
{
  if (readStatus(view.status) != Status::success)
    return;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < count; i += static_cast<Index>(gridDim.x) * blockDim.x) {
    const auto value = nodes[i];
    if (value.node >= view.node_count || value.position >= view.node_count) {
      fail(view);
      continue;
    }
    view.order[value.position] = value.node;
  }
}
__global__ void reconstructNodeToLevel(DeviceView view, Index offset_count)
{
  if (readStatus(view.status) != Status::success)
    return;
  if (!offset_count || offset_count > view.node_count + 1 || view.level_offsets[0] != 0 || view.level_offsets[offset_count - 1] != view.node_count) {
    fail(view);
    return;
  }
  for (Index position = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; position < view.node_count;
       position += static_cast<Index>(gridDim.x) * blockDim.x) {
    const Index node = view.order[position];
    Index low = 0, high = offset_count - 1;
    while (low + 1 < high) {
      const Index middle = low + (high - low) / 2;
      if (view.level_offsets[middle] <= position)
        low = middle;
      else
        high = middle;
    }
    if (node >= view.node_count || low + 1 >= offset_count || view.level_offsets[low] > position || view.level_offsets[low + 1] <= position) {
      fail(view);
      continue;
    }
    // Inverse construction also detects duplicate nodes before propagation can
    // consume an incomplete permutation. No independent replay is needed.
    if (atomicCAS(reinterpret_cast<unsigned long long*>(view.node_levels + node), 0ULL, static_cast<unsigned long long>(low + 1)) != 0ULL)
      fail(view);
  }
}
unsigned blocks(Index count)
{
  return static_cast<unsigned>(std::max<Index>(1, std::min<Index>(65535, count / 256 + (count % 256 != 0))));
}
}  // namespace

std::optional<CsrFifoLevelization> CudaDualCsrSession::Impl::reconstructMixed()
{
  demand(origin && origin->levels.complete, "mixed reconstruction requires a successful origin");
  const auto& cached = origin->values;
  const mixed::detail::PreviousLevels previous(
      origin->levels.level_offsets, cached ? cached->level_nodes : origin->levels.level_nodes,
      std::span<const Index>(*origin->node_groups).first(origin->view.node_count), origin->group_levels, true);
  const mixed::MixedLevelizationRequest request{origin->view.node_count, origin->generation.value(), generation.value()};
  const auto visitArc = [&](const ArcBinding& arc, const mixed::detail::TopologyVisitor& visitor) {
    if (!arc.active) return EdgeVisitResult::continueVisit();
    if (arc.source >= view.node_count || arc.sink >= view.node_count)
      return EdgeVisitResult::fail(CsrViewError::kNodeOutOfRange);
    return visitor({NodeId{arc.source}, NodeId{arc.sink}, arc.levelization, arc.storage_kind});
  };
  const mixed::detail::TopologyTraversal topology{static_cast<std::size_t>(view.node_count), [&](const mixed::detail::TopologyVisitor& visitor) {
    // Unchanged old edges are already ordered by the committed levels.
    for (const auto& [id, old] : storage->host_undo.bindings) {
      const auto& current = host->bindings[id];
      if (current.active == old.active && current.levelization == old.levelization) continue;
      const auto result = visitArc(current, visitor);
      if (result.control != EdgeVisitControl::kContinue) return result;
    }
    for (Index id = origin->view.arc_count; id < view.arc_count; ++id) {
      const auto result = visitArc(host->bindings[id], visitor);
      if (result.control != EdgeVisitControl::kContinue) return result;
    }
    return EdgeVisitResult::continueVisit();
  }};
  const auto build_start = Clock::now();
  auto built = mixed::detail::buildOwnedMixedGraph(topology, previous, request);
  const auto build_ms = elapsed(build_start);
  auto reject = [&](mixed::MixedStageFailureCode failure) {
    telemetry.mixed_failure = failure;
    code = failure == mixed::MixedStageFailureCode::kActualCycle ? CsrComputationCode::kCycle : CsrComputationCode::kInvariantViolation;
  };
  auto classify = [&] {
    const mixed::detail::TopologyTraversal actual_topology{static_cast<std::size_t>(view.node_count), [&](const mixed::detail::TopologyVisitor& visitor) {
      for (const auto& arc : host->bindings) {
        const auto result = visitArc(arc, visitor);
        if (result.control != EdgeVisitControl::kContinue) return result;
      }
      return EdgeVisitResult::continueVisit();
    }};
    const auto actual = mixed::detail::classifyActualDag(actual_topology);
    if constexpr (incregpusta_debug_checks_enabled) {
      ++telemetry.mixed.actual_cycle_classifier_runs;
      telemetry.mixed.actual_cycle_classifier_arc_visits += actual.eligible_arc_visits;
    }
    switch (actual.code) {
      case mixed::ActualDagClassificationCode::kDag:
        reject(mixed::MixedStageFailureCode::kOldLevelOrderConflict);
        break;
      case mixed::ActualDagClassificationCode::kCycle:
        reject(mixed::MixedStageFailureCode::kActualCycle);
        break;
      case mixed::ActualDagClassificationCode::kViewError:
        reject(mixed::MixedStageFailureCode::kViewError);
        break;
      case mixed::ActualDagClassificationCode::kInvariantFailure:
        reject(mixed::MixedStageFailureCode::kInvariantFailure);
        break;
    }
  };
  if (const auto* failure = std::get_if<mixed::MixedGraphBuildFailure>(&built)) {
    telemetry.mixed = failure->telemetry;
    telemetry.mixed.mixed_build_ms = build_ms;
    switch (failure->code) {
      case mixed::MixedGraphBuildFailureCode::kOldOldOrderConflict:
        classify();
        break;
      case mixed::MixedGraphBuildFailureCode::kViewError:
        reject(mixed::MixedStageFailureCode::kViewError);
        break;
      case mixed::MixedGraphBuildFailureCode::kMalformedPreviousLevels:
        reject(mixed::MixedStageFailureCode::kMalformedPreviousLevels);
        break;
      case mixed::MixedGraphBuildFailureCode::kMappingOverflow:
        reject(mixed::MixedStageFailureCode::kMappingOverflow);
        break;
      case mixed::MixedGraphBuildFailureCode::kInvariantFailure:
        reject(mixed::MixedStageFailureCode::kInvariantFailure);
        break;
    }
    return std::nullopt;
  }
  const auto& graph = std::get<mixed::MixedPrecedenceGraph>(built);
  telemetry.mixed = graph.telemetry;
  telemetry.mixed.mixed_build_ms = build_ms;
  const auto kahn_start = Clock::now();
  auto kahn = mixed::detail::levelizeBuiltMixedGraph(graph);
  telemetry.mixed.mixed_kahn_ms = elapsed(kahn_start);
  if (const auto* failure = std::get_if<mixed::MixedKahnFailure>(&kahn)) {
    if (failure->code == mixed::MixedKahnFailureCode::kMixedCycle)
      classify();
    else
      reject(mixed::MixedStageFailureCode::kInvariantFailure);
    return std::nullopt;
  }
  const auto& undo = storage->host_undo;
  bool roots_valid = true;
  if (undo.roots) {
    Index root_count = origin->view.root_count;
    for (const auto& [node, old] : undo.eligible_indegrees) {
      root_count -= old == 0;
      root_count = checkedAdd(root_count, host->eligible_indegrees[node] == 0);
    }
    for (Index node = origin->view.node_count; node < view.node_count; ++node)
      root_count = checkedAdd(root_count, host->eligible_indegrees[node] == 0);
    roots_valid = root_count == host->roots.size();
    for (Index root : host->roots)
      roots_valid = roots_valid && host->eligible_indegrees[root] == 0;
  } else {
    for (const auto& [node, old] : undo.eligible_indegrees)
      roots_valid = roots_valid && ((old == 0) == (host->eligible_indegrees[node] == 0));
    for (Index node = origin->view.node_count; node < view.node_count; ++node)
      roots_valid = roots_valid && host->eligible_indegrees[node] != 0;
  }
  if (!roots_valid) {
    reject(mixed::MixedStageFailureCode::kInvariantFailure);
    return std::nullopt;
  }
  const auto& mixed_levels = std::get<mixed::MixedKahnLevelization>(kahn);
  const auto prepare_start = Clock::now();
  auto& offsets = levels.level_offsets;
  offsets = {0};
  auto& new_nodes = storage->mixed_placements;
  new_nodes.clear();
  std::vector<Index> old_level_groups(origin->group_levels.size());
  for (Index group = 1; group < origin->group_levels.size(); ++group)
    old_level_groups[origin->group_levels[group]] = group;
  group_levels.assign(origin->group_levels.size(), 0);
  std::vector<Index> new_groups(graph.domain.n_new_nodes);
  std::vector<OldLevelCopy> copies;
  new_nodes.reserve(graph.domain.n_new_nodes);
  copies.reserve(graph.domain.n_old_levels);
  Index destination = 0;
  for (Index level = 0; level + 1 < mixed_levels.level_offsets.size(); ++level) {
    bool has_old = false;
    Index group = 0;
    const auto first_new = new_nodes.size();
    for (Index position = mixed_levels.level_offsets[level]; position < mixed_levels.level_offsets[level + 1]; ++position) {
      const auto vertex = mixed_levels.level_vertices[position];
      if (mixed::isVirtualOldLevel(vertex, graph.domain)) {
        demand(!has_old, "mixed frontier has multiple old levels");
        has_old = true;
        group = old_level_groups[vertex.value() + 1];
        const Index begin = previous.level_offsets[vertex.value()], end = previous.level_offsets[vertex.value() + 1];
        demand(begin < end && end <= origin->view.node_count, "old level D2D range invalid");
        copies.push_back({begin, end - begin, destination});
        destination = checkedAdd(destination, end - begin);
      } else {
        const auto decoded = mixed::newNodeForMixedVertex(vertex, graph.domain);
        const auto* node = std::get_if<mixed::NewActualNode>(&decoded);
        demand(node != nullptr, "mixed new-node mapping failed");
        new_nodes.push_back({node->value().value(), destination});
        destination = checkedAdd(destination, 1);
      }
      demand(destination <= view.node_count, "mixed reconstruction exceeds node capacity");
    }
    demand(destination > offsets.back(), "mixed reconstruction produced an empty level");
    offsets.push_back(destination);
    if (has_old)
      group_levels[group] = level + 1;
    else {
      group = group_levels.size();
      group_levels.push_back(level + 1);
    }
    for (auto i = first_new; i < new_nodes.size(); ++i)
      new_groups[new_nodes[i].node - origin->view.node_count] = group;
  }
  demand(destination == view.node_count && new_nodes.size() == graph.domain.n_new_nodes && copies.size() == graph.domain.n_old_levels,
         "mixed reconstruction does not cover current nodes");
  if constexpr (incregpusta_debug_checks_enabled) {
    telemetry.mixed.kahn_processed_count = mixed_levels.processed_count;
    telemetry.mixed.max_frontier = mixed_levels.max_frontier;
    telemetry.mixed.reconstructed_level_count = offsets.size() - 1;
  }
  std::optional<CsrFifoLevelization> oracle;
  if constexpr (incregpusta_debug_checks_enabled) {
    CsrFifoLevelization oracle_previous;
    oracle_previous.complete = true;
    oracle_previous.level_offsets = previous.level_offsets;
    oracle_previous.level_nodes = previous.level_nodes;
    oracle_previous.node_to_level.reserve(origin->view.node_count);
    for (Index node = 0; node < origin->view.node_count; ++node)
      oracle_previous.node_to_level.push_back(origin->levelOf(node));
    auto result = mixed::reconstructMixedLevelization(graph.domain, oracle_previous, mixed_levels);
    demand(std::holds_alternative<CsrFifoLevelization>(result), "CPU reconstruction oracle rejected mixed levels");
    oracle = std::get<CsrFifoLevelization>(std::move(result));
  }
  node_groups = origin->node_groups;
  node_groups->insert(node_groups->end(), new_groups.begin(), new_groups.end());
  telemetry.mixed_prepare_ms = elapsed(prepare_start);
  auto& result_buffers = storage->results[result_index];
  if (memory.ensure(result_buffers.level_offsets, offsets.size()))
    ++telemetry.grown_buffers;
  view.level_offsets = result_buffers.level_offsets.data;
  if constexpr (incregpusta_debug_checks_enabled) {
    if (memory.ensure(result_buffers.node_levels, view.node_count))
      ++telemetry.grown_buffers;
    view.node_levels = result_buffers.node_levels.data;
  }
  if (memory.ensure(storage->placements, new_nodes.size()))
    ++telemetry.grown_buffers;
  auto* device_new_nodes = storage->placements.data;
  try {
    memory.record(7);
    for (const auto& copy : copies)
      memory.copy(origin->view.order + copy.begin, view.order + copy.destination, copy.count);
    memory.upload(new_nodes, device_new_nodes);
    memory.upload(offsets, view.level_offsets);
    if constexpr (incregpusta_debug_checks_enabled)
      memory.zero(view.node_levels, view.node_count);
    if (!new_nodes.empty()) {
      insertNewLevelNodes<<<blocks(new_nodes.size()), 256, 0, memory.stream()>>>(view, device_new_nodes, new_nodes.size());
      ++telemetry.cuda.kernel_launches;
      check(cudaGetLastError(), "CUDA insert mixed new nodes launch");
    }
    if constexpr (incregpusta_debug_checks_enabled) {
      reconstructNodeToLevel<<<blocks(view.node_count), 256, 0, memory.stream()>>>(view, offsets.size());
      ++telemetry.cuda.kernel_launches;
      check(cudaGetLastError(), "CUDA mixed node2level launch");
    }
    // Uploads belong to the pending session/storage until execute's status wait.
  } catch (...) {
    try {
      memory.synchronize();
    } catch (...) {
      storage->poisoned = true;
    }
    throw;
  }
  return oracle;
}
}  // namespace ista::csr
