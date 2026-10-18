#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "CsrCudaPacking.hpp"
#include "CsrCudaMemory.cuh"
#include "CsrCudaParasitics.cuh"
#include "CsrCpuReferenceParasitics.hpp"

namespace {
using namespace ista::csr;
using namespace ista::csr::cuda_detail;

struct Output
{
  CsrRcEvaluationStatus driver_status, load_status, port_status, raw_status;
  CsrRcDriverFamily family{CsrRcDriverFamily::unavailable};
  double delay{}, slew{}, wire{}, load_slew{}, port_wire{}, port_slew{}, raw_wire{}, raw_slew{};
  Status status{};
  bool cache_ok{false}, failure_ok{false};
};

struct Request
{
  Index model{}, child{};
  unsigned output{}, analysis{};
  Index tree{};
  double input_slew{};
};

__global__ void evaluatePacked(DeviceView view, const RcModel* models, const RcTerminal* terminals,
                              const Request* requests, Output* outputs, CsrRcTimingRecord* records, Index count)
{
  for (Index id = blockIdx.x * blockDim.x + threadIdx.x; id < count; id += blockDim.x * gridDim.x) {
    Output result{};
    view.status = &result.status;
    const auto request = requests[id];
    const RcModelView model{models + request.model, terminals};
    CsrRcTimingRow rows[2]{{id * 6, 5, 0}, {id * 6 + 5, 1, 0}};
    CsrRcDriverLane driver{0, 2};
    const auto* first = cachedRcDriver(view, model, request.child, request.output, request.input_slew, rows[0], driver, records);
    const auto* other_identity = cachedRcDriver(view, model, request.child, request.output, request.input_slew, rows[1], driver, records);
    const auto* hit = cachedRcDriver(view, model, request.child, request.output, request.input_slew, rows[0], driver, records);
    result.cache_ok = first && other_identity && hit == first && first != other_identity && driver.miss_sequence == 2;
    if (first) {
      result.family = first->family;
      const auto family = first->family;
      result.driver_status = family == CsrRcDriverFamily::unavailable
          ? CsrRcEvaluationStatus::kUnavailable : CsrRcEvaluationStatus::kSuccess;
      if (family != CsrRcDriverFamily::unavailable) {
        const double slew = family == CsrRcDriverFamily::arnoldi ? first->arnoldi.driver_slew : first->dmp.driver_slew;
        selectCsrRcDriverRecords(records, rows, driver, slew);
        const auto selected = family == CsrRcDriverFamily::arnoldi ? driver.selected_arnoldi : driver.selected_dmp;
        result.cache_ok &= selected == rows[1].begin;
      }
    } else {
      result.driver_status = CsrRcEvaluationStatus::kNumericFailure;
    }
    if (result.driver_status != CsrRcEvaluationStatus::kNumericFailure) {
      result.delay = rcCellValue(view, model, *first, request.output, false);
      result.slew = rcCellValue(view, model, *first, request.output, true);
      result.load_status = evaluateRcLoad(model, 1, *first, view.children[request.child],
          request.output, result.slew, result.wire, result.load_slew);
    }
    const auto thresholds = csrArnoldiThresholds(35.0, 85.0, 0.6);
    result.port_status = evaluateRcNetFallback(model, 1, true, thresholds, request.input_slew,
        result.port_wire, result.port_slew);
    result.raw_status = evaluateRcNetFallback(model, 1, false, thresholds, request.input_slew,
        result.raw_wire, result.raw_slew);
    // Failed lookup/append must not publish a record or change the selected
    // driver. Use a separate worker-local status for these deliberate failures.
    auto failure_view = view;
    Status failure = Status::success;
    failure_view.status = &failure;
    const auto selected_arnoldi = driver.selected_arnoldi, selected_dmp = driver.selected_dmp;
    const auto unchanged = [&]() {
      return rows[0].size == 1 && rows[1].size == 1 && driver.miss_sequence == 2
          && driver.selected_arnoldi == selected_arnoldi && driver.selected_dmp == selected_dmp;
    };
    const auto* full_hit = cachedRcDriver(failure_view, model, request.child, request.output,
        request.input_slew, rows[1], driver, records);
    result.failure_ok = full_hit == other_identity && failure == Status::success && unchanged();
    const auto* full_miss = cachedRcDriver(failure_view, model, request.child, request.output,
        request.input_slew + 0.01, rows[1], driver, records);
    result.failure_ok &= full_miss == nullptr && failure == Status::invariant && unchanged();
    failure = Status::success;
    const auto* invalid = cachedRcDriver(failure_view, model, request.child, request.output,
        nan(""), rows[0], driver, records);
    result.failure_ok &= invalid == nullptr && failure == Status::numeric && unchanged();
    failure = Status::success;
    const auto* wrong_child = cachedRcDriver(failure_view, model, request.child == 0 ? 1 : 0,
        request.output, request.input_slew, rows[0], driver, records);
    result.failure_ok &= wrong_child == nullptr && failure == Status::invariant && unchanged();
    outputs[id] = result;
  }
}

void demand(bool value, const char* message)
{
  if (!value) throw std::runtime_error(message);
}
void near(double actual, double expected)
{
  demand(std::isfinite(actual) && std::isfinite(expected) && std::abs(actual - expected) <= 1e-6,
      "packed CUDA RC numeric mismatch");
}

ChildTimingArcModel makeChild()
{
  ChildTimingArcModel child;
  child.signature.library_name = "driver.lib";
  child.time_unit_scale = 1000.0;
  child.capacitance_unit_scale = 1000.0;
  child.slew_derate = 0.8;
  child.slew_lower_threshold_pct = {0.1, 0.2};
  child.slew_upper_threshold_pct = {0.9, 0.8};
  child.output_threshold_pct = {0.45, 0.55};
  for (unsigned output = 0; output < 2; ++output) {
    auto& delay = child.delay_tables[output];
    delay.present = true;
    delay.variable1 = TableVariable::kInputTransition;
    delay.variable2 = TableVariable::kOutputCapacitance;
    delay.axes = {{0.0, 250.0}, {0.0, 100.0}};
    delay.values = {20.0, 220.0, 70.0, 270.0};
    auto& slew = child.slew_tables[output];
    slew = delay;
    slew.values = {30.0, 330.0, 130.0, 430.0};
    child.signature.delay_tables[output] = {true, delay.variable1, delay.variable2};
    child.signature.slew_tables[output] = {true, slew.variable1, slew.variable2};
  }
  return child;
}

[[maybe_unused]] void compareCpu(const CsrParasiticTree& tree, const ChildTimingArcModel& child,
                const Request& request, const Output& result)
{
  auto owner = std::make_shared<CsrParasiticTree>(tree);
  const CsrParasiticBinding driver{owner, 0}, load{owner, 1};
  const auto transition = request.output ? Transition::kFall : Transition::kRise;
  const auto analysis = request.analysis ? AnalysisMode::kMin : AnalysisMode::kMax;
  const CsrCpuRcChildKey key{NodeId{0}, false, 0, 0, analysis, transition};
  CsrCpuRcCache cache;
  double delay{}, slew{};
  const auto status = evaluateCsrCpuRcChild(driver, key, child, request.input_slew, cache, delay, slew);
  demand(result.driver_status == status, "packed CUDA RC dispatch differs from CPU");
  demand(evaluateCsrCpuCellTiming(driver, key, child, request.input_slew, 0.0, false, cache, delay)
         == CsrRcEvaluationStatus::kSuccess, "CPU RC delay failed");
  demand(evaluateCsrCpuCellTiming(driver, key, child, request.input_slew, 0.0, true, cache, slew)
         == CsrRcEvaluationStatus::kSuccess, "CPU RC slew failed");
  near(result.delay, delay);
  near(result.slew, slew);
  double wire{}, load_slew{};
  const auto load_status = evaluateCsrCpuRcLoad(load, analysis, transition, slew, nullptr, cache, wire, load_slew);
  demand(load_status == result.load_status, "packed CUDA RC load dispatch differs");
  if (load_status == CsrRcEvaluationStatus::kSuccess) {
    near(result.wire, wire); near(result.load_slew, load_slew);
  }
  const InputPortRcThresholds thresholds{0.6, {35.0, 35.0}, {85.0, 85.0}};
  demand(evaluateCsrCpuNetTiming(load, analysis, transition, request.input_slew, true,
         nullptr, thresholds, nullptr, wire, load_slew) == result.port_status, "CUDA RC port status differs");
  near(result.port_wire, wire); near(result.port_slew, load_slew);
  demand(evaluateCsrCpuNetTiming(load, analysis, transition, request.input_slew, false,
         nullptr, thresholds, nullptr, wire, load_slew) == result.raw_status, "CUDA raw-tree status differs");
  near(result.raw_wire, wire); near(result.raw_slew, load_slew);
}
}  // namespace

