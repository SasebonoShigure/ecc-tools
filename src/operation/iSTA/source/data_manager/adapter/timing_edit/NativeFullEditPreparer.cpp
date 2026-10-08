// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "NativeFullEditPreparer.hpp"

#include <limits>
#include <stdexcept>
#include <utility>
#include "CsrReferenceTypes.hpp"
#include "Database.hpp"
#include "DataManager.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "NativeCandidateExecutor.hpp"
#include "NativeTimingEditPreparer.hpp"
#include "TimingEditValidation.hpp"

namespace ista::timing_edit {
namespace {
void require(bool value, const char* message)
{
  if (!value)
    throw std::invalid_argument(message);
}
AppendOnlySlot append(std::uint64_t& value)
{
  require(value < std::numeric_limits<std::uint64_t>::max(), "native full name capacity exhausted");
  return AppendOnlySlot{value++};
}
}
NativeFullEditState NativeFullEditPreparer::bindBaseline(
    Database& database, const csr::CsrReferenceInput& input, const BaselineSemanticIdentity& baseline)
{
  require(&database == &STADM.getDatabase(), "native full baseline is not canonical");
  NativeTimingEditPreparer::requireBaselineIdentity(database, input, baseline);
  TimingEditResolverState resolver;
  std::set<std::string> clock_nets;
  for (const auto& node : input.nodes) {
    resolver._nodes.emplace(ExistingNodeRef{node.key},
        NativeSlotBinding{append(resolver._high_water.node_slots), node.key.full_name, false});
    if (input.clock_owner_by_node.at(node.id.value()))
      clock_nets.insert(node.net_name);
  }
  for (const auto& [name, net] : database.get_net_map()) {
    static_cast<void>(net);
    resolver._nets.emplace(ExistingNetRef{name},
        NativeSlotBinding{append(resolver._high_water.net_slots), name, false});
  }
  return NativeFullEditState(database, TimingEditSessionToken{input.source_identity.session_token},
                             detail::nextNativeInstanceId(database), std::move(resolver), std::move(clock_nets));
}
NativeFullEditBatch NativeFullEditPreparer::prepare(
    Database& database, const NativeExecutionAuthority& authority, EditBatch edits, NativeFullEditState& state,
    std::map<std::string, ParasiticNet> prepared_rc)
{
  require(authority.readyForQuery() && authority.sessionToken() == state._session
              && state._database == &database && &database == &STADM.getDatabase(),
          "native full edit owner/session mismatch");
  require(edits.expected_generation == state._generation, "native full edit stale generation");
  require(state._generation.value() < std::numeric_limits<std::uint64_t>::max(), "native full generation exhausted");
  auto normalized = normalizeEditBatch(std::move(edits));
  if (const auto* errors = std::get_if<TimingEditContractViolations>(&normalized))
    throw std::invalid_argument(errors->front().field + ": " + errors->front().message);
  const auto& plan = std::get<EditBatch>(normalized);
  require(plan.edits.size() <= std::numeric_limits<std::uint64_t>::max() - state._next_instance,
          "native instance identifier capacity exhausted");
  AppendOnlySlotPlan slots;
  slots.before = state._resolver.highWater();
  slots.after = slots.before;
  std::vector<TokenMaterialization> names;
  names.reserve(plan.edits.size());
  for (const auto& edit : plan.edits) {
    const auto* rc = std::get_if<ParasiticRcInputs>(&edit.rc_inputs);
    require(!rc || rc->parasitic_policy == ParasiticEditPolicy::kPreRouteTree,
            "native full edit requires NoParasitic or prepared pre-route trees");
    require(!state._resolver.materializations().contains(edit.creation_token), "native full creation token already exists");
    names.push_back(defaultTokenMaterialization(edit.creation_token, edit.library_cell));
    slots.nodes.push_back({CreatedNodeRef{edit.creation_token, CreatedNodeRole::kInput}, append(slots.after.node_slots), false});
    slots.nodes.push_back({CreatedNodeRef{edit.creation_token, CreatedNodeRole::kOutput}, append(slots.after.node_slots), false});
    slots.nets.push_back({CreatedNetRef{edit.creation_token, CreatedNetRole::kOutputNet}, append(slots.after.net_slots), false});
  }
  PreparedTimingEditResolverPatch patch(state._resolver, state._resolver.prepareAssignmentsDelta(slots, names));
  for (const auto& edit : plan.edits) {
    const auto& net = patch.view().resolveNet(edit.original_net);
    require(!state._clock_nets.contains(net), "native full edit touches propagated clock net");
    if (database.get_net_map().contains(net))
      detail::requireDataOnlyNet(database, net);
    // New nets are checked against the preceding edits by raw preparation.
  }
  auto transaction = NativeEditTransaction::prepareFull(authority, plan.edits, patch.view(), state._next_instance, std::move(prepared_rc));
  return NativeFullEditBatch(state, TransactionGeneration{state._generation.value() + 1},
                            state._next_instance + plan.edits.size(), std::move(patch), std::move(transaction));
}
NativeFullEditBatch::NativeFullEditBatch(NativeFullEditState& state, TransactionGeneration target, std::uint64_t next_instance,
                                        PreparedTimingEditResolverPatch patch, NativeEditTransaction transaction)
    : _state(&state), _target(target), _next_instance(next_instance),
      _patch(std::move(patch)), _transaction(std::move(transaction)) {}
NativeFullEditState::NativeFullEditState(Database& database, TimingEditSessionToken session, std::uint64_t next_instance,
                                        TimingEditResolverState resolver, std::set<std::string> clock_nets)
    : _database(&database), _session(session), _next_instance(next_instance),
      _resolver(std::move(resolver)), _clock_nets(std::move(clock_nets)) {}
NativeFullEditBatch::NativeFullEditBatch(NativeFullEditBatch&& other) noexcept
    : _state(std::exchange(other._state, nullptr)), _target(other._target), _next_instance(other._next_instance),
      _patch(std::move(other._patch)), _transaction(std::move(other._transaction)) {}
bool NativeFullEditBatch::canCommit() const noexcept
{
  return _state && _transaction.active() && _state->_generation.value() + 1 == _target.value();
}
bool NativeFullEditBatch::commit() noexcept
{
  if (!canCommit())
    return false;
  _patch.apply();
  _patch.commit();
  _transaction.commit();
  _state->_generation = _target;
  _state->_next_instance = _next_instance;
  _state = nullptr;
  return true;
}
}  // namespace ista::timing_edit
