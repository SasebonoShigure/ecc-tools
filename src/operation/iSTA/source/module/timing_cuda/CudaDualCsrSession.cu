#include <chrono>

#include "CsrCudaKernels.cuh"
#include "CsrReferenceValidator.hpp"
#include "CudaDualCsrDebugState.hpp"
#include "CudaDualCsrInternal.cuh"

namespace ista::csr {
using namespace cuda_detail;
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void describe(CsrCudaTelemetry& value)
{
  int device = 0;
  check(cudaGetDevice(&device), "cudaGetDevice");
  cudaDeviceProp properties{};
  check(cudaGetDeviceProperties(&properties, device), "cudaGetDeviceProperties");
  value.device_name = properties.name;
  check(cudaDriverGetVersion(&value.driver_version), "cudaDriverGetVersion");
  check(cudaRuntimeGetVersion(&value.runtime_version), "cudaRuntimeGetVersion");
}
}  // namespace
CudaDualCsrSession::Impl::Impl(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water, std::shared_ptr<Impl> before)
    : storage(before ? before->storage : std::make_shared<DeviceStorage>(telemetry.cuda)), memory(storage->memory),
      result_index(before ? 1 - before->result_index : 0), domain_index(before ? 1 - before->domain_index : 0), token(input.source_identity.session_token),
      generation(before ? before->generation.value() + 1 : 0), high_water(water), runtime_owner(!before), origin(std::move(before))
{
  const auto start = Clock::now();
  describe(telemetry.cuda);
  auto packed = pack(input, storage->models);
  host = std::make_shared<HostGraphState>();
  telemetry.cuda.encode_ms = elapsed(start);
  memory.bindTelemetry(telemetry.cuda);
  host->layout = std::move(packed.layout);
  auto& paths = storage->domains[domain_index].host;
  paths = std::move(packed.paths);
  host->bindings = std::move(packed.bindings);
  host->adjacency = std::move(packed.adjacency);
  host->path_seeds = std::move(packed.path_seeds);
  host->node_seeds = std::move(packed.node_seeds);
  host->out_offsets = std::move(packed.out_offsets);
  host->in_offsets = std::move(packed.in_offsets);
  host->roots = std::move(packed.roots);
  host->ends = std::move(packed.ends);
  view.node_count = packed.node_count;
  view.arc_count = packed.arcs.size();
  view.child_count = storage->models.children.size();
  view.child_state_count = packed.child_state_count;
  view.tag_count = packed.tag_count;
  view.path_count = paths.tags.size();
  view.clock_count = packed.clock_count;
  view.root_count = host->roots.size();
  view.end_count = host->ends.size();
  view.endpoint_count = packed.endpoints.size();
  table_count = storage->models.tables.size();
  if (origin) {
    std::swap(storage->graph, storage->full_work);
    storage->full_graph_pending = true;
    for (auto& row : storage->graph.aux_storage)
      row.count = 0;
  }
  allocate();
  try {
    memory.record(0);
    memory.upload(host->out_offsets, view.out_offsets);
    memory.upload(packed.out_arcs, view.out_arcs);
    memory.upload(host->in_offsets, view.in_offsets);
    memory.upload(packed.in_arcs, view.in_arcs);
    memory.upload(host->roots, view.roots);
    memory.upload(host->ends, view.ends);
    memory.upload(packed.endpoints, view.endpoints);
    memory.upload(packed.arcs, view.arcs);
    const auto child_begin = storage->uploaded_children, table_begin = storage->uploaded_tables;
    if (view.child_count > child_begin)
      memory.upload(storage->models.children.data() + child_begin, const_cast<Child*>(view.children) + child_begin, view.child_count - child_begin);
    if (table_count > table_begin)
      memory.upload(storage->models.tables.data() + table_begin, const_cast<double*>(view.table_data) + table_begin, table_count - table_begin);
    memory.upload(host->node_seeds, seed_nodes);
    memory.upload(host->path_seeds, seed_paths);
    memory.upload(paths.offsets, view.path_offsets);
    memory.upload(paths.tags, view.path_tags);
    memory.record(1);
    execute();
    storage->uploaded_children = view.child_count;
    storage->uploaded_tables = table_count;
    if (code && origin)
      cancel();
  } catch (...) {
    try {
      memory.synchronize();
    } catch (...) {
      storage->poisoned = true;
    }
    throw;
  }
}
CudaDualCsrSession::Impl::Impl(std::shared_ptr<Impl> before, const CudaDualCsrPatch& patch, CudaLevelizationMode mode)
    : storage(before->storage), memory(storage->memory), result_index(1 - before->result_index), domain_index(before->domain_index),
      token(before->token), generation(patch.target_generation), high_water(patch.after), origin(std::move(before))
{
  const auto& prior = *origin;
  host = prior.host;
  const auto start = Clock::now();
  telemetry.cuda.device_name = prior.telemetry.cuda.device_name;
  telemetry.cuda.driver_version = prior.telemetry.cuda.driver_version;
  telemetry.cuda.runtime_version = prior.telemetry.cuda.runtime_version;
  auto packed = prepare(prior, patch);
  memory.bindTelemetry(telemetry.cuda);
  telemetry.cuda.encode_ms = elapsed(start);
  allocate();
  origin->bindGraph();
  transferPatch(prior, patch, packed);
  execute(mode);
  if (code)
    cancel();
}
CudaDualCsrSession::Impl::~Impl()
{
  cancel();
}
CsrCudaTelemetry CudaDualCsrSession::Impl::restorePending() noexcept
{
  CsrCudaTelemetry cleanup;
  storage->host_undo.restore();
  if (storage->closed) {
    storage->host_undo.clear();
    storage->pending = false;
    storage->undo_captured = false;
    storage->slot_undo_count = storage->arc_undo_count = 0;
    storage->aux_bindings.clear();
    poisoned = true;
    return cleanup;
  }
  memory.bindTelemetry(cleanup);
  if (storage->full_graph_pending) {
    std::swap(storage->graph, storage->full_work);
    storage->full_graph_pending = false;
  }
  bindGraph();
  try {
    if (storage->host_undo.roots)
      memory.upload(host->roots, view.roots);
    if (storage->host_undo.ends)
      memory.upload(host->ends, view.ends);
    bindAuxRows();
    if (storage->undo_captured) {
      launchRestoreUndo(view, storage->slot_undo.data, storage->slot_undo_count,
                        storage->arc_undo.data, storage->arc_undo_count, memory.stream());
      cleanup.kernel_launches += storage->slot_undo_count != 0 || storage->arc_undo_count != 0;
    }
    memory.synchronize();
  } catch (...) {
    poisoned = true;
    storage->poisoned = true;
    // bindAuxRows may already have enqueued reads from aux_bindings.
    try {
      memory.synchronize();
    } catch (...) {
    }
  }
  storage->pending = false;
  storage->undo_captured = false;
  storage->slot_undo_count = storage->arc_undo_count = 0;
  storage->aux_bindings.clear();
  storage->host_undo.clear();
  memory.bindTelemetry(telemetry.cuda);
  return cleanup;
}
void CudaDualCsrSession::Impl::cancel() noexcept
{
  if (origin) {
    const auto cleanup = origin->restorePending();
    telemetry.cuda.kernel_launches += cleanup.kernel_launches;
    telemetry.cuda.h2d_bytes += cleanup.h2d_bytes;
    telemetry.cuda.d2d_bytes += cleanup.d2d_bytes;
    origin.reset();
  }
}
void CudaDualCsrSession::Impl::execute(CudaLevelizationMode mode)
{
  values.reset();
  levels.complete = false;
  telemetry.cuda.kernel_launches += launchSeedInitialization(
      view, seed_nodes, host->node_seeds.size(), seed_paths, host->path_seeds.size(), memory.stream());
  check(cudaGetLastError(), "CUDA path initialization launch");
  memory.zero(view.arc_state, view.arc_count);
  memory.zero(view.child_state, view.child_state_count);
  memory.record(2);
  std::optional<CsrFifoLevelization> oracle;
  if (mode == CudaLevelizationMode::kMixed) {
    oracle = reconstructMixed();
    if (code) {
      memory.synchronize();
      telemetry.cuda.h2d_ms = memory.milliseconds(0, 1);
      telemetry.update_ms = memory.milliseconds(1, 2);
      return;
    }
  } else {
    launchLevelization(view, memory.stream());
    ++telemetry.cuda.kernel_launches;
    check(cudaGetLastError(), "CUDA targeted full levelization launch");
  }
  memory.record(3);
  telemetry.cuda.kernel_launches += launchPropagation(view, memory.stream());
  check(cudaGetLastError(), "CUDA targeted full propagation launch");
  memory.record(4);
  Status status;
  Index level_count;
  try {
    memory.download(view.status, &status, 1);
    memory.download(view.level_offset_count, &level_count, 1);
    memory.synchronize();
  } catch (...) {
    try {
      memory.synchronize();
    } catch (...) {
      storage->poisoned = true;
    }
    throw;
  }
  if (status == Status::success) {
    if (!level_count || level_count > view.node_count + 1)
      throw std::runtime_error("CUDA targeted returned invalid level count");
    levels.node_to_level.resize(view.node_count);
    levels.level_offsets.resize(level_count);
    std::vector<Index> order;
    if constexpr (incregpusta_debug_checks_enabled) {
      order.resize(view.node_count);
      levels.level_nodes.resize(view.node_count);
    }
    try {
      if constexpr (incregpusta_debug_checks_enabled)
        memory.download(view.order, order);
      memory.download(view.node_levels, levels.node_to_level);
      memory.download(view.level_offsets, levels.level_offsets);
      memory.record(5);
      memory.synchronize();
    } catch (...) {
      try {
        memory.synchronize();
      } catch (...) {
        storage->poisoned = true;
      }
      throw;
    }
    if constexpr (incregpusta_debug_checks_enabled) {
      for (std::size_t i = 0; i < order.size(); ++i)
        levels.level_nodes[i] = NodeId{order[i]};
      if (oracle && (levels.level_offsets != oracle->level_offsets || levels.level_nodes != oracle->level_nodes
                     || levels.node_to_level != oracle->node_to_level))
        throw std::runtime_error("GPU mixed reconstruction differs from CPU reconstruction oracle");
      if (oracle)
        for (const auto& arc : host->bindings)
          if (arc.active && arc.levelization && levels.node_to_level[arc.source] >= levels.node_to_level[arc.sink])
            throw std::runtime_error("GPU mixed levels violate actual edge precedence");
    }
    levels.complete = true;
  } else {
    memory.record(5);
    memory.synchronize();
    switch (status) {
      case Status::cycle:
        code = CsrComputationCode::kCycle;
        break;
      case Status::numeric:
        code = CsrComputationCode::kNumericFailure;
        break;
      case Status::invariant:
        code = CsrComputationCode::kInvariantViolation;
        break;
      case Status::success:
        break;
    }
  }
  telemetry.cuda.h2d_ms = memory.milliseconds(0, 1);
  telemetry.update_ms = memory.milliseconds(1, 2);
  telemetry.cuda.levelization_ms = memory.milliseconds(mode == CudaLevelizationMode::kMixed ? 7 : 2, 3);
  if (mode == CudaLevelizationMode::kMixed)
    telemetry.mixed.reconstruct_ms = telemetry.cuda.levelization_ms;
  telemetry.cuda.propagation_ms = memory.milliseconds(3, 4);
  telemetry.cuda.d2h_ms = memory.milliseconds(4, 5);
}
const CsrTimingValues& CudaDualCsrSession::Impl::materialize()
{
  if (values)
    return *values;
  const auto start = Clock::now();
  memory.bindTelemetry(telemetry.cuda);
  DownloadData data;
  std::vector<Index> order;
  if constexpr (!incregpusta_debug_checks_enabled)
    order.resize(view.node_count);
  data.nodes.resize(view.node_count);
  data.paths.resize(view.path_count);
  data.arc_state.resize(view.arc_count);
  data.child_state.resize(view.child_state_count);
  try {
    memory.record(6);
    memory.download(view.nodes, data.nodes);
    memory.download(view.paths, data.paths);
    memory.download(view.arc_state, data.arc_state);
    memory.download(view.child_state, data.child_state);
    if constexpr (!incregpusta_debug_checks_enabled)
      memory.download(view.order, order);
    memory.record(7);
    memory.synchronize();
    telemetry.cuda.d2h_ms += memory.milliseconds(6, 7);
    // Publish the cache only after download and all allocating unpack work succeed.
    auto result = unpackValues(std::move(data), host->layout, storage->domains[domain_index].host);
    if constexpr (!incregpusta_debug_checks_enabled) {
      result.level_nodes.reserve(order.size());
      for (const auto node : order)
        result.level_nodes.emplace_back(node);
    }
    values.emplace(std::move(result));
    // Keep one owner for functional levels. The next mixed batch borrows them
    // from this cache after materialization, or from levels before any query.
    values->level_offsets = std::move(levels.level_offsets);
    if constexpr (incregpusta_debug_checks_enabled)
      values->level_nodes = std::move(levels.level_nodes);
    values->node_to_level = std::move(levels.node_to_level);
    telemetry.materialize_ms += elapsed(start);
    return *values;
  } catch (const CudaError&) {
    try {
      memory.synchronize();
    } catch (...) {
      storage->poisoned = true;
    }
    poisoned = true;
    storage->poisoned = true;
    if (origin)
      origin->poisoned = true;
    throw;
  }
}
CudaDualCsrSession::CudaDualCsrSession(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water)
{
  const auto start = Clock::now();
  demand(validateReferenceInput(input).empty(), "CUDA targeted baseline malformed");
  *this = fromNativeExport(input, water);
  _impl->telemetry.cuda.end_to_end_ms = elapsed(start);
}
CudaDualCsrSession CudaDualCsrSession::fromNativeExport(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water)
{
  const auto start = Clock::now();
  demand(input.unsupported_reasons.empty(), "CUDA targeted baseline unsupported");
  demand(input.source_identity.session_token != 0 && water.node_slots == input.nodes.size() && water.arc_slots == input.arcs.size(),
         "CUDA targeted baseline binding mismatch");
  CsrCudaBackend::requireDevice();
  CudaDualCsrSession result(std::make_shared<Impl>(input, water));
  result._impl->telemetry.cuda.end_to_end_ms = elapsed(start);
  return result;
}
CudaDualCsrSession::CudaDualCsrSession(std::shared_ptr<Impl> impl) noexcept : _impl(std::move(impl))
{
}
CudaDualCsrSession::~CudaDualCsrSession() = default;
CudaDualCsrSession::CudaDualCsrSession(CudaDualCsrSession&&) noexcept = default;
CudaDualCsrSession& CudaDualCsrSession::operator=(CudaDualCsrSession&&) noexcept = default;
CudaDualCsrSession CudaDualCsrSession::stage(const CudaDualCsrPatch& patch, CudaLevelizationMode mode)
{
  const auto start = Clock::now();
  demand(bool(_impl), "CUDA targeted owner was consumed");
  demand(mode == CudaLevelizationMode::kFull || mode == CudaLevelizationMode::kMixed, "unknown CUDA levelization mode");
  Impl::validatePatch(*_impl, patch);
  _impl->storage->pending = true;
  try {
    CudaDualCsrSession candidate(std::make_shared<Impl>(_impl, patch, mode));
    candidate._impl->telemetry.cuda.end_to_end_ms = elapsed(start);
    return candidate;
  } catch (const CudaAllocationError&) {
    _impl->restorePending();
    throw;
  } catch (const CudaError&) {
    _impl->poisoned = true;
    _impl->storage->poisoned = true;
    _impl->restorePending();
    throw;
  } catch (...) {
    _impl->restorePending();
    throw;
  }
}
CudaDualCsrSession CudaDualCsrSession::stageFull(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water)
{
  const auto start = Clock::now();
  demand(validateReferenceInput(input).empty(), "CUDA full rebuild malformed input");
  auto candidate = stageFullNativeExport(input, water);
  candidate._impl->telemetry.cuda.end_to_end_ms = elapsed(start);
  return candidate;
}
CudaDualCsrSession CudaDualCsrSession::stageFullNativeExport(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water)
{
  const auto start = Clock::now();
  requireCommitted();
  demand(_impl->generation.value() != absent && input.source_identity.session_token == _impl->token.value(),
         "CUDA full rebuild session/generation mismatch");
  demand(water.node_slots == input.nodes.size() && water.arc_slots == input.arcs.size()
             && input.unsupported_reasons.empty(),
         "CUDA full rebuild malformed or unsupported input");
  _impl->storage->pending = true;
  try {
    CudaDualCsrSession candidate(std::make_shared<Impl>(input, water, _impl));
    candidate._impl->telemetry.cuda.end_to_end_ms = elapsed(start);
    return candidate;
  } catch (const CudaAllocationError&) {
    _impl->restorePending();
    throw;
  } catch (const CudaError&) {
    _impl->poisoned = true;
    _impl->storage->poisoned = true;
    _impl->restorePending();
    throw;
  } catch (...) {
    _impl->restorePending();
    throw;
  }
}
bool CudaDualCsrSession::publish(CudaDualCsrSession&& candidate) noexcept
{
  if (!_impl || !candidate._impl || _impl->poisoned || _impl->storage->poisoned || !_impl->storage->pending || _impl->origin || candidate._impl->poisoned || candidate._impl->origin.get() != _impl.get()
      || candidate._impl->token != _impl->token || _impl->generation.value() == absent || candidate._impl->generation.value() != _impl->generation.value() + 1
      || candidate._impl->code || !candidate._impl->levels.complete)
    return false;
  for (const auto& binding : _impl->storage->aux_bindings)
    _impl->storage->graph.aux_storage[binding.source].count = binding.replacement_count;
  _impl->storage->aux_bindings.clear();
  _impl->storage->host_undo.clear();
  _impl->storage->pending = false;
  _impl->storage->full_graph_pending = false;
  _impl->storage->undo_captured = false;
  _impl->storage->slot_undo_count = _impl->storage->arc_undo_count = 0;
  _impl.swap(candidate._impl);
  _impl->runtime_owner = true;
  _impl->origin.reset();
  candidate._impl.reset();
  _impl->memory.bindTelemetry(_impl->telemetry.cuda);
  return true;
}
bool CudaDualCsrSession::close() noexcept
{
  if (!_impl)
    return true;
  if (!_impl->runtime_owner) {
    _impl->cancel();
    const bool clean = !_impl->storage->poisoned;
    _impl.reset();
    return clean;
  }
  auto& storage = *_impl->storage;
  const bool pending = storage.pending;
  const auto status = _impl->memory.close();
  storage.closed = true;
  storage.poisoned = true;
  _impl.reset();
  return status == cudaSuccess && !pending;
}

