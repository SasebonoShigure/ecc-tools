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

  TimingEditResolverState result;
  for (std::size_t index = 0; index < snapshot->nodes.size(); ++index) {
    const auto& key = snapshot->nodes[index].key;
    if (!result._nodes.emplace(ExistingNodeRef{key}, NativeSlotBinding{AppendOnlySlot{index}, key.full_name, false}).second) {
      invalid("baseline NodeKey collision");
    }
  }
  result._high_water.node_slots = snapshot->nodes.size();

  std::uint64_t net_slot = 0;
  for (const auto& [name, net] : database.get_net_map()) {
    static_cast<void>(net);
    if (!result._nets.emplace(ExistingNetRef{name}, NativeSlotBinding{AppendOnlySlot{net_slot++}, name, false}).second) {
      invalid("baseline net-name collision");
    }
  }
  result._high_water.net_slots = net_slot;

  for (std::size_t index = 0; index < snapshot->arcs.size(); ++index) {
    if (!result._arcs.emplace(ExistingArcRef{snapshot->arcs[index].key}, NativeSlotBinding{AppendOnlySlot{index}, {}, false}).second) {
      invalid("baseline ArcKey collision");
    }
  }
  result._high_water.arc_slots = snapshot->arcs.size();
  return result;
}

TimingEditResolverState TimingEditResolverState::stage(const PreparedBatch& batch) const
{
  requireHighWater(_high_water, batch.slotPlan().before);
  TimingEditResolverState staged = *this;

  std::set<std::string> instance_names;
  std::set<std::string> net_names;
  std::set<std::string> pin_names;
  for (const auto& value : batch.tokenMaterializations()) {
    if (value.instance_name.empty() || value.output_net_name.empty() || value.input_pin_name.empty() || value.output_pin_name.empty()) {
      invalid("token materialization contains an empty native name");
    }
    if (!instance_names.insert(value.instance_name).second || !net_names.insert(value.output_net_name).second
        || !pin_names.insert(value.input_pin_name).second || !pin_names.insert(value.output_pin_name).second) {
      invalid("token materialization collides within the staged batch");
    }
    const auto [position, inserted] = staged._materializations.emplace(value.creation_token, value);
    if (!inserted && !(position->second == value)) {
      invalid("creation token was materialized differently in another generation");
    }
  }

  const auto& plan = batch.slotPlan();
  applyAssignments(staged._nodes, plan.nodes, plan.before.node_slots, plan.after.node_slots,
                   [&staged](const StableNodeRef& ref) { return staged.materializedNodeName(ref); });
  applyAssignments(staged._nets, plan.nets, plan.before.net_slots, plan.after.net_slots,
                   [&staged](const StableNetRef& ref) { return staged.materializedNetName(ref); });
  applyAssignments(staged._arcs, plan.arcs, plan.before.arc_slots, plan.after.arc_slots,
                   [](const StableArcRef&) { return std::string{}; });
  staged._high_water = plan.after;
  return staged;
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

std::string TimingEditResolverState::materializedNodeName(const StableNodeRef& ref) const
{
  if (const auto* existing = std::get_if<ExistingNodeRef>(&ref)) {
    return existing->baseline_key.full_name;
  }
  const auto& created = std::get<CreatedNodeRef>(ref);
  const auto& value = materialization(created.creation_token);
  return created.role == CreatedNodeRole::kInput ? value.input_pin_name : value.output_pin_name;
}

std::string TimingEditResolverState::materializedNetName(const StableNetRef& ref) const
{
  if (const auto* existing = std::get_if<ExistingNetRef>(&ref)) {
    return existing->baseline_name;
  }
  return materialization(std::get<CreatedNetRef>(ref).creation_token).output_net_name;
}

}  // namespace ista::timing_edit
