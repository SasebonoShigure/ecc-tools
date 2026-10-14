#include "CudaDualCsrInternal.cuh"
#include "CsrCudaKernels.cuh"

namespace ista::csr::cuda_detail {
namespace {
__device__ bool slot(DeviceView view, const SlotPatch& patch, bool incoming)
{
  const Index* offsets = incoming ? view.in_offsets : view.out_offsets;
  Index* arcs = const_cast<Index*>(incoming ? view.in_arcs : view.out_arcs);
  if (patch.row >= view.node_count || patch.slot < offsets[patch.row] || patch.slot >= offsets[patch.row + 1] || arcs[patch.slot] != patch.expected)
    return false;
  if (patch.replacement != absent
      && (patch.replacement >= view.arc_count || !view.arcs[patch.replacement].active
          || (incoming ? view.arcs[patch.replacement].sink : view.arcs[patch.replacement].source) != patch.row))
    return false;
  arcs[patch.slot] = patch.replacement;
  return true;
}
__device__ bool payloadEqual(const Arc& actual, const Arc& expected)
{
  if (!actual.active || actual.source != expected.source || actual.sink != expected.sink || actual.child_count != expected.child_count || actual.child_begin != expected.child_begin
      || actual.delay != expected.delay || actual.delay_min != expected.delay_min || actual.delay_max != expected.delay_max || actual.net != expected.net
      || actual.rc_binding != expected.rc_binding || actual.clock != expected.clock || actual.has_load != expected.has_load || actual.levelization != expected.levelization
      || actual.data_arrival != expected.data_arrival || actual.data_slew != expected.data_slew || actual.clock_eligible != expected.clock_eligible)
    return false;
  for (unsigned i = 0; i < 4; ++i)
    if (actual.load[i] != expected.load[i])
      return false;
  return true;
}
__global__ void removeBase(DeviceView view, DevicePatch patch)
{
  if (readStatus(view.status) != Status::success)
    return;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < patch.remove_count;
       i += static_cast<Index>(gridDim.x) * blockDim.x)
    if (!slot(view, patch.removes[i], false))
      recordFailure(view.status, Status::invariant);
}
__global__ void deactivateArcs(DeviceView view, DevicePatch patch)
{
  if (readStatus(view.status) != Status::success)
    return;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < patch.deactivate_count;
       i += static_cast<Index>(gridDim.x) * blockDim.x) {
    const auto& record = patch.deactivations[i];
    Arc& arc = const_cast<Arc*>(view.arcs)[record.id];
    if (!arc.active || arc.source != record.source || arc.sink != record.sink)
      recordFailure(view.status, Status::invariant);
    else
      arc.active = false;
  }
}
__global__ void changePayloads(DeviceView view, DevicePatch patch)
{
  if (readStatus(view.status) != Status::success)
    return;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < patch.payload_count;
       i += static_cast<Index>(gridDim.x) * blockDim.x) {
    const auto& record = patch.payloads[i];
    Arc& arc = const_cast<Arc*>(view.arcs)[record.id];
    if (!payloadEqual(arc, record.expected))
      recordFailure(view.status, Status::invariant);
    else
      arc = record.replacement;
  }
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < patch.seed_load_count;
       i += static_cast<Index>(gridDim.x) * blockDim.x) {
    const auto& record = patch.seed_loads[i];
    auto& seed = patch.seed_nodes[record.slot].state.sequential;
    bool matches = seed.kind == SeedKind::output && seed.rc_binding == absent;
    for (unsigned lane = 0; lane < 4; ++lane)
      matches = matches && seed.output_load[lane] == record.expected[lane];
    if (!matches)
      recordFailure(view.status, Status::invariant);
    else
      for (unsigned lane = 0; lane < 4; ++lane)
        seed.output_load[lane] = record.replacement[lane];
  }
}
__global__ void changeSlots(DeviceView view, DevicePatch patch)
{
  if (readStatus(view.status) != Status::success)
    return;
  const Index count = patch.change_count + patch.reverse_count;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
       i += static_cast<Index>(gridDim.x) * blockDim.x) {
    const bool incoming = i >= patch.change_count;
    const auto& record = incoming ? patch.reverse_changes[i - patch.change_count] : patch.changes[i];
    if (!slot(view, record, incoming))
      recordFailure(view.status, Status::invariant);
  }
}
__global__ void checkAux(DeviceView view, DevicePatch patch)
{
  if (readStatus(view.status) != Status::success)
    return;
  // One block owns a row; wide rows are checked by all its threads.
  for (Index i = blockIdx.x; i < patch.aux_patch_count; i += gridDim.x) {
    const auto& record = patch.aux_patches[i];
    const auto& row = view.aux_rows[record.source];
    if (row.count != record.expected_count) {
      recordFailure(view.status, Status::invariant);
      continue;
    }
    for (Index j = threadIdx.x; j < row.count; j += blockDim.x)
      if (row.arcs[j] != patch.aux_values[record.expected_offset + j])
        recordFailure(view.status, Status::invariant);
  }
}
__global__ void changeAux(DeviceView view, DevicePatch patch)
{
  if (readStatus(view.status) != Status::success)
    return;
  for (Index i = blockIdx.x; i < patch.aux_patch_count; i += gridDim.x) {
    const auto& record = patch.aux_patches[i];
    auto& row = const_cast<AuxRow*>(view.aux_rows)[record.source];
    for (Index j = threadIdx.x; j < record.replacement_count; j += blockDim.x)
      const_cast<Index*>(row.arcs)[j] = patch.aux_values[record.replacement_offset + j];
    // No thread here reads count; downstream users wait for this whole kernel.
    if (threadIdx.x == 0)
      row.count = record.replacement_count;
  }
}
__global__ void bindAux(DeviceView view, const AuxBinding* bindings, Index count, Index capacity)
{
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < count;
       i += static_cast<Index>(gridDim.x) * blockDim.x) {
    const auto& binding = bindings[i];
    // Rollback may follow failed capacity preparation of a newly appended row.
    if (binding.source < view.node_count && binding.source < capacity)
      const_cast<AuxRow*>(view.aux_rows)[binding.source] = binding.before;
  }
}
__global__ void captureUndo(DeviceView view, SlotUndo* slots, Index slot_count, ArcUndo* arcs, Index arc_count, NodeSeed* seeds, SeedLoadUndo* loads, Index load_count)
{
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < slot_count;
       i += static_cast<Index>(gridDim.x) * blockDim.x)
    slots[i].value = *slots[i].location;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < arc_count;
       i += static_cast<Index>(gridDim.x) * blockDim.x)
    arcs[i].value = view.arcs[arcs[i].id];
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < load_count;
       i += static_cast<Index>(gridDim.x) * blockDim.x)
    for (unsigned lane = 0; lane < 4; ++lane)
      loads[i].value[lane] = seeds[loads[i].slot].state.sequential.output_load[lane];
}
__global__ void restoreUndo(DeviceView view, const SlotUndo* slots, Index slot_count, const ArcUndo* arcs, Index arc_count, NodeSeed* seeds, const SeedLoadUndo* loads, Index load_count)
{
  // Restoration consumes actual saved values even when the patch failed midway.
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < slot_count;
       i += static_cast<Index>(gridDim.x) * blockDim.x)
    *slots[i].location = slots[i].value;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < arc_count;
       i += static_cast<Index>(gridDim.x) * blockDim.x)
    const_cast<Arc*>(view.arcs)[arcs[i].id] = arcs[i].value;
  for (Index i = static_cast<Index>(blockIdx.x) * blockDim.x + threadIdx.x; i < load_count;
       i += static_cast<Index>(gridDim.x) * blockDim.x)
    for (unsigned lane = 0; lane < 4; ++lane)
      seeds[loads[i].slot].state.sequential.output_load[lane] = loads[i].value[lane];
}
}  // namespace
void launchAuxBindings(DeviceView view, const AuxBinding* bindings, Index count, Index capacity, cudaStream_t stream, unsigned threads)
{
  bindAux<<<kernelBlocks(count, threads), threads, 0, stream>>>(view, bindings, count, capacity);
  check(cudaGetLastError(), "CUDA Aux row binding launch");
}
void launchCaptureUndo(DeviceView view, SlotUndo* slots, Index slot_count, ArcUndo* arcs, Index arc_count,
                       NodeSeed* seeds, SeedLoadUndo* loads, Index load_count,
                       cudaStream_t stream, unsigned threads)
{
  if (slot_count || arc_count || load_count) {
    captureUndo<<<kernelBlocks(std::max({slot_count, arc_count, load_count}), threads), threads, 0, stream>>>(view, slots, slot_count, arcs, arc_count, seeds, loads, load_count);
    check(cudaGetLastError(), "CUDA capture local undo launch");
  }
}
void launchRestoreUndo(DeviceView view, const SlotUndo* slots, Index slot_count, const ArcUndo* arcs, Index arc_count,
                       NodeSeed* seeds, const SeedLoadUndo* loads, Index load_count,
                       cudaStream_t stream, unsigned threads)
{
  if (slot_count || arc_count || load_count) {
    restoreUndo<<<kernelBlocks(std::max({slot_count, arc_count, load_count}), threads), threads, 0, stream>>>(view, slots, slot_count, arcs, arc_count, seeds, loads, load_count);
    check(cudaGetLastError(), "CUDA restore local undo launch");
  }
}
std::uint64_t launchTargetedPatch(DeviceView view, DevicePatch patch, cudaStream_t stream, unsigned threads)
{
  std::uint64_t launches = 0;
  if (patch.remove_count) {
    removeBase<<<kernelBlocks(patch.remove_count, threads), threads, 0, stream>>>(view, patch);
    check(cudaGetLastError(), "CUDA Base remove launch");
    ++launches;
  }
  if (patch.deactivate_count) {
    deactivateArcs<<<kernelBlocks(patch.deactivate_count, threads), threads, 0, stream>>>(view, patch);
    check(cudaGetLastError(), "CUDA deactivate arc launch");
    ++launches;
  }
  // Payloads must be complete before any slot validates replacement metadata.
  if (patch.payload_count || patch.seed_load_count) {
    changePayloads<<<kernelBlocks(std::max(patch.payload_count, patch.seed_load_count), threads), threads, 0, stream>>>(view, patch);
    check(cudaGetLastError(), "CUDA payload change launch");
    ++launches;
  }
  if (patch.change_count || patch.reverse_count) {
    changeSlots<<<kernelBlocks(checkedAdd(patch.change_count, patch.reverse_count), threads), threads, 0, stream>>>(view, patch);
    check(cudaGetLastError(), "CUDA Base slot change launch");
    ++launches;
  }
  if (patch.aux_patch_count) {
    const auto blocks = static_cast<unsigned>(std::min<Index>(65535, patch.aux_patch_count));
    checkAux<<<blocks, threads, 0, stream>>>(view, patch);
    check(cudaGetLastError(), "CUDA Aux expected-value launch");
    changeAux<<<blocks, threads, 0, stream>>>(view, patch);
    check(cudaGetLastError(), "CUDA Aux change launch");
    launches += 2;
  }
  return launches;
}
}  // namespace ista::csr::cuda_detail
