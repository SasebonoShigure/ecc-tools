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
__device__ bool tableEqual(const Table& a, const double* av, const Table& b, const double* bv)
{
  if (a.present != b.present || a.dimensions != b.dimensions || a.variable1 != b.variable1 || a.value_size != b.value_size)
    return false;
  for (unsigned axis = 0; axis < 2; ++axis) {
    if (a.axis_size[axis] != b.axis_size[axis])
      return false;
    for (Index i = 0; i < a.axis_size[axis]; ++i)
      if (av[a.axis_offset[axis] + i] != bv[b.axis_offset[axis] + i])
        return false;
  }
  for (Index i = 0; i < a.value_size; ++i)
    if (av[a.value_offset + i] != bv[b.value_offset + i])
      return false;
  return true;
}
__device__ bool payloadEqual(DeviceView view, DevicePatch patch, const Arc& actual, const Arc& expected)
{
  if (!actual.active || actual.source != expected.source || actual.sink != expected.sink || actual.child_count != expected.child_count
      || actual.delay != expected.delay || actual.delay_min != expected.delay_min || actual.delay_max != expected.delay_max || actual.net != expected.net
      || actual.clock != expected.clock || actual.has_load != expected.has_load || actual.levelization != expected.levelization
      || actual.data_arrival != expected.data_arrival || actual.data_slew != expected.data_slew)
    return false;
  for (unsigned i = 0; i < 4; ++i)
    if (actual.load[i] != expected.load[i])
      return false;
  for (Index i = 0; i < actual.child_count; ++i) {
    const Child& a = view.children[actual.child_begin + i];
    const Child& b = patch.expected_children[expected.child_begin + i];
    if (a.time_scale != b.time_scale || a.capacitance_scale != b.capacitance_scale || a.arc_index != b.arc_index || a.sense != b.sense
        || a.trigger != b.trigger)
      return false;
    for (unsigned t = 0; t < 2; ++t)
      if (a.check_present[t] != b.check_present[t] || !tableEqual(a.delay[t], view.table_data, b.delay[t], patch.expected_tables)
          || !tableEqual(a.slew[t], view.table_data, b.slew[t], patch.expected_tables))
        return false;
  }
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
    if (!payloadEqual(view, patch, arc, record.expected)) {
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
  for (Index i = 0; i < patch.aux_expected_count; ++i) {
    const auto& row = patch.aux_expected[i];
    for (Index j = 0; j < row.count; ++j)
      if (patch.old_aux_arcs[row.old_offset + j] != patch.expected_aux_arcs[row.payload_offset + j]) {
        *view.status = Status::invariant;
        return;
      }
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
}  // namespace
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
