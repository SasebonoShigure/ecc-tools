#include <chrono>
#include <numeric>

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

std::optional<CsrComputationCode> failureCode(Status status)
{
  switch (status) {
    case Status::success: return std::nullopt;
    case Status::cycle: return CsrComputationCode::kCycle;
    case Status::numeric: return CsrComputationCode::kNumericFailure;
    case Status::invariant: return CsrComputationCode::kInvariantViolation;
  }
  return CsrComputationCode::kInvariantViolation;
}
}  // namespace
CudaDualCsrSession::Impl::Impl(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water)
    : storage(std::make_shared<DeviceStorage>(telemetry.cuda)), memory(storage->memory),
      token(input.source_identity.session_token), generation(0), high_water(water), runtime_owner(true)
{
  describeDevice(telemetry.cuda);
  const auto start = Clock::now();
  auto packed = pack(input, storage->models);
  telemetry.cuda.encode_ms = elapsed(start);
  initializeFull(packed, input.parasitics.input_port_thresholds);
}
void CudaDualCsrSession::Impl::initializeFull(HostData& packed, const InputPortRcThresholds& thresholds)
{
  host = std::make_shared<HostGraphState>();
  host->layout = std::move(packed.layout);
  auto& paths = storage->domain.host;
  paths = std::move(packed.paths);
  host->bindings = std::move(packed.bindings);
  host->eligible_indegrees.assign(packed.node_count, 0);
  for (const auto& arc : host->bindings)
    if (arc.active && arc.levelization)
      host->eligible_indegrees[arc.sink] = checkedAdd(host->eligible_indegrees[arc.sink], 1);
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
  view.rc_net_count = packed.rc_nets.size();
  view.rc_port_derate = thresholds.slew_derate;
  for (unsigned output = 0; output < 2; ++output) {
    view.rc_port_lower[output] = thresholds.lower[output];
    view.rc_port_upper[output] = thresholds.upper[output];
  }
  view.root_count = host->roots.size();
  view.end_count = host->ends.size();
  view.endpoint_count = packed.endpoints.size();
  table_count = storage->models.tables.size();
  allocate();
  try {
    memory.record(0);
    transferRc(packed);
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
    memory.upload(paths.rows, view.path_rows);
    memory.upload(paths.tags, view.path_tags);
    memory.record(1);
    execute();
    storage->uploaded_children = view.child_count;
    storage->uploaded_tables = table_count;
    storage->uploaded_rc_load_cells = storage->models.rc_load_cells.size();
    for (std::size_t index = 0; index < packed.rc_nets.size(); ++index)
      storage->graph.rc_nets[index].owner = packed.rc_nets[index].owner;
    host->rc = std::move(packed.rc);
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
    : storage(before->storage), memory(storage->memory), result_index(1 - before->result_index),
      token(before->token), generation(patch.target_generation), high_water(patch.after), origin(std::move(before))
{
  const auto& prior = *origin;
  host = prior.host;
  const auto start = Clock::now();
  telemetry.cuda.device_name = prior.telemetry.cuda.device_name;
  telemetry.cuda.device_uuid = prior.telemetry.cuda.device_uuid;
  telemetry.cuda.driver_version = prior.telemetry.cuda.driver_version;
  telemetry.cuda.runtime_version = prior.telemetry.cuda.runtime_version;
  auto& packed = prepare(prior, patch);
  prepareRcPatch(prior, patch, packed);
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
  storage->domain.restoreHost();
  // A failed candidate constructor also reaches this surviving origin owner.
  if (node_groups && node_groups->size() > view.node_count)
    node_groups->resize(view.node_count);
  if (storage->closed) {
    storage->host_undo.clear();
    storage->domain.clear();
    clearRcPatch();
    storage->pending = false;
    storage->undo_captured = false;
    storage->slot_undo_count = storage->arc_undo_count = 0;
    storage->aux_bindings.clear();
    poisoned = true;
    return cleanup;
  }
  memory.bindTelemetry(cleanup);
  bindGraph();
  try {
    restorePathRows(cleanup);
    restoreRcPatch(cleanup);
    if (storage->host_undo.roots)
      memory.upload(host->roots, view.roots);
    if (storage->host_undo.ends)
      memory.upload(host->ends, view.ends);
    cleanup.kernel_launches += bindAuxRows();
    if (storage->undo_captured) {
      launchRestoreUndo(view, storage->slot_undo.data, storage->slot_undo_count,
                        storage->arc_undo.data, storage->arc_undo_count, seed_nodes, storage->seed_undo.data, storage->seed_undo_count, memory.stream(), storage->kernel_threads);
      cleanup.kernel_launches += storage->slot_undo_count != 0 || storage->arc_undo_count != 0 || storage->seed_undo_count != 0;
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
  storage->domain.clear();
  clearRcPatch();
  // Charge undo waits to the surviving owner, including constructor failures.
  telemetry.cuda.stream_wait_ms += cleanup.stream_wait_ms;
  telemetry.cuda.stream_waits += cleanup.stream_waits;
  memory.bindTelemetry(telemetry.cuda);
  return cleanup;
}
void CudaDualCsrSession::Impl::cancel() noexcept
{
  if (origin) {
    const auto cleanup = origin->restorePending();
    telemetry.cuda.kernel_launches += cleanup.kernel_launches;
    telemetry.cuda.device_algorithm_calls += cleanup.device_algorithm_calls;
    telemetry.cuda.h2d_bytes += cleanup.h2d_bytes;
    telemetry.cuda.d2d_bytes += cleanup.d2d_bytes;
    origin.reset();
  }
}
void CudaDualCsrSession::Impl::execute(CudaLevelizationMode mode) try
{
  values.reset();
  levels.complete = false;
  telemetry.cuda.kernel_launches += launchSeedInitialization(
      view, seed_nodes, host->node_seeds.size(), seed_paths, host->path_seeds.size(), memory.stream(), storage->kernel_threads);
  check(cudaGetLastError(), "CUDA path initialization launch");
  memory.zero(view.arc_state, view.arc_count);
  memory.zero(view.child_state, view.child_state_count);
  memory.record(2);
  std::optional<CsrFifoLevelization> oracle;
  if (mode == CudaLevelizationMode::kMixed) {
    oracle = reconstructMixed();
  } else {
    const auto prior_readback = telemetry.cuda.d2h_bytes;
    const auto prior_wait = telemetry.cuda.stream_wait_ms;
    levelizeFull();
    telemetry.cuda.levelization_decision_bytes = telemetry.cuda.d2h_bytes - prior_readback;
    telemetry.cuda.levelization_decision_wait_ms = telemetry.cuda.stream_wait_ms - prior_wait;
  }
  Status status;
  try {
    const auto prior_readback = telemetry.cuda.d2h_bytes;
    const auto prior_wait = telemetry.cuda.stream_wait_ms;
    memory.download(view.status, &status, 1);
    memory.synchronize();
    telemetry.cuda.status_d2h_bytes += telemetry.cuda.d2h_bytes - prior_readback;
    telemetry.cuda.status_wait_ms += telemetry.cuda.stream_wait_ms - prior_wait;
  } catch (...) {
    try { memory.synchronize(); } catch (...) { storage->poisoned = true; }
    throw;
  }
  // Device patch failure takes precedence over a CPU mixed conflict, including
  // the CPU failure exits that did not enqueue reconstruction.
  if (status != Status::success) {
    code = failureCode(status);
    telemetry.mixed_failure.reset();
  }
  if (code) {
    telemetry.cuda.h2d_ms = memory.milliseconds(0, 1);
    telemetry.update_ms = memory.milliseconds(1, 2);
    return;
  }
  if (mode == CudaLevelizationMode::kFull) {
    node_groups = std::make_shared<std::vector<Index>>(view.node_count);
    group_levels.resize(levels.level_offsets.size());
    std::iota(group_levels.begin(), group_levels.end(), Index{0});
  }
  if (mode == CudaLevelizationMode::kFull || incregpusta_debug_checks_enabled) {
    std::vector<Index> order, inverse;
    if constexpr (incregpusta_debug_checks_enabled) {
      order.resize(view.node_count);
      levels.level_nodes.resize(view.node_count);
      if (mode == CudaLevelizationMode::kMixed)
        inverse.resize(view.node_count);
    }
    try {
      const auto prior_readback = telemetry.cuda.d2h_bytes;
      const auto prior_wait = telemetry.cuda.stream_wait_ms;
      if (mode == CudaLevelizationMode::kFull)
        memory.download(view.node_levels, *node_groups);
      if constexpr (incregpusta_debug_checks_enabled) {
        memory.download(view.order, order);
        if (mode == CudaLevelizationMode::kMixed)
          memory.download(view.node_levels, inverse);
      }
      memory.synchronize();
      telemetry.cuda.level_metadata_d2h_bytes += telemetry.cuda.d2h_bytes - prior_readback;
      telemetry.cuda.level_metadata_wait_ms += telemetry.cuda.stream_wait_ms - prior_wait;
    } catch (...) {
      try { memory.synchronize(); } catch (...) { storage->poisoned = true; }
      throw;
    }
    if constexpr (incregpusta_debug_checks_enabled) {
      for (std::size_t i = 0; i < order.size(); ++i)
        levels.level_nodes[i] = NodeId{order[i]};
      if (oracle) {
        if (levels.level_offsets != oracle->level_offsets || levels.level_nodes != oracle->level_nodes
            || inverse != oracle->node_to_level)
          throw std::runtime_error("GPU mixed reconstruction differs from CPU reconstruction oracle");
        for (Index node = 0; node < view.node_count; ++node)
          if (levelOf(node) != inverse[node])
            throw std::runtime_error("stable layer membership differs from GPU reconstruction");
        std::vector<Index> indegrees(view.node_count, 0);
        for (const auto& arc : host->bindings)
          if (arc.active && arc.levelization) {
            if (inverse[arc.source] >= inverse[arc.sink])
              throw std::runtime_error("GPU mixed levels violate actual edge precedence");
            indegrees[arc.sink] = checkedAdd(indegrees[arc.sink], 1);
          }
        if (indegrees != host->eligible_indegrees)
          throw std::runtime_error("cached eligible indegrees differ from actual graph");
      }
    }
  }
  memory.record(3);
  propagate();
  memory.record(4);
  try {
    const auto prior_readback = telemetry.cuda.d2h_bytes;
    const auto prior_wait = telemetry.cuda.stream_wait_ms;
    memory.download(view.status, &status, 1);
    memory.record(5);
    memory.synchronize();
    telemetry.cuda.status_d2h_bytes += telemetry.cuda.d2h_bytes - prior_readback;
    telemetry.cuda.status_wait_ms += telemetry.cuda.stream_wait_ms - prior_wait;
  } catch (...) {
    try { memory.synchronize(); } catch (...) { storage->poisoned = true; }
    throw;
  }
  code = failureCode(status);
  levels.complete = !code;
  telemetry.cuda.h2d_ms = memory.milliseconds(0, 1);
  telemetry.update_ms = memory.milliseconds(1, 2);
  telemetry.cuda.levelization_ms = memory.milliseconds(mode == CudaLevelizationMode::kMixed ? 7 : 2, 3);
  if (mode == CudaLevelizationMode::kMixed)
    telemetry.mixed.reconstruct_ms = telemetry.cuda.levelization_ms;
  telemetry.cuda.propagation_ms = memory.milliseconds(3, 4);
  telemetry.cuda.delivery_d2h_ms = memory.milliseconds(4, 5);
} catch (...) {
  // Member-backed uploads must finish before a failed constructor destroys them.
  try { memory.synchronize(); } catch (...) { storage->poisoned = true; }
  throw;
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
  ReportTransfer transfer;
  if (report_values.nodes.empty()) {
    transfer.node_values.resize(view.node_count);
  } else {
    data.nodes.resize(view.node_count);
    transfer.nodes.reserve(view.node_count - report_values.nodes.size());
    for (Index node = 0; node < view.node_count; ++node) {
      const auto cached = report_values.nodes.find(node);
      if (cached != report_values.nodes.end()) data.nodes[node] = cached->second.timing;
      else transfer.nodes.push_back(node);
    }
    transfer.node_values.resize(transfer.nodes.size());
  }
  const auto& domain = storage->domain.host;
  if (report_values.paths.empty() && domain.live_tags == view.path_count) {
    transfer.path_values.resize(view.path_count);
  } else {
    data.paths.resize(view.path_count);
    transfer.paths.reserve(domain.live_tags - report_values.paths.size());
    for (const auto& segment : domain.segments)
      if (domain.rows[segment.row] == segment.range)
        for (Index slot = segment.range.begin; slot < segment.range.end; ++slot) {
          const auto cached = report_values.paths.find(slot);
          if (cached != report_values.paths.end()) data.paths[slot] = cached->second;
          else transfer.paths.push_back(slot);
        }
    transfer.path_values.resize(transfer.paths.size());
  }
  data.arc_state.resize(view.arc_count);
  data.child_state.resize(view.child_state_count);
  try {
    const auto prior_readback = telemetry.cuda.d2h_bytes;
    const auto prior_wait = telemetry.cuda.stream_wait_ms;
    enqueueReportRead(transfer);
    memory.download(view.arc_state, data.arc_state);
    memory.download(view.child_state, data.child_state);
    if constexpr (!incregpusta_debug_checks_enabled)
      memory.download(view.order, order);
    memory.record(7);
    memory.synchronize();
    telemetry.cuda.delivery_d2h_ms += memory.milliseconds(6, 7);
    telemetry.cuda.result_d2h_bytes += telemetry.cuda.d2h_bytes - prior_readback;
    telemetry.cuda.result_wait_ms += telemetry.cuda.stream_wait_ms - prior_wait;
    if (data.nodes.empty()) data.nodes = std::move(transfer.node_values);
    else
      for (std::size_t slot = 0; slot < transfer.nodes.size(); ++slot)
        data.nodes[transfer.nodes[slot]] = transfer.node_values[slot];
    if (data.paths.empty()) data.paths = std::move(transfer.path_values);
    else
      for (std::size_t slot = 0; slot < transfer.paths.size(); ++slot)
        data.paths[transfer.paths[slot]] = transfer.path_values[slot];
    // Publish the cache only after download and all allocating unpack work succeed.
    auto result = unpackValues(std::move(data), host->layout, storage->domain.host);
    if constexpr (!incregpusta_debug_checks_enabled) {
      result.level_nodes.reserve(order.size());
      for (const auto node : order)
        result.level_nodes.emplace_back(node);
    }
    result.level_offsets = levels.level_offsets;
    result.node_to_level.reserve(view.node_count);
    for (Index node = 0; node < view.node_count; ++node)
      result.node_to_level.push_back(levelOf(node));
    if constexpr (incregpusta_debug_checks_enabled)
      result.level_nodes = std::move(levels.level_nodes);
    values.emplace(std::move(result));
    report_values.nodes.clear();
    report_values.paths.clear();
    report_values.complete = &*values;
    telemetry.materialize_ms += elapsed(start);
    return *values;
  } catch (const CudaAllocationError&) {
    throw;
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
bool CudaDualCsrSession::canPublish(const CudaDualCsrSession& candidate) const noexcept
{
  if (!_impl || !candidate._impl || _impl->poisoned || _impl->storage->poisoned || !_impl->storage->pending || _impl->origin || candidate._impl->poisoned || candidate._impl->origin.get() != _impl.get()
      || candidate._impl->token != _impl->token || _impl->generation.value() == absent || candidate._impl->generation.value() != _impl->generation.value() + 1
      || candidate._impl->code || !candidate._impl->levels.complete)
    return false;
  return true;
}
bool CudaDualCsrSession::publish(CudaDualCsrSession&& candidate) noexcept
{
  if (!canPublish(candidate)) return false;
  for (const auto& binding : _impl->storage->aux_bindings)
    _impl->storage->graph.aux_storage[binding.source].count = binding.replacement_count;
  _impl->storage->aux_bindings.clear();
  _impl->storage->host_undo.clear();
  _impl->storage->domain.clear();
  candidate._impl->clearRcPatch();
  _impl->storage->pending = false;
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
    state.memory.download(state.view.storage_visits, result.storage_visits.data(), result.storage_visits.size());
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
