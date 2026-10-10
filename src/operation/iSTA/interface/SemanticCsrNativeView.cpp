// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "SemanticCsrNativeView.hpp"
#include <stdexcept>
#include <vector>
#include "DataManager.hpp"
#include "DualCsrState.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeEditTransaction.hpp"
#include "TimingEditResolver.hpp"
#include "TimingResultScatterInternal.hpp"

namespace ista {
void SemanticCsrNativeView::materialize(Database& database, const timing_edit::NativeExecutionAuthority& authority,
    const timing_dual_csr::DualCsrState& graph, const timing_edit::TimingEditResolverState& resolver,
    std::span<const timing_edit::EditBatch> committed_batches, const csr::CsrTimingValues& values)
{
  using namespace timing_edit;
  if (!authority.readyForQuery() || authority.sessionToken() != _session || graph.session_token != _session
      || &database != &STADM.getDatabase() || graph.generation < _generation
      || committed_batches.size() != graph.generation.value())
    throw std::invalid_argument("semantic native view lost its committed owner or generation");
  if (_generation == graph.generation) {
    auto result = csr::detail::prepareDualCsrTimingResult(database, graph, values);
    csr::detail::publishPreparedTimingResult(result);
    return;
  }
  // One raw transaction covers the entire unmaterialized suffix, including a failed final build.
  std::vector<BufferInsertionMutation> edits;
  for (std::size_t index = _generation.value(); index < committed_batches.size(); ++index) {
    const auto& batch = committed_batches[index];
    if (batch.expected_generation.value() != index)
      throw std::invalid_argument("semantic native view requires contiguous committed batches");
    for (const auto& edit : batch.edits) {
      if (!std::holds_alternative<UnplacedInputs>(edit.placement_inputs)
          || !std::holds_alternative<NoParasiticRcInputs>(edit.rc_inputs))
        throw std::invalid_argument("semantic native view supports unplaced NoParasitic traces");
      edits.push_back(edit);
    }
  }
  auto transaction = NativeEditTransaction::prepareFull(
      authority, edits, TimingEditResolverView(resolver), timing_edit::detail::nextNativeInstanceId(database));
  NativeCandidateExecutor::prepareFullGraph(transaction);
  auto result = csr::detail::prepareDualCsrTimingResult(database, graph, values);
  csr::detail::publishPreparedTimingResult(result);
  transaction.commit();
  _generation = graph.generation;
}
}  // namespace ista