void testCudaParasiticPacking()
{
  const std::vector<CsrRcTreeNode> nodes{{0, 0.0, 0.0153}, {0, 2.0, 0.0352}};
  const std::vector<std::size_t> terminals{0, 1};
  const auto original = buildCsrParasiticModel(nodes, terminals);
  std::vector<CsrParasiticTree> trees;
  for (unsigned family = 0; family < 3; ++family) {
    auto model = std::make_shared<CsrParasiticModel>(original);
    if (family) model->arnoldi.order = 0;  // Exercise DMP dispatch independently of reduction availability.
    if (family == 2) model->dmp.valid = false;
    CsrParasiticTree tree;
    tree.terminals = {NodeId{0}, NodeId{1}};
    tree.lanes.fill(model);
    auto alternate_nodes = nodes;
    alternate_nodes[1].capacitance_pf = 0.0723;
    auto alternate = std::make_shared<CsrParasiticModel>(buildCsrParasiticModel(alternate_nodes, terminals));
    if (family) alternate->arnoldi.order = 0;
    if (family == 2) alternate->dmp.valid = false;
    tree.lanes[1] = tree.lanes[2] = alternate;
    trees.push_back(std::move(tree));
  }
  PackedRcTree packed;
  std::vector<RcModel> models;
  std::vector<RcTerminal> terminal_data;
  std::vector<std::array<Index, 4>> lane_models;
  for (const auto& tree : trees) {
    packRcTree(tree, packed);
    demand(packed.models.size() == 2 && packed.terminals.size() == 4
        && packed.lanes[0] == packed.lanes[3] && packed.lanes[1] == packed.lanes[2],
        "shared RC lanes duplicated terminal payload");
    auto lanes = packed.lanes;
    for (auto& lane : lanes) lane += models.size();
    lane_models.push_back(lanes);
    for (auto model : packed.models) {
      model.terminal_begin += terminal_data.size();
      models.push_back(model);
    }
    terminal_data.insert(terminal_data.end(), packed.terminals.begin(), packed.terminals.end());
  }
  const auto* retained = packed.terminals.data();
  packRcTree(trees.front(), packed);
  demand(retained == packed.terminals.data(), "same-size RC repack lost capacity");
  auto malformed = trees.front();
  malformed.terminals.push_back(NodeId{2});
  bool rejected = false;
  try { packRcTree(malformed, packed); } catch (const std::invalid_argument&) { rejected = true; }
  demand(rejected, "malformed RC terminal extent accepted");

  ModelStorage library;
  const auto child = makeChild();
  auto children = std::make_shared<const std::vector<ChildTimingArcModel>>(std::vector{child});
  const auto first = library.intern(children);
  demand(first == library.intern(std::make_shared<const std::vector<ChildTimingArcModel>>(*children)),
      "equal Liberty values no longer reuse model storage");
  auto changed = child;
  changed.signature.library_name = "other.lib";
  changed.output_threshold_pct[0] = 0.3;
  const auto second = library.intern(std::make_shared<const std::vector<ChildTimingArcModel>>(std::vector{changed}));
  demand(library.children[first].library != library.children[second].library
         && library.children[second].output_threshold[0] == 0.3, "RC library/threshold packing lost identity");

  std::vector<Request> requests;
  for (unsigned i = 0; i < 1025; ++i) {
    const auto output = i / 6 % 2, analysis = i / 12 % 2;
    requests.push_back({lane_models[i % 3][analysis * 2 + output], i / 3 % 2 ? second : first,
        output, analysis, i % 3, 0.001 + i / 10000.0});
  }
  CsrCudaTelemetry telemetry;
  Runtime memory(telemetry);
  DeviceView view;
  view.child_count = library.children.size();
  view.children = memory.allocate<Child>(view.child_count);
  view.table_data = memory.allocate<double>(library.tables.size());
  auto* device_models = memory.allocate<RcModel>(models.size());
  auto* device_terminals = memory.allocate<RcTerminal>(terminal_data.size());
  auto* device_requests = memory.allocate<Request>(requests.size());
  auto* device_outputs = memory.allocate<Output>(requests.size());
  auto* device_records = memory.allocate<CsrRcTimingRecord>(requests.size() * 6);
  memory.upload(library.children, view.children);
  memory.upload(library.tables, view.table_data);
  memory.upload(models, device_models);
  memory.upload(terminal_data, device_terminals);
  memory.upload(requests, device_requests);
  std::vector<Output> first_results(requests.size()), results(requests.size());
  for (unsigned threads : {64, 256}) {
    // The host copies complete records, including padding the compiler does
    // not write. A nonzero sentinel keeps missing real-field writes visible.
    check(cudaMemsetAsync(device_outputs, 0xff, requests.size() * sizeof(Output), memory.stream()), "RC output padding");
    evaluatePacked<<<7, threads, 0, memory.stream()>>>(view, device_models, device_terminals,
        device_requests, device_outputs, device_records, requests.size());
    check(cudaGetLastError(), "RC packing kernel");
    memory.download(device_outputs, results);
    memory.synchronize();
    for (std::size_t i = 0; i < requests.size(); ++i) {
      const auto& result = results[i];
      const auto& request = requests[i];
      const auto expected_family = i % 3 == 0 ? CsrRcDriverFamily::arnoldi
          : i % 3 == 1 ? CsrRcDriverFamily::dmp : CsrRcDriverFamily::unavailable;
      demand(result.cache_ok && result.failure_ok && result.status == Status::success && result.family == expected_family
          && result.port_status == CsrRcEvaluationStatus::kSuccess
          && result.raw_status == CsrRcEvaluationStatus::kSuccess, "packed CUDA RC family/status failed");
      if constexpr (ista::incregpusta_debug_checks_enabled)
        compareCpu(trees[request.tree], request.child == second ? changed : child, request, result);
      if (threads == 256) {
        const auto& before = first_results[i];
        demand(result.delay == before.delay && result.slew == before.slew
            && result.wire == before.wire && result.load_slew == before.load_slew
            && result.port_wire == before.port_wire && result.port_slew == before.port_slew,
            "RC numeric result depends on launch shape");
      }
    }
    if (threads == 64) first_results = results;
  }
  check(memory.close(), "RC packing cleanup");
  std::cout << "CUDA packed RC tables/driver/load passed\n";
}
