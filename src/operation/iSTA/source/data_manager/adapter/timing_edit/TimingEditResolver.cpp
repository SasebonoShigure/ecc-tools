// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "TimingEditResolver.hpp"

#include <set>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <type_traits>

#include "Database.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista::timing_edit {
namespace {

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("timing edit resolver: " + std::move(message));
}

void requireHighWater(const AppendOnlyHighWater& actual, const AppendOnlyHighWater& expected)
{
  if (!(actual == expected)) {
    invalid("slot plan before-water does not match current resolver state");
  }
}

template <typename StableRef, typename NameResolver>
void applyAssignments(std::map<StableRef, NativeSlotBinding>& bindings, const std::vector<SlotAssignment<StableRef>>& assignments,
                      std::uint64_t before, std::uint64_t after, NameResolver&& resolve_name)
{
  if (after < before) {
    invalid("append-only high-water moved backwards");
  }

  std::set<std::uint64_t> new_slots;
  std::set<StableRef> assigned_refs;
  for (const auto& assignment : assignments) {
    if (!assigned_refs.insert(assignment.ref).second) {
      invalid("slot plan assigns the same stable reference more than once");
    }
    const std::uint64_t slot = assignment.slot.value();
    if (slot >= after) {
      invalid("slot assignment is at or above the target high-water");
    }

    const auto existing = bindings.find(assignment.ref);
    if (existing != bindings.end()) {
      if (existing->second.slot != assignment.slot) {
        invalid("stable reference changed append-only slot");
      }
      if (existing->second.tombstone && !assignment.tombstone) {
        invalid("slot plan attempts to resurrect a tombstone");
      }
      existing->second.tombstone = assignment.tombstone;
      continue;
    }

    if (slot < before || !new_slots.insert(slot).second) {
      invalid("new stable reference does not use a unique appended slot");
    }
    bindings.emplace(assignment.ref, NativeSlotBinding{assignment.slot, resolve_name(assignment.ref), assignment.tombstone});
  }

  if (new_slots.size() != after - before) {
    invalid("slot plan leaves a hole in the append-only range");
  }
  for (std::uint64_t slot = before; slot < after; ++slot) {
    if (!new_slots.contains(slot)) {
      invalid("slot plan does not cover every appended slot");
    }
  }
}

template <typename StableRef>
const NativeSlotBinding& requireLiveBinding(const std::map<StableRef, NativeSlotBinding>& bindings, const StableRef& ref,
                                            std::string_view kind)
{
  const auto binding = bindings.find(ref);
  if (binding == bindings.end()) {
    invalid(std::string(kind) + " reference is not bound");
  }
  if (binding->second.tombstone) {
    invalid(std::string(kind) + " reference resolves to a tombstone");
  }
  return binding->second;
}

template <typename StableRef>
StableRef reverseResolve(const std::map<StableRef, NativeSlotBinding>& bindings, const std::string& native_name, std::string_view kind)
{
  const StableRef* resolved = nullptr;
  for (const auto& [ref, binding] : bindings) {
    if (binding.tombstone || binding.native_name != native_name) {
      continue;
    }
    if (resolved != nullptr) {
      invalid(std::string(kind) + " native name has multiple live stable bindings");
    }
    resolved = &ref;
  }
  if (resolved == nullptr) {
    invalid(std::string(kind) + " native name is not bound");
  }
  return *resolved;
}

}  // namespace

TimingEditResolverState TimingEditResolverState::bindBaseline(const Database& database)
{
  const csr::CsrGraphExportResult exported = csr::exportCsrGraphSnapshot(database);
  const auto* snapshot = std::get_if<csr::CsrGraphSnapshot>(&exported);
  if (snapshot == nullptr) {
    invalid("cannot bind an invalid baseline CSR graph");
  }

  return bindRecords(database, *snapshot);
}

TimingEditResolverState TimingEditResolverState::bindReferenceBaseline(const Database& database, const csr::CsrReferenceInput& input)
{
  return bindRecords(database, input);
}

