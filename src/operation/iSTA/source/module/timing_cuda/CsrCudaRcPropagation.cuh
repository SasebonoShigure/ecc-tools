#pragma once

#include "CsrCudaParasitics.cuh"

namespace ista::csr::cuda_detail {

__device__ inline RcModelView rcModel(const RcNet& net, unsigned lane)
{
  return {net.models + net.lanes[lane], net.terminals};
}

__device__ inline double cellTiming(DeviceView view, Index binding_index, Index child_begin, Index local,
                                   unsigned analysis, unsigned output, double input_slew, double load, bool slew)
{
  const Index child_index = child_begin + local;
  const auto& child = view.children[child_index];
  if (binding_index == absent)
    return lookupTable(view, slew ? child.slew[output] : child.delay[output], child, input_slew, load);
  const auto& binding = view.rc_bindings[binding_index];
  auto& net = view.rc_nets[binding.net];
  const auto lane = analysis * 2 + output;
  if (binding.child_begin > net.child_count || local >= net.child_count - binding.child_begin) {
    *view.status = Status::invariant;
    return 0.0;
  }
  const auto model = rcModel(net, lane);
  auto& row = net.rows[lane * net.child_count + binding.child_begin + local];
  const auto* record = cachedRcDriver(view, model, child_index, output, input_slew, row, net.drivers[lane], net.records);
  if (!record) return 0.0;
  const double value = rcCellValue(view, model, *record, output, slew);
  if (!isfinite(value) || (slew && value < 0.0))
    return numeric(view);
  return value;
}

__device__ inline void selectRcDriver(DeviceView view, Index node, bool clock)
{
  if (!view.rc_net_count) return;
  const auto index = view.rc_net_by_node[node];
  if (index == absent) return;
  auto& net = view.rc_nets[index];
  for (unsigned lane = 0; lane < 4; ++lane) {
    const auto slew = clock ? view.nodes[node].physical_clock[lane].slew : view.nodes[node].slew[lane];
    if (slew.present)
      selectCsrRcDriverRecords(net.records, net.rows, net.drivers[lane], slew.value);
    else
      net.drivers[lane].selected_arnoldi = net.drivers[lane].selected_dmp = kCsrRcAbsent;
  }
}

__device__ inline CsrRcEvaluationStatus netTiming(
    DeviceView view, Index binding_index, unsigned analysis, unsigned output,
    double input_slew, bool initialize, double& delay, double& slew)
{
  const auto& binding = view.rc_bindings[binding_index];
  const auto& net = view.rc_nets[binding.net];
  const auto lane = analysis * 2 + output;
  const auto model = rcModel(net, lane);
  auto status = CsrRcEvaluationStatus::kUnavailable;
  const Child* driver_child = nullptr;
  if (!initialize) {
    const auto& driver = net.drivers[lane];
    for (unsigned family = 0; family < 2 && status == CsrRcEvaluationStatus::kUnavailable; ++family) {
      const auto index = family == 0 ? driver.selected_arnoldi : driver.selected_dmp;
      if (index == kCsrRcAbsent) continue;
      const auto& record = net.records[index];
      if (record.child_model >= view.child_count)
        return CsrRcEvaluationStatus::kNumericFailure;
      driver_child = view.children + record.child_model;
      status = evaluateRcLoad(model, binding.terminal, record, *driver_child, output, input_slew, delay, slew);
    }
  }
  if (status == CsrRcEvaluationStatus::kSuccess && binding.terminal && binding.load_cell != absent) {
    const auto& load = view.rc_load_cells[binding.load_cell];
    if (load.library != driver_child->library) {
      const auto normalize = [](double value) { return value > 1.0 ? value * 0.01 : value; };
      status = adjustCsrParasiticLoadThreshold(
          {normalize(driver_child->lower[output]), normalize(driver_child->upper[output]),
           normalize(driver_child->output_threshold[output]), driver_child->slew_derate},
          {normalize(load.lower[output]), normalize(load.upper[output]),
           normalize(load.input_threshold[output]), load.slew_derate}, output == 1, delay, slew);
    }
  }
  if (status != CsrRcEvaluationStatus::kUnavailable) return status;
  return evaluateRcNetFallback(model, binding.terminal, binding.input_port,
      csrArnoldiThresholds(view.rc_port_lower[output], view.rc_port_upper[output], view.rc_port_derate),
      input_slew, delay, slew);
}

}  // namespace ista::csr::cuda_detail
