#include "CudaDualCsrInternal.cuh"

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
      || actual.clock != expected.clock || actual.has_load != expected.has_load || actual.levelization != expected.levelization
      || actual.data_arrival != expected.data_arrival || actual.data_slew != expected.data_slew || actual.clock_eligible != expected.clock_eligible)
    return false;
  for (unsigned i = 0; i < 4; ++i)
    if (actual.load[i] != expected.load[i])
      return false;
  return true;
}
__global__ void removeBase(DeviceView view, DevicePatch patch)
{
  if (*view.status != Status::success)
    return;
  for (Index i = 0; i < patch.remove_count; ++i)
    if (!slot(view, patch.removes[i], false)) {
      *view.status = Status::invariant;
      return;
    }
  for (Index i = 0; i < patch.deactivate_count; ++i) {
    const auto& record = patch.deactivations[i];
    Arc& arc = const_cast<Arc*>(view.arcs)[record.id];
    if (!arc.active || arc.source != record.source || arc.sink != record.sink) {
      *view.status = Status::invariant;
      return;
    }
    arc.active = false;
  }
}
__global__ void changeBase(DeviceView view, DevicePatch patch)
{
  if (*view.status != Status::success)
    return;
  for (Index i = 0; i < patch.payload_count; ++i) {
    const auto& record = patch.payloads[i];
    Arc& arc = const_cast<Arc*>(view.arcs)[record.id];
    if (!payloadEqual(arc, record.expected)) {
      *view.status = Status::invariant;
      return;
    }
    arc = record.replacement;
  }
  for (Index i = 0; i < patch.change_count; ++i)
    if (!slot(view, patch.changes[i], false)) {
      *view.status = Status::invariant;
      return;
    }
  for (Index i = 0; i < patch.aux_patch_count; ++i) {
    const auto& record = patch.aux_patches[i];
    auto& row = const_cast<AuxRow*>(view.aux_rows)[record.source];
    if (row.count != record.expected_count) {
      *view.status = Status::invariant;
      return;
    }
    for (Index j = 0; j < row.count; ++j)
      if (row.arcs[j] != patch.aux_values[record.expected_offset + j]) {
        *view.status = Status::invariant;
        return;
      }
    for (Index j = 0; j < record.replacement_count; ++j)
      const_cast<Index*>(row.arcs)[j] = patch.aux_values[record.replacement_offset + j];
    row.count = record.replacement_count;
  }
}
__global__ void reverseChangeBase(DeviceView view, DevicePatch patch)
{
  if (*view.status != Status::success)
    return;
  for (Index i = 0; i < patch.reverse_count; ++i)
    if (!slot(view, patch.reverse_changes[i], true)) {
      *view.status = Status::invariant;
      return;
    }
}
__global__ void captureUndo(DeviceView view, SlotUndo* slots, Index slot_count, ArcUndo* arcs, Index arc_count)
{
  for (Index i = 0; i < slot_count; ++i)
    slots[i].value = *slots[i].location;
  for (Index i = 0; i < arc_count; ++i)
    arcs[i].value = view.arcs[arcs[i].id];
}
__global__ void restoreUndo(DeviceView view, const SlotUndo* slots, Index slot_count, const ArcUndo* arcs, Index arc_count)
{
  // Restoration consumes actual saved values even when the patch failed midway.
  for (Index i = 0; i < slot_count; ++i)
    *slots[i].location = slots[i].value;
  for (Index i = 0; i < arc_count; ++i)
    const_cast<Arc*>(view.arcs)[arcs[i].id] = arcs[i].value;
}
}  // namespace
void launchCaptureUndo(DeviceView view, SlotUndo* slots, Index slot_count, ArcUndo* arcs, Index arc_count, cudaStream_t stream)
{
  if (slot_count || arc_count) {
    captureUndo<<<1, 1, 0, stream>>>(view, slots, slot_count, arcs, arc_count);
    check(cudaGetLastError(), "CUDA capture local undo launch");
  }
}
void launchRestoreUndo(DeviceView view, const SlotUndo* slots, Index slot_count, const ArcUndo* arcs, Index arc_count, cudaStream_t stream)
{
  if (slot_count || arc_count) {
    restoreUndo<<<1, 1, 0, stream>>>(view, slots, slot_count, arcs, arc_count);
    check(cudaGetLastError(), "CUDA restore local undo launch");
  }
}
void launchTargetedPatch(DeviceView view, DevicePatch patch, cudaStream_t stream)
{
  removeBase<<<1, 1, 0, stream>>>(view, patch);
  check(cudaGetLastError(), "CUDA Base remove launch");
  changeBase<<<1, 1, 0, stream>>>(view, patch);
  check(cudaGetLastError(), "CUDA Base change launch");
  reverseChangeBase<<<1, 1, 0, stream>>>(view, patch);
  check(cudaGetLastError(), "CUDA BaseIn reverse change launch");
}
}  // namespace ista::csr::cuda_detail