template <typename CanonicalRecords>
TimingEditResolverState TimingEditResolverState::bindRecords(const Database& database, const CanonicalRecords& input)
{
  TimingEditResolverState result;
  for (std::size_t index = 0; index < input.nodes.size(); ++index) {
    const auto& key = input.nodes[index].key;
    if (!result._nodes.emplace(ExistingNodeRef{key}, NativeSlotBinding{AppendOnlySlot{index}, key.full_name, false}).second) {
      invalid("baseline NodeKey collision");
    }
  }
  result._high_water.node_slots = input.nodes.size();

  std::uint64_t net_slot = 0;
  for (const auto& [name, net] : database.get_net_map()) {
    static_cast<void>(net);
    if (!result._nets.emplace(ExistingNetRef{name}, NativeSlotBinding{AppendOnlySlot{net_slot++}, name, false}).second) {
      invalid("baseline net-name collision");
    }
  }
  result._high_water.net_slots = net_slot;

  for (std::size_t index = 0; index < input.arcs.size(); ++index) {
    if (!result._arcs.emplace(ExistingArcRef{input.arcs[index].key}, NativeSlotBinding{AppendOnlySlot{index}, {}, false}).second) {
      invalid("baseline ArcKey collision");
    }
  }
  result._high_water.arc_slots = input.arcs.size();
  return result;
}

TimingEditResolverState TimingEditResolverState::stage(const PreparedBatch& batch) const
{
  return stageAssignments(batch.slotPlan(), batch.tokenMaterializations());
}

TimingEditResolverDelta TimingEditResolverState::prepareAssignmentsDelta(
    const AppendOnlySlotPlan& plan, const std::vector<TokenMaterialization>& values) const
{
  requireHighWater(_high_water, plan.before);
  TimingEditResolverDelta result;
  auto& delta = result._changes;
  auto& expected = result._expected;
  expected._high_water = plan.before;
  std::set<std::string> instance_names, net_names, pin_names;
  for (const auto& value : values) {
    if (value.instance_name.empty() || value.output_net_name.empty() || value.input_pin_name.empty() || value.output_pin_name.empty())
      invalid("token materialization contains an empty native name");
    if (!instance_names.insert(value.instance_name).second || !net_names.insert(value.output_net_name).second
        || !pin_names.insert(value.input_pin_name).second || !pin_names.insert(value.output_pin_name).second)
      invalid("token materialization collides within the staged batch");
    const auto previous = _materializations.find(value.creation_token);
    if (previous != _materializations.end() && previous->second != value)
      invalid("creation token was materialized differently in another generation");
    if (previous != _materializations.end())
      expected._materializations.emplace(previous->first, previous->second);
    if (!delta._materializations.emplace(value.creation_token, value).second)
      invalid("creation token occurs more than once in a batch");
  }
  const auto copy_touched = [](const auto& before, const auto& assignments, auto& changed) {
    for (const auto& assignment : assignments) {
      const auto found = before.find(assignment.ref);
      if (found != before.end())
        changed.emplace(found->first, found->second);
    }
  };
  copy_touched(_nodes, plan.nodes, delta._nodes);
  copy_touched(_nets, plan.nets, delta._nets);
  copy_touched(_arcs, plan.arcs, delta._arcs);
  expected._nodes = delta._nodes;
  expected._nets = delta._nets;
  expected._arcs = delta._arcs;
  const auto materialization_for = [&](const CreationToken& token) -> const TokenMaterialization& {
    const auto found = delta._materializations.find(token);
    return found == delta._materializations.end() ? materialization(token) : found->second;
  };
  applyAssignments(delta._nodes, plan.nodes, plan.before.node_slots, plan.after.node_slots,
                   [&](const StableNodeRef& ref) {
                     if (const auto* existing = std::get_if<ExistingNodeRef>(&ref))
                       return existing->baseline_key.full_name;
                     const auto& created = std::get<CreatedNodeRef>(ref);
                     const auto& value = materialization_for(created.creation_token);
                     return created.role == CreatedNodeRole::kInput ? value.input_pin_name : value.output_pin_name;
                   });
  applyAssignments(delta._nets, plan.nets, plan.before.net_slots, plan.after.net_slots,
                   [&](const StableNetRef& ref) {
                     if (const auto* existing = std::get_if<ExistingNetRef>(&ref))
                       return existing->baseline_name;
                     return materialization_for(std::get<CreatedNetRef>(ref).creation_token).output_net_name;
                   });
  applyAssignments(delta._arcs, plan.arcs, plan.before.arc_slots, plan.after.arc_slots,
                   [](const StableArcRef&) { return std::string{}; });
  delta._high_water = plan.after;
  return result;
}

TimingEditResolverState TimingEditResolverState::stageAssignments(
    const AppendOnlySlotPlan& plan, const std::vector<TokenMaterialization>& values) const
{
  auto staged = *this;
  PreparedTimingEditResolverPatch patch(staged, prepareAssignmentsDelta(plan, values));
  patch.apply();
  patch.commit();
  return staged;
}

