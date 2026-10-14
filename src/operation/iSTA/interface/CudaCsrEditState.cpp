// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "CudaCsrEditState.hpp"
#include <stdexcept>
#include "Database.hpp"
#include "DualCsrBaselineFactory.hpp"
#include "DualCsrArcGroupResolverValidation.hpp"
#include "DualCsrArcGroupIndexValidation.hpp"
#include "DualCsrStateValidation.hpp"
#include "DualCsrSemanticIdentity.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"

namespace ista {
CudaCsrEditState::CudaCsrEditState(Database& database, const csr::CsrReferenceInput& input,
                                 timing_edit::TimingEditSessionToken session)
{
  using namespace timing_dual_csr;
  if (session.value() == 0 || input.source_identity.session_token != session.value()
      || input.source_identity.graph_generation != 0)
    throw std::invalid_argument("CUDA host edits require the session's generation-zero baseline");
  resolver = timing_edit::TimingEditResolverState::bindReferenceBaseline(database, input);
  auto initial = DualCsrBaselineFactory::fromNativeExport(input, session, resolver.highWater());
  if (!initial.success()) throw std::invalid_argument("CUDA host baseline is invalid");
  graph = std::move(*initial.state);
  auto indexed = DualCsrArcGroupIndex::fromNativeExport(graph);
  if (!indexed.success()) throw std::invalid_argument("CUDA host index is invalid");
  index = std::move(*indexed.index);
  auto initial_identity = timing_edit::NativeTimingEditPreparer::identityFromInput(
      database, input, timing_edit::TransactionGeneration{0});
  if constexpr (incregpusta_debug_checks_enabled)
    identity = initial_identity;
  baseline = std::make_shared<const timing_edit::BaselineSemanticIdentity>(
      timing_edit::BaselineSemanticIdentity{database.get_design_name(), std::move(initial_identity.current_fingerprints),
                                            std::move(initial_identity.canonical_pre_state)});
}

timing_dual_csr::PendingStructuredBatch CudaCsrEditState::prepareBatch(timing_edit::EditBatch edits)
{
  return timing_dual_csr::DualCsrStructuredBatchPreparer::prepareBound(
      std::move(edits), baseline, resolver, timing_dual_csr::DualSemanticReadView(graph), graph, identity, index);
}

CudaCsrEditState::Patch::Patch(CudaCsrEditState& owner, timing_dual_csr::VerifiedStructuredBatch batch)
    : _owner(owner), _resolver(owner.resolver, std::move(batch._resolver_delta)),
      _graph(owner.graph, batch.descriptor()), _index(owner.graph, owner.index, batch.descriptor())
{
  if (batch._source_graph != &owner.graph)
    throw std::invalid_argument("CUDA host patch belongs to another committed graph");
}
void CudaCsrEditState::Patch::apply()
{
  _resolver.apply();
  _graph.apply();
  _index.apply();
  if constexpr (incregpusta_debug_checks_enabled) {
    using namespace timing_dual_csr;
    if (!validateDualCsrState(_owner.graph).empty()
        || !validateDualCsrArcGroupIndex(_owner.graph, _owner.index).empty()
        || !validateDualCsrArcGroupResolverFull(_owner.index, _owner.resolver).empty())
      throw std::logic_error("CUDA host CSR proof failed");
    _identity = deriveDualCsrCommittedIdentity(_owner.graph, _owner.resolver);
  }
}
void CudaCsrEditState::Patch::commit() noexcept
{
  _index.commit();
  _graph.commit();
  _resolver.commit();
  _owner.identity.swap(_identity);
}
}  // namespace ista
