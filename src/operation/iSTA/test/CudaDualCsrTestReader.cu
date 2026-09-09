#include "CudaDualCsrInternal.cuh"
#include "CudaDualCsrTestReader.hpp"
#include "timing_edit/BufferTraceCsrJson.hpp"
namespace ista::csr {
void CudaDualCsrTestReader::omitAuxAndRecompute(CudaDualCsrSession& candidate)
{
  using namespace cuda_detail;
  if constexpr (!incregpusta_debug_checks_enabled)
    throw std::logic_error("GPU omission canary requires development checks ON");
  demand(candidate._impl && candidate._impl->origin && !candidate._impl->poisoned, "GPU omission canary requires a detached healthy candidate");
  auto& state = *candidate._impl;
  try {
    const std::vector<Index> omitted(state.aux_mapping.size(), absent);
    state.memory.upload(omitted, state.view.source_to_aux_row);
    state.memory.zero(state.view.status, 1);
    state.memory.zero(state.view.storage_visits, 8);
    state.values.reset();
    state.code.reset();
    state.execute();
  } catch (const CudaError&) {
    state.poisoned = true;
    state.origin->poisoned = true;
    throw;
  }
}
nlohmann::json CudaDualCsrTestReader::capture(const CudaDualCsrSession& session)
{
  using namespace cuda_detail;
  demand(session._impl && !session._impl->poisoned && session._impl->values, "acceptance reader needs successful device state");
  auto& state = *session._impl;
  auto read = [&]<class T>(const T* source, Index count) {
    std::vector<T> result(count);
    state.memory.download(source, result);
    state.memory.synchronize();
    return result;
  };
  try {
    auto out_offsets = read(state.view.out_offsets, state.out_offsets.size());
    auto in_offsets = read(state.view.in_offsets, state.in_offsets.size());
    auto aux_offsets = read(state.view.aux_offsets, state.aux_offsets.size());
    nlohmann::json arcs = nlohmann::json::array();
    for (const auto& arc : read(state.view.arcs, state.view.arc_count))
      arcs.push_back({{"source", arc.source},
                      {"sink", arc.sink},
                      {"active", arc.active},
                      {"net", arc.net},
                      {"clock", arc.clock},
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
            {"aux_offsets", aux_offsets},
            {"aux_arcs", read(state.view.aux_arcs, aux_offsets.back())},
            {"source_to_aux_row", read(state.view.source_to_aux_row, state.aux_mapping.size())},
            {"roots", read(state.view.roots, state.view.root_count)},
            {"ends", read(state.view.ends, state.view.end_count)},
            {"arcs", arcs},
            {"timing", buffer_trace::test::csrTimingValuesToJson(session.timingValues())}};
  } catch (const CudaError&) {
    state.poisoned = true;
    if (state.origin)
      state.origin->poisoned = true;
    throw;
  }
}
}  // namespace ista::csr