TimingEditResolverDelta TimingEditResolverState::prepare(const PreparedBatch& batch) const
{
  return prepareAssignmentsDelta(batch.slotPlan(), batch.tokenMaterializations());
}

const std::string& TimingEditResolverState::resolveNode(const StableNodeRef& ref) const
{
  return requireLiveBinding(_nodes, ref, "node").native_name;
}

const std::string& TimingEditResolverState::resolveNet(const StableNetRef& ref) const
{
  return requireLiveBinding(_nets, ref, "net").native_name;
}

AppendOnlySlot TimingEditResolverState::resolveArcSlot(const StableArcRef& ref) const
{
  return requireLiveBinding(_arcs, ref, "arc").slot;
}

const TokenMaterialization& TimingEditResolverState::materialization(const CreationToken& token_value) const
{
  const auto value = _materializations.find(token_value);
  if (value == _materializations.end()) {
    invalid("creation token has no native materialization");
  }
  return value->second;
}

StableNodeRef TimingEditResolverState::stableNodeForNativeName(const std::string& native_name) const
{
  return reverseResolve(_nodes, native_name, "node");
}

StableNetRef TimingEditResolverState::stableNetForNativeName(const std::string& native_name) const
{
  return reverseResolve(_nets, native_name, "net");
}


namespace {
template <class Map, class Key>
const typename Map::mapped_type* findValue(const Map& values, const Key& key)
{
  const auto found = values.find(key);
  return found == values.end() ? nullptr : &found->second;
}
const NativeSlotBinding& requireLive(const NativeSlotBinding* binding, const char* kind)
{
  if (!binding || binding->tombstone)
    invalid(std::string(kind) + " reference is missing or tombstoned");
  return *binding;
}
}  // namespace

const NativeSlotBinding* TimingEditResolverView::findNodeBinding(const StableNodeRef& ref) const
{
  if (_delta)
    if (const auto* value = findValue(_delta->_changes._nodes, ref))
      return value;
  return findValue(_base._nodes, ref);
}
const NativeSlotBinding* TimingEditResolverView::findNodeBindingForKey(const csr::NodeKey& key) const
{
  if (const auto* baseline = findNodeBinding(ExistingNodeRef{key}))
    return baseline;
  // A committed peer can originate in an earlier edit. Its stable identity is
  // still the creation token, even when it is outside this batch's capture plan.
  // ponytail: linear in created history; add a reverse index only if profiling
  // shows this uncommon peer lookup dominating capture.
  const auto created = [&](const auto& values) -> const NativeSlotBinding* {
    for (const auto& [token, names] : values) {
      if (names.input_pin_name == key.full_name)
        return findNodeBinding(CreatedNodeRef{token, CreatedNodeRole::kInput});
      if (names.output_pin_name == key.full_name)
        return findNodeBinding(CreatedNodeRef{token, CreatedNodeRole::kOutput});
    }
    return nullptr;
  };
  if (_delta)
    if (const auto* binding = created(_delta->_changes._materializations))
      return binding;
  return created(_base._materializations);
}
const NativeSlotBinding* TimingEditResolverView::findNetBinding(const StableNetRef& ref) const
{
  if (_delta)
    if (const auto* value = findValue(_delta->_changes._nets, ref))
      return value;
  return findValue(_base._nets, ref);
}
const NativeSlotBinding* TimingEditResolverView::findArcBinding(const StableArcRef& ref) const
{
  if (_delta)
    if (const auto* value = findValue(_delta->_changes._arcs, ref))
      return value;
  return findValue(_base._arcs, ref);
}
const std::string& TimingEditResolverView::resolveNode(const StableNodeRef& ref) const
{
  return requireLive(findNodeBinding(ref), "node").native_name;
}
const std::string& TimingEditResolverView::resolveNet(const StableNetRef& ref) const
{
  return requireLive(findNetBinding(ref), "net").native_name;
}
AppendOnlySlot TimingEditResolverView::resolveArcSlot(const StableArcRef& ref) const
{
  return requireLive(findArcBinding(ref), "arc").slot;
}
const TokenMaterialization* TimingEditResolverView::findMaterialization(const CreationToken& token) const
{
  if (_delta)
    if (const auto* value = findValue(_delta->_changes._materializations, token))
      return value;
  return findValue(_base._materializations, token);
}
const TokenMaterialization& TimingEditResolverView::materialization(const CreationToken& token) const
{
  const auto* value = findMaterialization(token);
  if (!value)
    invalid("creation token has no native materialization");
  return *value;
}