void CudaDualCsrSession::requireCommitted() const
{
  if (!_impl || _impl->origin || _impl->poisoned || _impl->storage->poisoned
      || _impl->storage->pending || _impl->code || !_impl->levels.complete)
    throw std::logic_error("CUDA committed timing unavailable");
}
const CsrTimingValues& CudaDualCsrSession::timingValues() const
{
  if (!_impl || _impl->poisoned || _impl->storage->poisoned || (_impl->storage->pending && !_impl->origin) || _impl->code || !_impl->levels.complete)
    throw std::logic_error("CUDA targeted timing unavailable");
  return _impl->materialize();
}
std::optional<CsrComputationCode> CudaDualCsrSession::computationCode() const noexcept
{
  return _impl ? _impl->code : std::optional{CsrComputationCode::kInvariantViolation};
}
const CudaDualCsrTelemetry& CudaDualCsrSession::telemetry() const
{
  if (!_impl)
    throw std::logic_error("CUDA targeted owner consumed");
  return _impl->telemetry;
}
timing_edit::TransactionGeneration CudaDualCsrSession::generation() const noexcept
{
  return _impl ? _impl->generation : timing_edit::TransactionGeneration{};
}
bool CudaDualCsrSession::poisoned() const noexcept
{
  return !_impl || _impl->poisoned || _impl->storage->poisoned;
}
std::vector<std::vector<Index>> CudaDualCsrSession::Impl::readAuxRows()
{
  std::vector<AuxRow> rows(view.node_count);
  std::vector<std::vector<Index>> result(view.node_count);
  try {
    memory.download(view.aux_rows, rows);
    memory.synchronize();
    for (Index i = 0; i < view.node_count; ++i) {
      demand((!rows[i].count || rows[i].arcs == storage->graph.aux_storage[i].buffer.data) && rows[i].count <= storage->graph.aux_storage[i].buffer.capacity,
             "CUDA diagnostic Aux row does not belong to its storage");
      result[i].resize(rows[i].count);
    }
    for (Index i = 0; i < view.node_count; ++i)
      memory.download(rows[i].arcs, result[i]);
    memory.synchronize();
    return result;
  } catch (const CudaError&) {
    storage->poisoned = true;
    try {
      memory.synchronize();
    } catch (...) {
    }
    throw;
  } catch (...) {
    try {
      memory.synchronize();
    } catch (...) {
      storage->poisoned = true;
    }
    throw;
  }
}

