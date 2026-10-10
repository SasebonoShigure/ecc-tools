#include "CudaDualCsrInternal.cuh"
#include <algorithm>
#include <cmath>
#include "CsrCudaClockPropagation.cuh"
#include "CudaDualCsrTestReader.hpp"
#include "timing_edit/BufferTraceCsrJson.hpp"
namespace ista::csr {
#if ECC_INCREGPUSTA_DEBUG_CHECKS
namespace {
__global__ void invalidSemanticDeviceAddress()
{
  *static_cast<volatile int*>(nullptr) = 1;
}
}
void CudaDualCsrTestReader::corruptContext()
{
  invalidSemanticDeviceAddress<<<1, 1>>>();
  cuda_detail::demand(cudaGetLastError() == cudaSuccess, "semantic fault kernel did not launch");
  cuda_detail::demand(cudaDeviceSynchronize() == cudaErrorIllegalAddress, "semantic device fault did not synchronize");
}
#endif
CudaDualCsrSession CudaDualCsrTestReader::computeRc(const CsrReferenceInput& input, unsigned threads,
    std::optional<timing_edit::AppendOnlyHighWater> high_water)
{
  auto state = std::make_shared<CudaDualCsrSession::Impl>(
      input, high_water.value_or(timing_edit::AppendOnlyHighWater{input.nodes.size(), 0, input.arcs.size()}));
  cuda_detail::demand(!state->code && threads > 0 && threads <= 1024, "RC component initialization failed");
  state->storage->kernel_threads = threads;
  state->memory.zero(state->view.status, 1);
  state->memory.zero(state->view.level_offset_count, 1);
  state->execute();
  return CudaDualCsrSession(std::move(state));
}
CudaDualCsrSession CudaDualCsrTestReader::stageRc(CudaDualCsrSession& owner, const CsrReferenceInput& input)
{
  return owner.stageFullNativeExport(input, {input.nodes.size(), 0, input.arcs.size()});
}
CudaDualCsrSession CudaDualCsrTestReader::stageFullCsr(CudaDualCsrSession& owner, const timing_dual_csr::DualCsrState& input)
{
  return owner.stageFullCsr(input);
}
void CudaDualCsrTestReader::setKernelThreads(CudaDualCsrSession& session, unsigned threads)
{
  cuda_detail::demand(session._impl && !session._impl->storage->pending && threads > 0 && threads <= 1024,
                      "test launch shape needs an idle session and valid block size");
  session._impl->storage->kernel_threads = threads;
}
const CsrTimingValues& CudaDualCsrTestReader::materializeWithLevelOwnershipCheck(CudaDualCsrSession& session)
{
  using namespace cuda_detail;
  demand(session._impl && !session._impl->values, "level ownership check needs an unqueried generation");
  const auto& levels = session._impl->levels;
  const auto* offsets = levels.level_offsets.data();
  const auto* nodes = levels.level_nodes.data();
  if constexpr (!incregpusta_debug_checks_enabled)
    demand(levels.level_nodes.empty(), "unqueried OFF generation retained a host permutation");
  const auto* inverse = levels.node_to_level.data();
  const auto& result = session.timingValues();
  demand(offsets == result.level_offsets.data() && inverse == result.node_to_level.data(),
         "materialization copied functional level storage");
  if constexpr (incregpusta_debug_checks_enabled)
    demand(nodes == result.level_nodes.data(), "materialization copied diagnostic level order");
  demand(result.level_nodes.size() == result.node_to_level.size(), "query did not materialize the full level order");
  return result;
}
const std::vector<BufferCandidateTiming>& CudaDualCsrTestReader::readCandidateTiming(const CudaDualCsrSession& session)
{
  cuda_detail::demand(session._impl && !session._impl->values, "candidate check requires no full timing cache");
  const auto& result = session.candidateTiming();
  cuda_detail::demand(!session._impl->values, "candidate query materialized full timing");
  return result;
}
void CudaDualCsrTestReader::omitAuxAndRecompute(CudaDualCsrSession& candidate)
{
  using namespace cuda_detail;
  if constexpr (!incregpusta_debug_checks_enabled)
    throw std::logic_error("GPU omission canary requires development checks ON");
  demand(candidate._impl && candidate._impl->origin && !candidate._impl->poisoned, "GPU omission canary requires a detached healthy candidate");
  auto& state = *candidate._impl;
  const std::vector<AuxRow> omitted(state.view.node_count);
  try {
    state.memory.upload(omitted, state.view.aux_rows);
    state.memory.zero(state.view.status, 1);
    state.memory.zero(state.view.storage_visits, 12);
    state.values.reset();
    state.code.reset();
    state.execute();
    if (state.code)
      state.cancel();
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
nlohmann::json CudaDualCsrTestReader::capture(const CudaDualCsrSession& session)
{
  using namespace cuda_detail;
  demand(session._impl && !session._impl->poisoned && session._impl->levels.complete, "acceptance reader needs successful device state");
  auto& state = *session._impl;
  auto read = [&]<class T>(const T* source, Index count) {
    std::vector<T> result(count);
    try {
      state.memory.download(source, result);
      state.memory.synchronize();
      return result;
    } catch (...) {
      try {
        state.memory.synchronize();
      } catch (...) {
        state.storage->poisoned = true;
      }
      throw;
    }
  };
  try {
    auto out_offsets = read(state.view.out_offsets, state.host->out_offsets.size());
    auto in_offsets = read(state.view.in_offsets, state.host->in_offsets.size());
    nlohmann::json arcs = nlohmann::json::array();
    for (const auto& arc : read(state.view.arcs, state.view.arc_count))
      arcs.push_back({{"source", arc.source},
                      {"sink", arc.sink},
                      {"active", arc.active},
                      {"net", arc.net},
                      {"clock", arc.clock},
                      {"clock_eligible", arc.clock_eligible},
                      {"child_begin", arc.child_begin},
                      {"child_count", arc.child_count},
                      {"delay", arc.delay},
                      {"delay_min", arc.delay_min},
                      {"delay_max", arc.delay_max},
                      {"has_load", arc.has_load},
                      {"load", arc.load},
                      {"levelization", arc.levelization},
                      {"data_arrival", arc.data_arrival},
                      {"data_slew", arc.data_slew}});
    return {{"generation", state.generation.value()},
            {"out_offsets", out_offsets},
            {"out_arcs", read(state.view.out_arcs, out_offsets.back())},
            {"in_offsets", in_offsets},
            {"in_arcs", read(state.view.in_arcs, in_offsets.back())},
            {"aux_rows", state.readAuxRows()},
            {"roots", read(state.view.roots, state.view.root_count)},
            {"ends", read(state.view.ends, state.view.end_count)},
            {"arcs", arcs},
            {"timing", buffer_trace::test::csrTimingValuesToJson(session.timingValues())}};
  } catch (const CudaError&) {
    state.poisoned = true;
    state.storage->poisoned = true;
    if (state.origin)
      state.origin->poisoned = true;
    throw;
  }
}
namespace {
__global__ void clockContractKernel(cuda_detail::DeviceView view)
{
  for (cuda_detail::Index arc = 0; arc < view.arc_count; ++arc) {
    cuda_detail::initializeArc(view, arc);
    if (*view.status != cuda_detail::Status::success)
      return;
  }
  cuda_detail::clockPass(view);
}
}  // namespace

void runCudaClockContracts()
{
  using namespace cuda_detail;
  CsrCudaTelemetry telemetry;
  Runtime memory(telemetry);
  DeviceView view;
  view.node_count = 3;
  view.arc_count = 2;
  view.clock_count = 1;
  view.out_offsets = memory.allocate<Index>(4);
  view.out_arcs = memory.allocate<Index>(2);
  view.in_offsets = memory.allocate<Index>(4);
  view.in_arcs = memory.allocate<Index>(2);
  view.order = memory.allocate<Index>(3);
  view.nodes = memory.allocate<NodeState>(3);
  view.arcs = memory.allocate<Arc>(2);
  view.arc_state = memory.allocate<ArcState>(2);
  view.status = memory.allocate<Status>(1);
  const std::vector<Index> offsets{0, 2, 2, 2}, outgoing_arcs{0, 1}, order{0, 1, 2};
  memory.upload(offsets, view.out_offsets);
  memory.upload(outgoing_arcs, view.out_arcs);
  const std::vector<Index> incoming_offsets{0, 0, 1, 2};
  memory.upload(incoming_offsets, view.in_offsets);
  memory.upload(outgoing_arcs, view.in_arcs);
  memory.upload(order, view.order);
  memory.synchronize();
  std::vector<NodeState> seeds(3);
  std::vector<Arc> arcs(2);
  for (Index node = 0; node < 3; ++node) {
    seeds[node].clock_owner = 0;
    seeds[node].clock_point = true;
    seeds[node].propagated_clock = node != 1;
    for (unsigned lane = 0; lane < 4; ++lane) {
      seeds[node].slew[lane] = {7.0, true};
      if (node == 0) {
        seeds[node].physical_clock[lane].arrival = {0.0, true};
        seeds[node].physical_clock[lane].slew = {0.0, true};
      }
    }
  }
  for (Index arc = 0; arc < 2; ++arc) {
    arcs[arc].source = 0;
    arcs[arc].sink = arc + 1;
    arcs[arc].net = false;
    arcs[arc].has_load = true;
    arcs[arc].clock_eligible = true;
    arcs[arc].delay_max = 2.0 + arc;
    arcs[arc].delay_min = 1.0 + arc;
  }
  const auto run = [&]() {
    Status status = Status::success;
    memory.upload(seeds, view.nodes);
    memory.upload(arcs, view.arcs);
    memory.zero(view.arc_state, view.arc_count);
    memory.upload(&status, view.status, 1);
    clockContractKernel<<<1, 1, 0, memory.stream()>>>(view);
    check(cudaGetLastError(), "clock contract launch");
    memory.download(view.status, &status, 1);
    memory.synchronize();
    return status;
  };
  demand(run() == Status::success, "CUDA physical clock fanout failed");
  std::vector<NodeState> result(3);
  memory.download(view.nodes, result);
  memory.synchronize();
  for (Index node = 0; node < 3; ++node) {
    for (unsigned lane = 0; lane < 4; ++lane) {
      const auto& value = result[node].physical_clock[lane];
      const double expected = node == 0 ? 0.0 : (lane < 2 ? 1.0 + node : static_cast<double>(node));
      demand(value.arrival.present && value.arrival.value == expected && value.slew.present && value.slew.value == 0.0,
             "CUDA physical fanout lane differs");
      demand(value.predecessor == (node == 0 ? absent : 0) && value.predecessor_delay == expected
                 && value.predecessor_transition == (node == 0 ? 2 : lane % 2),
             "CUDA physical clock predecessor differs");
      demand(result[node].propagated_clock == seeds[node].propagated_clock && result[node].slew[lane].value == 7.0,
             "CUDA physical clock changed its mode or data timing");
    }
  }
  DownloadData downloaded;
  downloaded.nodes = result;
  PathDomain empty_paths;
  empty_paths.offsets.assign(result.size() * 8 + 1, 0);
  const auto published = unpackValues(std::move(downloaded), ResultLayout{}, empty_paths);
  for (Index node = 0; node < 3; ++node)
    for (unsigned lane = 0; lane < 4; ++lane) {
      const auto index = node * 4 + lane;
      const auto& clock = result[node].physical_clock[lane];
      const bool propagated = result[node].propagated_clock;
      demand(published.effective_clock.arrivals.presence[index] == 1
                 && published.effective_clock.arrivals.values[index] == (propagated ? clock.arrival.value : 0.0)
                 && published.effective_clock.slews.presence[index] == 1
                 && published.effective_clock.slews.values[index] == (propagated ? clock.slew.value : 0.0),
             "CUDA effective clock materialization lost mode-specific values");
      const bool predecessor = propagated && node != 0;
      demand(published.effective_clock.predecessor_nodes.presence[index] == predecessor
                 && published.effective_clock.predecessor_arc_delays.presence[index] == predecessor
                 && published.effective_clock.predecessor_transitions.presence[index] == predecessor
                 && (!predecessor || (published.effective_clock.predecessor_nodes.values[index] == NodeId{0}
                     && published.effective_clock.predecessor_arc_delays.values[index] == clock.predecessor_delay
                     && published.effective_clock.predecessor_transitions.values[index] == static_cast<Transition>(lane % 2))),
             "CUDA effective clock materialization lost predecessor identity");
    }
  seeds[2].clock_owner = 1;
  demand(run() == Status::invariant, "CUDA clock owner conflict was silently ignored");
  seeds[2].clock_owner = 0;
  arcs[0].delay_max = std::numeric_limits<double>::max();
  demand(run() == Status::numeric, "CUDA physical clock overflow was not rejected");
  arcs[0].delay_max = 2.0;
  demand(run() == Status::success, "CUDA healthy clock pass failed after rejection");
  view.node_count = 4;
  view.arc_count = 3;
  view.out_offsets = memory.allocate<Index>(5);
  view.out_arcs = memory.allocate<Index>(3);
  view.in_offsets = memory.allocate<Index>(5);
  view.in_arcs = memory.allocate<Index>(3);
  view.order = memory.allocate<Index>(4);
  view.nodes = memory.allocate<NodeState>(4);
  view.arcs = memory.allocate<Arc>(3);
  view.arc_state = memory.allocate<ArcState>(3);
  const std::vector<Index> chain_out{0, 1, 2, 3, 3}, chain_in{0, 0, 0, 0, 3}, chain_arcs{0, 1, 2};
  std::vector<Index> chain_order{0, 1, 2, 3};
  memory.upload(chain_out, view.out_offsets);
  memory.upload(chain_in, view.in_offsets);
  memory.upload(chain_arcs, view.out_arcs);
  memory.upload(chain_arcs, view.in_arcs);
  seeds.assign(4, {});
  arcs.assign(3, {});
  for (Index node = 0; node < 4; ++node) {
    seeds[node].clock_owner = 0;
    if (node == 3)
      continue;
    for (unsigned lane = 0; lane < 4; ++lane) {
      seeds[node].physical_clock[lane].arrival = {0.0, true};
      seeds[node].physical_clock[lane].slew = {0.0, true};
    }
    auto& arc = arcs[node];
    arc.source = node;
    arc.sink = 3;
    arc.net = false;
    arc.has_load = arc.clock_eligible = true;
    arc.delay = arc.delay_min = arc.delay_max = 0.1 + node * 0.75e-6;
  }
  result.resize(4);
  do {
    memory.upload(chain_order, view.order);
    demand(run() == Status::success, "CUDA clock epsilon-chain failed");
    memory.download(view.nodes, result);
    memory.synchronize();
    for (unsigned lane = 0; lane < 4; ++lane) {
      const auto& value = result[3].physical_clock[lane];
      const double delay = 0.1 + (lane < 2 ? 0.75e-6 : 0.0);
      demand(value.predecessor == (lane < 2 ? 1 : 0) && value.predecessor_delay == delay
                 && value.arrival.value == std::round(delay * 1e15) / 1e15,
             "CUDA clock epsilon value/identity depends on the source schedule");
    }
  } while (std::next_permutation(chain_order.begin(), chain_order.begin() + 3));

}
}  // namespace ista::csr
