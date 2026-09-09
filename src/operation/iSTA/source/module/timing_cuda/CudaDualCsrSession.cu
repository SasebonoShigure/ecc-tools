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
CudaDualCsrSession::Impl::Impl(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water)
    : token(input.source_identity.session_token), generation(0), high_water(water)
{
  const auto start = Clock::now();
  describe(telemetry.cuda);
  auto host = pack(input);
  telemetry.cuda.encode_ms = elapsed(start);
  layout = std::move(host.layout);
  out_offsets = host.out_offsets;
  in_offsets = host.in_offsets;
  roots = host.roots;
  ends = host.ends;
  aux_lengths.resize(input.nodes.size(), 0);
  aux_mapping.resize(input.nodes.size(), absent);
  aux_offsets = {0};
  for (const auto& arc : host.arcs)
    bindings.push_back({arc.source, arc.sink, true});
  view.node_count = host.nodes.size();
  view.arc_count = host.arcs.size();
  view.child_count = host.children.size();
  view.tag_count = host.tag_count;
  view.root_count = roots.size();
  view.end_count = ends.size();
  view.endpoint_count = host.endpoints.size();
  table_count = host.table_data.size();
  allocate();
  memory.record(0);
  memory.upload(out_offsets, view.out_offsets);
  memory.upload(host.out_arcs, view.out_arcs);
  memory.upload(in_offsets, view.in_offsets);
  memory.upload(host.in_arcs, view.in_arcs);
  memory.upload(aux_offsets, view.aux_offsets);
  memory.upload(aux_mapping, view.source_to_aux_row);
  memory.upload(roots, view.roots);
  memory.upload(ends, view.ends);
  memory.upload(host.endpoints, view.endpoints);
  memory.upload(host.arcs, view.arcs);
  memory.upload(host.children, view.children);
  memory.upload(host.table_data, view.table_data);
  memory.upload(host.nodes, seed_nodes);
  memory.upload(host.paths, seed_paths);
  memory.record(1);
  execute();
}
CudaDualCsrSession::Impl::Impl(std::shared_ptr<const Impl> before, const CudaDualCsrPatch& patch)
    : token(before->token), generation(patch.target_generation), high_water(patch.after), origin(std::move(before))
{
  const auto& prior = *origin;
  const auto start = Clock::now();
  describe(telemetry.cuda);
  auto packed = prepare(prior, patch);
  telemetry.cuda.encode_ms = elapsed(start);
  allocate();
  transferPatch(prior, patch, packed);
  execute();
}
void CudaDualCsrSession::Impl::execute()
{
  memory.copy(seed_nodes, view.nodes, view.node_count);
  memory.copy(seed_paths, view.paths, checkedProduct(checkedProduct(view.node_count, 8), view.tag_count));
  memory.zero(view.arc_state, view.arc_count);
  memory.zero(view.child_state, view.child_count);
  memory.record(2);
  launchLevelization(view, memory.stream());
  ++telemetry.cuda.kernel_launches;
  check(cudaGetLastError(), "CUDA targeted full levelization launch");
  memory.record(3);
  telemetry.cuda.kernel_launches += launchPropagation(view, memory.stream());
  check(cudaGetLastError(), "CUDA targeted full propagation launch");
  memory.record(4);
  Status status;
  Index levels;
  memory.download(view.status, &status, 1);
  memory.download(view.level_offset_count, &levels, 1);
  memory.synchronize();
  if (status == Status::success) {
    if (!levels || levels > view.node_count + 1)
      throw std::runtime_error("CUDA targeted returned invalid level count");
    DownloadData data;
    data.nodes.resize(view.node_count);
    data.paths.resize(checkedProduct(checkedProduct(view.node_count, 8), view.tag_count));
    data.arc_state.resize(view.arc_count);
    data.child_state.resize(view.child_count);
    data.order.resize(view.node_count);
    data.node_levels.resize(view.node_count);
    data.level_offsets.resize(levels);
    memory.download(view.nodes, data.nodes);
    memory.download(view.paths, data.paths);
    memory.download(view.arc_state, data.arc_state);
    memory.download(view.child_state, data.child_state);
    memory.download(view.order, data.order);
    memory.download(view.node_levels, data.node_levels);
    memory.download(view.level_offsets, data.level_offsets);
    memory.record(5);
    memory.synchronize();
    values = unpackValues(std::move(data), layout);
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
  telemetry.cuda.levelization_ms = memory.milliseconds(2, 3);
  telemetry.cuda.propagation_ms = memory.milliseconds(3, 4);
  telemetry.cuda.d2h_ms = memory.milliseconds(4, 5);
}
CudaDualCsrSession::CudaDualCsrSession(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater water)
{
  const auto start = Clock::now();
  demand(validateReferenceInput(input).empty() && input.unsupported_reasons.empty(), "CUDA targeted baseline malformed or unsupported");
  demand(input.source_identity.session_token != 0 && water.node_slots == input.nodes.size() && water.arc_slots == input.arcs.size(),
         "CUDA targeted baseline binding mismatch");
  CsrCudaBackend::requireDevice();
  _impl = std::make_shared<Impl>(input, water);
  _impl->telemetry.cuda.end_to_end_ms = elapsed(start);
}
CudaDualCsrSession::CudaDualCsrSession(std::shared_ptr<Impl> impl) noexcept : _impl(std::move(impl))
{
}
CudaDualCsrSession::~CudaDualCsrSession() = default;
CudaDualCsrSession::CudaDualCsrSession(CudaDualCsrSession&&) noexcept = default;
CudaDualCsrSession& CudaDualCsrSession::operator=(CudaDualCsrSession&&) noexcept = default;
CudaDualCsrSession CudaDualCsrSession::stage(const CudaDualCsrPatch& patch)
{
  const auto start = Clock::now();
  demand(bool(_impl), "CUDA targeted owner was consumed");
  Impl::validatePatch(*_impl, patch);
  try {
    CudaDualCsrSession candidate(std::make_shared<Impl>(_impl, patch));
    candidate._impl->telemetry.cuda.end_to_end_ms = elapsed(start);
    return candidate;
  } catch (const CudaError&) {
    _impl->poisoned = true;
    throw;
  }
}
bool CudaDualCsrSession::publish(CudaDualCsrSession&& candidate) noexcept
{
  if (!_impl || !candidate._impl || _impl->poisoned || _impl->origin || candidate._impl->poisoned || candidate._impl->origin.get() != _impl.get()
      || candidate._impl->token != _impl->token || _impl->generation.value() == absent || candidate._impl->generation.value() != _impl->generation.value() + 1
      || candidate._impl->code || !candidate._impl->values)
    return false;
  _impl.swap(candidate._impl);
  _impl->origin.reset();
  candidate._impl.reset();
  return true;
}
const CsrTimingValues& CudaDualCsrSession::timingValues() const
{
  if (!_impl || !_impl->values)
    throw std::logic_error("CUDA targeted timing unavailable");
  return *_impl->values;
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
  return !_impl || _impl->poisoned;
}
CudaDualCsrDebugState CudaDualCsrSession::debugState() const
{
  if constexpr (!incregpusta_debug_checks_enabled)
    throw std::logic_error("CUDA device diagnostic requires checks ON");
  if (!_impl || _impl->poisoned)
    throw std::logic_error("CUDA targeted diagnostic owner unavailable");
  CudaDualCsrDebugState result;
  auto& state = *_impl;
  try {
    auto read = [&]<class T>(const T* source, Index count, std::vector<T>& target) {
      target.resize(count);
      state.memory.download(source, target);
    };
    read(state.view.out_offsets, state.out_offsets.size(), result.out_offsets);
    read(state.view.out_arcs, state.out_offsets.back(), result.out_arcs);
    read(state.view.in_offsets, state.in_offsets.size(), result.in_offsets);
    read(state.view.in_arcs, state.in_offsets.back(), result.in_arcs);
    read(state.view.aux_offsets, state.aux_offsets.size(), result.aux_offsets);
    read(state.view.aux_arcs, state.aux_offsets.back(), result.aux_arcs);
    read(state.view.source_to_aux_row, state.aux_mapping.size(), result.source_to_aux_row);
    read(state.view.arcs, state.view.arc_count, result.arcs);
    read(state.view.children, state.view.child_count, result.children);
    read(state.view.table_data, state.table_count, result.table_data);
    state.memory.download(state.view.storage_visits, result.storage_visits.data(), 8);
    state.memory.synchronize();
    return result;
  } catch (const CudaError&) {
    state.poisoned = true;
    if (state.origin)
      state.origin->poisoned = true;
    throw;
  }
}
}  // namespace ista::csr