CudaDualCsrDebugState CudaDualCsrSession::debugState() const
{
  if constexpr (!incregpusta_debug_checks_enabled)
    throw std::logic_error("CUDA device diagnostic requires checks ON");
  if (!_impl || _impl->poisoned || _impl->storage->poisoned || (_impl->storage->pending && !_impl->origin) || _impl->code)
    throw std::logic_error("CUDA targeted diagnostic owner unavailable");
  CudaDualCsrDebugState result;
  auto& state = *_impl;
  state.memory.bindTelemetry(state.telemetry.cuda);
  result.out_offsets.resize(state.host->out_offsets.size());
  result.out_arcs.resize(state.host->out_offsets.back());
  result.in_offsets.resize(state.host->in_offsets.size());
  result.in_arcs.resize(state.host->in_offsets.back());
  result.aux_rows = state.readAuxRows();
  result.arcs.resize(state.view.arc_count);
  result.children.resize(state.view.child_count);
  result.table_data.resize(state.table_count);
  try {
    auto read = [&]<class T>(const T* source, Index count, std::vector<T>& target) {
      demand(target.size() == count, "diagnostic download capacity mismatch");
      state.memory.download(source, target);
    };
    read(state.view.out_offsets, state.host->out_offsets.size(), result.out_offsets);
    read(state.view.out_arcs, state.host->out_offsets.back(), result.out_arcs);
    read(state.view.in_offsets, state.host->in_offsets.size(), result.in_offsets);
    read(state.view.in_arcs, state.host->in_offsets.back(), result.in_arcs);
    read(state.view.arcs, state.view.arc_count, result.arcs);
    read(state.view.children, state.view.child_count, result.children);
    read(state.view.table_data, state.table_count, result.table_data);
    state.memory.download(state.view.storage_visits, result.storage_visits.data(), 8);
    state.memory.synchronize();
    return result;
  } catch (...) {
    try {
      state.memory.synchronize();
    } catch (...) {
      state.storage->poisoned = true;
    }
    state.poisoned = true;
    state.storage->poisoned = true;
    if (state.origin)
      state.origin->poisoned = true;
    throw;
  }
}
}  // namespace ista::csr
