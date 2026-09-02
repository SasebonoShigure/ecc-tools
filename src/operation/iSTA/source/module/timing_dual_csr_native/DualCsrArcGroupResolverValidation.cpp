// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrArcGroupResolverValidation.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ista::timing_dual_csr {
namespace {

void reject(DualCsrViolations& errors, DualCsrViolationCode code,
            std::string field, std::string message)
{
  errors.push_back({code, std::move(field), std::move(message)});
}

}  // namespace

DualCsrArcGroupBindingAttempt captureFullDualCsrArcGroupBindings(
    const timing_edit::TimingEditResolverState& resolver)
{
  DualCsrArcGroupBindingAttempt attempt;
  attempt.bindings.reserve(resolver.arcBindings().size());
  for (const auto& [ref, binding] : resolver.arcBindings())
    attempt.bindings.push_back(
        {csr::ArcId{binding.slot.value()}, ref});
  std::ranges::sort(attempt.bindings, [](const auto& lhs,
                                         const auto& rhs) {
    return lhs.id.value() < rhs.id.value();
  });
  for (std::size_t index = 0; index < attempt.bindings.size(); ++index)
    if (attempt.bindings[index].id.value() != index)
      reject(attempt.violations, DualCsrViolationCode::kId,
             "arc_group_resolver.bindings",
             "resolver does not cover contiguous logical ArcIds");
  if (attempt.bindings.size() != resolver.highWater().arc_slots)
    reject(attempt.violations, DualCsrViolationCode::kHighWater,
           "arc_group_resolver.bindings",
           "resolver binding count differs from ArcId high-water");
  return attempt;
}

DualCsrViolations validateDualCsrArcGroupResolverFull(
    const DualCsrArcGroupIndex& index,
    const timing_edit::TimingEditResolverState& resolver)
{
  DualCsrViolations errors;
  if (resolver.highWater().arc_slots != index.arc_high_water
      || resolver.arcBindings().size() != index.slots.size())
    reject(errors, DualCsrViolationCode::kHighWater,
           "arc_group_resolver.full",
           "resolver and index ArcId coverage differ");
  for (const auto& [ref, binding] : resolver.arcBindings()) {
    const auto id = binding.slot.value();
    if (id >= index.slots.size()) {
      reject(errors, DualCsrViolationCode::kId,
             "arc_group_resolver.full.slot",
             "resolver slot is outside index");
      continue;
    }
    const auto& indexed = index.slots[id];
    if (indexed.ref != ref
        || binding.tombstone == indexed.current_active_key.has_value())
      reject(errors, DualCsrViolationCode::kExpectedValue,
             "arc_group_resolver.full.binding",
             "resolver ref/activity differs from index slot");
  }
  return errors;
}

DualCsrViolations validateDualCsrArcGroupResolverTransition(
    const DualCsrArcGroupIndex& committed,
    const DualCsrArcGroupIndex& staged,
    const DualCsrBatchDescriptor& descriptor,
    const timing_edit::TimingEditResolverState& staged_resolver)
{
  DualCsrViolations errors;
  if (committed.session_token != descriptor.session_token
      || staged.session_token != descriptor.session_token
      || committed.generation != descriptor.expected_generation
      || staged.generation != descriptor.target_generation
      || committed.arc_high_water != descriptor.before.arc_slots
      || staged.arc_high_water != descriptor.after.arc_slots
      || staged_resolver.highWater().arc_slots
             != descriptor.after.arc_slots)
    reject(errors, DualCsrViolationCode::kGeneration,
           "arc_group_resolver.transition",
           "descriptor/index/resolver boundary differs");

  if (descriptor.after.arc_slots < descriptor.before.arc_slots) {
    reject(errors, DualCsrViolationCode::kHighWater,
           "arc_group_resolver.transition.append",
           "descriptor ArcId high-water regresses");
    return errors;
  }
  std::vector<bool> appended(descriptor.after.arc_slots
                                 - descriptor.before.arc_slots,
                             false);
  for (const auto& delta : descriptor.arc_group_index_delta) {
    const auto id = delta.id.value();
    const auto found = staged_resolver.arcBindings().find(delta.ref);
    if (found == staged_resolver.arcBindings().end()
        || found->second.slot.value() != id
        || found->second.tombstone
               == delta.replacement_active_key.has_value())
      reject(errors, DualCsrViolationCode::kExpectedValue,
             "arc_group_resolver.transition.binding",
             "changed StableArcRef does not bind the staged ArcId/activity");
    if (id >= staged.slots.size()
        || staged.slots[id].ref != delta.ref
        || staged.slots[id].current_active_key
               != delta.replacement_active_key)
      reject(errors, DualCsrViolationCode::kExpectedValue,
             "arc_group_resolver.transition.index",
             "changed index slot differs from typed delta");
    if (id < descriptor.before.arc_slots) {
      if (id >= committed.slots.size()
          || committed.slots[id].ref != delta.ref
          || committed.slots[id].current_active_key
                 != delta.expected_active_key)
        reject(errors, DualCsrViolationCode::kExpectedValue,
               "arc_group_resolver.transition.expected",
               "changed existing slot differs from exact-before index");
    } else if (id < descriptor.after.arc_slots) {
      if (delta.expected_active_key
          || appended[id - descriptor.before.arc_slots])
        reject(errors, DualCsrViolationCode::kConflict,
               "arc_group_resolver.transition.append",
               "appended ArcId transition is duplicated or has a before key");
      appended[id - descriptor.before.arc_slots] = true;
    }
  }
  if (!std::ranges::all_of(appended, [](bool value) { return value; }))
    reject(errors, DualCsrViolationCode::kShape,
           "arc_group_resolver.transition.append",
           "typed delta does not cover the complete appended ArcId range");
  return errors;
}

}  // namespace ista::timing_dual_csr
