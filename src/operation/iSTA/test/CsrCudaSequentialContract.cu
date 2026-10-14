#include <limits>
#include <tuple>

#include "CudaDualCsrInternal.cuh"
#include "CsrCudaSequentialPropagation.cuh"
#include "CudaDualCsrTestReader.hpp"

namespace ista::csr {
namespace {
using namespace cuda_detail;

__global__ void sequentialContractKernel(DeviceView view, Endpoint endpoint)
{
  sequentialPass(view);
  if (*view.status == Status::success)
    sequentialRequired(view, endpoint);
}

auto pathFields(const PathState& path)
{
  return std::tie(path.arrival, path.slew, path.launch, path.predecessor_delay, path.start, path.predecessor_node,
                  path.predecessor_arc, path.predecessor_transition, path.crpr_node, path.crpr_transition, path.present);
}

Index row(Index node, unsigned analysis, unsigned source, unsigned transition, Index tag)
{
  return (((node * 2 + analysis) * 2 + source) * 2 + transition) * 2 + tag;
}
void seedLoadPatchUndoContract()
{
  constexpr Index count = 513;
  CsrCudaTelemetry telemetry;
  Runtime memory(telemetry);
  DeviceView view;
  view.status = memory.allocate<Status>(1);
  DevicePatch patch;
  patch.seed_nodes = memory.allocate<NodeSeed>(count);
  auto* changes_device = memory.allocate<SeedLoadPatch>(count);
  auto* saved_device = memory.allocate<SeedLoadUndo>(count);
  patch.seed_loads = changes_device;
  patch.seed_load_count = count;
  std::vector<NodeSeed> original(count), result(count);
  std::vector<SeedLoadPatch> changes(count);
  std::vector<SeedLoadUndo> saved(count);
  for (Index i = 0; i < count; ++i) {
    original[i].node = i;
    original[i].state.sequential.kind = SeedKind::output;
    changes[i].slot = saved[i].slot = i;
    for (unsigned lane = 0; lane < 4; ++lane) {
      const double load = 1.0 + i + lane * 0.25;
      original[i].state.sequential.output_load[lane] = changes[i].expected[lane] = load;
      changes[i].replacement[lane] = load + 2.0;
      saved[i].value[lane] = -1.0;  // Capture must read actual device values.
    }
  }
  for (unsigned threads : {37U, 256U}) {
    Status status = Status::success;
    memory.upload(original, patch.seed_nodes);
    memory.upload(changes, changes_device);
    memory.upload(saved, saved_device);
    memory.upload(&status, view.status, 1);
    launchCaptureUndo(view, nullptr, 0, nullptr, 0, patch.seed_nodes, saved_device, count, memory.stream(), threads);
    launchTargetedPatch(view, patch, memory.stream(), threads);
    memory.download(view.status, &status, 1);
    memory.download(patch.seed_nodes, result);
    memory.synchronize();
    demand(status == Status::success, "CUDA Q seed load patch failed");
    for (Index i = 0; i < count; ++i)
      for (unsigned lane = 0; lane < 4; ++lane)
        demand(result[i].state.sequential.output_load[lane] == changes[i].replacement[lane],
               "CUDA Q seed patch omitted a unique target");
    // The second patch has stale expectations after real writes by the first.
    launchTargetedPatch(view, patch, memory.stream(), threads);
    memory.download(view.status, &status, 1);
    memory.synchronize();
    demand(status == Status::invariant, "CUDA Q seed patch accepted stale loads");
    launchRestoreUndo(view, nullptr, 0, nullptr, 0, patch.seed_nodes, saved_device, count, memory.stream(), threads);
    memory.download(patch.seed_nodes, result);
    memory.synchronize();
    for (Index i = 0; i < count; ++i)
      for (unsigned lane = 0; lane < 4; ++lane)
        demand(result[i].state.sequential.output_load[lane] == original[i].state.sequential.output_load[lane],
               "CUDA Q seed rollback did not restore actual captured values");
  }
}
}  // namespace

void runCudaSequentialContracts()
{
  seedLoadPatchUndoContract();
  CsrCudaTelemetry telemetry;
  Runtime memory(telemetry);
  DeviceView view;
  view.node_count = 4;
  view.arc_count = 1;
  view.child_count = 2;
  view.tag_count = 2;
  view.nodes = memory.allocate<NodeState>(4);
  view.path_count = 64;
  view.paths = memory.allocate<PathState>(64);
  std::vector<Index> path_offsets(33), path_tags(64);
  for (Index index = 0; index < path_offsets.size(); ++index)
    path_offsets[index] = index * 2;
  for (Index slot = 0; slot < path_tags.size(); ++slot)
    path_tags[slot] = slot % 2;
  view.path_offsets = memory.allocate<Index>(path_offsets.size());
  view.path_tags = memory.allocate<Index>(path_tags.size());
  memory.upload(path_offsets, view.path_offsets);
  memory.upload(path_tags, view.path_tags);
  view.arcs = memory.allocate<Arc>(1);
  view.children = memory.allocate<Child>(2);
  view.arc_state = memory.allocate<ArcState>(1);
  view.out_offsets = memory.allocate<Index>(5);
  view.out_arcs = memory.allocate<Index>(1);
  view.status = memory.allocate<Status>(1);
  const std::vector<double> tables{0.0, 10.0, 1.0, 11.0, 0.5, 0.0, 100.0, 0.0, 100.0, 0.0, 100.0, 200.0, 300.0};
  view.table_data = memory.allocate<double>(tables.size());
  const std::vector<Index> offsets{0, 0, 1, 1, 1}, outgoing_arcs{0};
  memory.upload(offsets, view.out_offsets);
  memory.upload(outgoing_arcs, view.out_arcs);
  memory.upload(tables, view.table_data);
  memory.synchronize();
  std::vector<NodeState> seeds(4);
  std::vector<PathState> paths(64);
  std::vector<Arc> arcs(1);
  std::vector<Child> children(2);
  std::vector<ArcState> cached(1);
  cached[0].delay = 42.0;  // Standalone CTQ must not mutate graph timing caches.
  for (Index node = 0; node < 2; ++node) {
    auto& seed = seeds[node].sequential;
    seed.kind = node == 0 ? SeedKind::clock : SeedKind::output;
    seed.clock_node = 0;
    seed.path_start = 1;
    seed.path_tag = 0;
    seed.trigger = 1;
    seed.child_count = node == 1 ? 1 : 0;
  }
  seeds[3].sequential.kind = SeedKind::scalar;
  seeds[0].propagated_clock = seeds[2].propagated_clock = true;
  for (unsigned lane = 0; lane < 4; ++lane) {
    seeds[0].physical_clock[lane].arrival = {lane % 2 == 0 ? 99.0 : (lane < 2 ? 7.0 : 3.0), true};
    seeds[0].physical_clock[lane].slew = {lane % 2 == 0 ? 99.0 : (lane < 2 ? 4.0 : 2.0), true};
    seeds[2].physical_clock[lane].arrival = {999.0, true};
    seeds[2].physical_clock[lane].slew = {999.0, true};
    seeds[1].sequential.output_load[lane] = 2.0;
  }
  arcs[0].source = 1;
  arcs[0].sink = 2;
  children[0].sense = 3;  // Non-unate, falling trigger, both output transitions.
  children[0].trigger = 1;
  for (unsigned transition = 0; transition < 2; ++transition) {
    auto& delay = children[0].delay[transition];
    delay.present = true;
    delay.dimensions = 1;
    delay.variable1 = 2;
    delay.axis_size[0] = 2;
    delay.value_offset = 2;
    delay.value_size = 2;
    auto& slew = children[0].slew[transition];
    slew.present = true;
    slew.value_offset = 4;
    slew.value_size = 1;
    auto& check_table = children[1].check[transition];
    check_table.present = true;
    check_table.dimensions = 2;
    check_table.variable1 = 3;
    check_table.axis_offset[0] = 5;
    check_table.axis_offset[1] = 7;
    check_table.axis_size[0] = check_table.axis_size[1] = 2;
    check_table.value_offset = 9;
    check_table.value_size = 4;
  }
  children[1].time_scale = 10.0;
  children[1].capacitance_scale = 123.0;
  children[1].trigger = 0;  // Check candidates are not filtered by clock trigger.
  Endpoint endpoint;
  endpoint.node = 3;
  endpoint.period = 100.0;
  endpoint.sequential_check = true;
  endpoint.clock_node = 0;
  endpoint.clock_transition = 1;
  endpoint.child_begin = 1;
  endpoint.child_count = 1;
  auto& rise = paths[row(3, 0, 0, 0, 0)];
  rise.present = true;
  rise.arrival = 10.0;
  rise.slew = 1.25;
  auto& fall0 = paths[row(3, 0, 0, 1, 0)];
  fall0 = rise;
  fall0.arrival += 0.5e-6;
  fall0.slew = 8.0;
  auto& fall1 = paths[row(3, 0, 0, 1, 1)];
  fall1 = fall0;
  fall1.arrival = 10.0 + 1.2e-6;
  auto& reg = paths[row(3, 0, 1, 1, 1)];
  reg.present = true;
  reg.arrival = 100.0;
  reg.slew = 5.0;
  std::vector<NodeState> result(4);
  std::vector<PathState> result_paths(64);
  std::vector<ArcState> result_cache(1);
  const auto run = [&]() {
    Status status = Status::success;
    memory.upload(seeds, view.nodes);
    memory.upload(paths, view.paths);
    memory.upload(arcs, view.arcs);
    memory.upload(children, view.children);
    memory.upload(cached, view.arc_state);
    memory.upload(&status, view.status, 1);
    sequentialContractKernel<<<1, 1, 0, memory.stream()>>>(view, endpoint);
    check(cudaGetLastError(), "sequential contract launch");
    memory.download(view.status, &status, 1);
    memory.download(view.nodes, result);
    memory.download(view.paths, result_paths);
    memory.download(view.arc_state, result_cache);
    memory.synchronize();
    return status;
  };
  demand(run() == Status::success, "CUDA sequential/check computation failed");
  demand(result[0].arrival == 7.0 && result[1].arrival == 10.0 && result[3].arrival == 0.0 && result[3].required == 98.5,
         "CUDA CK/Q seed or input-priority two-stage endpoint winner differs");
  auto expected_paths = paths;
  for (Index node = 0; node < 2; ++node) {
    for (unsigned analysis = 0; analysis < 2; ++analysis) {
      for (unsigned transition = 0; transition < 2; ++transition) {
        PathState expected;
        expected.present = true;
        expected.arrival = (analysis == 0 ? 7.0 : 3.0) + (node == 1 ? 3.0 : 0.0);
        expected.launch = analysis == 0 ? 7.0 : 3.0;
        expected.slew = node == 1 ? 0.5 : (analysis == 0 ? 4.0 : 2.0);
        expected.start = 1;
        expected.crpr_node = 0;
        expected.crpr_transition = 1;
        expected_paths[row(node, analysis, 1, transition, 0)] = expected;
        demand(result[node].slew[analysis * 2 + transition].present
                   && result[node].slew[analysis * 2 + transition].value == expected.slew,
               "CUDA selected-trigger data slew differs");
      }
    }
  }
  for (std::size_t index = 0; index < paths.size(); ++index)
    demand(pathFields(result_paths[index]) == pathFields(expected_paths[index]), "CUDA register path, winner or CRPR identity differs");
  demand(result_cache[0].delay == 42.0, "standalone CUDA CTQ changed graph cache");
  seeds[0].propagated_clock = false;
  demand(run() == Status::success && result[0].arrival == 0.0 && result[1].arrival == 3.0
             && result[3].required == 97.5,
         "ideal clock consumed physical delay or slew");
  seeds[0].propagated_clock = true;
  for (auto& load : seeds[1].sequential.output_load)
    load = 4.0;
  rise = fall0 = fall1 = {};
  demand(run() == Status::success && result[1].arrival == 12.0 && result[3].required == 91.0,
         "CUDA Q load refresh or register endpoint check differs");
  children[1].check[1].present = false;
  seeds[3].required = 1234.0;
  demand(run() == Status::success && result[3].required == 1234.0, "missing check transition did not preserve required");
  children[1].check[1].present = true;
  children[1].time_scale = std::numeric_limits<double>::max();
  demand(run() == Status::numeric, "CUDA timing-check numeric overflow was not rejected");
  children[1].time_scale = 10.0;
  seeds[1].sequential.output_load[0] = std::numeric_limits<double>::quiet_NaN();
  demand(run() == Status::numeric, "CUDA sequential Q accepted a non-finite output load");
  seeds[1].sequential.output_load[0] = 4.0;
  for (Index node = 0; node < 2; ++node) {
    seeds[node].sequential.path_tag = absent;
    seeds[node].sequential.clock_node = absent;
  }
  demand(run() == Status::success && result[0].arrival == 0.0 && result[1].arrival == 5.0,
         "CUDA unclocked sequential scalar calculation was lost");
  for (Index node = 0; node < 2; ++node) {
    seeds[node].sequential.clock_node = 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
      seeds[node].physical_clock[lane].arrival = {};
      seeds[node].physical_clock[lane].slew = {};
    }
  }
  demand(run() == Status::success && result[0].arrival == 0.0 && result[1].arrival == 5.0,
         "CUDA existing unreachable CK lost scalar-only output calculation");
  for (Index node = 0; node < 2; ++node)
    for (Index lane = node * 16; lane < (node + 1) * 16; ++lane)
      demand(!result_paths[lane].present, "CUDA unclocked sequential start invented a path");
}
}  // namespace ista::csr