struct PreparedTimingEditResolverPatch::Data
{
  TimingEditResolverState& state;
  TimingEditResolverDelta delta;
  std::vector<std::pair<NativeSlotBinding*, NativeSlotBinding*>> bindings;
  std::vector<std::map<StableNodeRef, NativeSlotBinding>::iterator> nodes;
  std::vector<std::map<StableNetRef, NativeSlotBinding>::iterator> nets;
  std::vector<std::map<StableArcRef, NativeSlotBinding>::iterator> arcs;
  std::vector<std::map<CreationToken, TokenMaterialization>::iterator> materializations;
  enum class Phase { kPrepared, kApplied, kFinished } phase{Phase::kPrepared};

  Data(TimingEditResolverState& owner, TimingEditResolverDelta changes) : state(owner), delta(std::move(changes))
  {
    requireHighWater(state._high_water, delta._expected._high_water);
    const auto collect = [&](auto& target, auto& replacement, const auto& expected, auto& added) {
      added.reserve(replacement.size());
      for (auto it = replacement.begin(); it != replacement.end(); ++it) {
        const auto old = target.find(it->first);
        const auto before = expected.find(it->first);
        if ((old == target.end()) != (before == expected.end())
            || (old != target.end() && old->second != before->second))
          invalid("pending resolver delta no longer matches the local old value");
        if (old == target.end())
          added.push_back(it);
        else
          bindings.emplace_back(&old->second, &it->second);
      }
    };
    collect(state._nodes, delta._changes._nodes, delta._expected._nodes, nodes);
    collect(state._nets, delta._changes._nets, delta._expected._nets, nets);
    collect(state._arcs, delta._changes._arcs, delta._expected._arcs, arcs);
    materializations.reserve(delta._changes._materializations.size());
    for (auto it = delta._changes._materializations.begin(); it != delta._changes._materializations.end(); ++it) {
      const auto old = state._materializations.find(it->first);
      const auto before = delta._expected._materializations.find(it->first);
      if ((old == state._materializations.end()) != (before == delta._expected._materializations.end())
          || (old != state._materializations.end() && old->second != before->second))
        invalid("pending token materialization no longer matches its old value");
      if (old == state._materializations.end())
        materializations.push_back(it);
    }
  }
  void apply() noexcept
  {
    if (phase != Phase::kPrepared)
      return;
    static_assert(std::is_nothrow_swappable_v<NativeSlotBinding>);
    for (const auto& [target, replacement] : bindings)
      std::swap(*target, *replacement);
    state._nodes.merge(delta._changes._nodes);
    state._nets.merge(delta._changes._nets);
    state._arcs.merge(delta._changes._arcs);
    state._materializations.merge(delta._changes._materializations);
    std::swap(state._high_water, delta._changes._high_water);
    phase = Phase::kApplied;
  }
  void rollback() noexcept
  {
    if (phase != Phase::kApplied)
      return;
    const auto erase = [](auto& target, const auto& inserted) {
      for (auto it : inserted)
        target.erase(it);
    };
    erase(state._nodes, nodes);
    erase(state._nets, nets);
    erase(state._arcs, arcs);
    erase(state._materializations, materializations);
    for (const auto& [target, saved] : bindings)
      std::swap(*target, *saved);
    std::swap(state._high_water, delta._changes._high_water);
    phase = Phase::kFinished;
  }
};
PreparedTimingEditResolverPatch::PreparedTimingEditResolverPatch(TimingEditResolverState& state, TimingEditResolverDelta delta)
    : _data(std::make_unique<Data>(state, std::move(delta))) {}
PreparedTimingEditResolverPatch::PreparedTimingEditResolverPatch(PreparedTimingEditResolverPatch&&) noexcept = default;
PreparedTimingEditResolverPatch::~PreparedTimingEditResolverPatch() { rollback(); }
TimingEditResolverView PreparedTimingEditResolverPatch::view() const
{
  if (!_data || _data->phase != Data::Phase::kPrepared)
    throw std::logic_error("resolver projection is only available before apply");
  return {_data->state, _data->delta};
}
void PreparedTimingEditResolverPatch::apply() noexcept { if (_data) _data->apply(); }
void PreparedTimingEditResolverPatch::rollback() noexcept { if (_data) _data->rollback(); }
void PreparedTimingEditResolverPatch::commit() noexcept { if (_data) _data->phase = Data::Phase::kFinished; }

}  // namespace ista::timing_edit
