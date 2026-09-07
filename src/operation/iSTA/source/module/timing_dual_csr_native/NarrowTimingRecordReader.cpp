#include "NarrowTimingRecordReader.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <variant>

#include "IncreGpuStaBuildConfig.hpp"
#include "NarrowTimingCaptureInternal.hpp"
#include "NativeDualCsrPayloadCaptureInternal.hpp"
#include "StableArcEndpointRefsInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr {
namespace {

detail::NativeCaptureResult reject(const AffectedCapturePlan& plan,
                                   NarrowCaptureError error)
{
  if constexpr (!incregpusta_debug_checks_enabled)
    return {std::nullopt, error, {}};
  detail::NativeCaptureResult rejected;
  rejected.audit.requested_nodes = plan.nodes.size();
  rejected.audit.requested_instances
      = plan.token_materializations.size();
  rejected.audit.requested_arcs = std::ranges::count_if(
      plan.arcs, [](const auto& request) {
        return request.expected_post_multiplicity != 0;
      });
  rejected.audit.requested_deactivations = std::ranges::count_if(
      plan.arcs, [](const auto& request) {
        return request.expected_post_multiplicity == 0;
      });
  rejected.audit.requested_load_nets
      = plan.load_nets.size();
  rejected.audit.requested_node_roles = plan.node_roles.size();
  rejected.audit.requested_cell_pin_capacitances
      = plan.cell_pin_capacitances.size();
  rejected.audit.rejected_reads = 1;
  rejected.error = error;
  return rejected;
}

bool exactBatchBinding(const timing_edit::PreparedBatch& batch,
                       const AffectedCapturePlan& plan)
{
  return plan.session_token == batch.sessionToken()
         && plan.expected_generation == batch.expectedGeneration()
         && plan.target_generation == batch.targetGeneration()
         && plan.before == batch.slotPlan().before
         && plan.after == batch.slotPlan().after
         && plan.normalized_edits == batch.normalizedEdits()
         && plan.normalized_forward == batch.forwardDelta()
         && plan.normalized_reverse == batch.reverseDelta()
         && plan.slot_plan == batch.slotPlan()
         && plan.token_materializations == batch.tokenMaterializations()
         && plan.invalidation_closure == batch.invalidationClosure()
         && plan.policy_receipt == batch.policyReceipt();
}

bool validHighWater(const AffectedCapturePlan& plan)
{
  return plan.after.node_slots >= plan.before.node_slots
         && plan.after.net_slots >= plan.before.net_slots
         && plan.after.arc_slots >= plan.before.arc_slots
         && plan.slot_plan.before == plan.before
         && plan.slot_plan.after == plan.after;
}

bool targetedNoParasitic(const AffectedCapturePlan& plan)
{
  if (plan.policy_receipt.disposition
          != timing_edit::EditDisposition::kSupported
      || !plan.policy_receipt.reasons.empty()
      || plan.normalized_edits.empty())
    return false;
  return std::ranges::all_of(
      plan.normalized_edits, [](const auto& edit) {
        return std::holds_alternative<timing_edit::NoParasiticRcInputs>(
            edit.rc_inputs);
      });
}

template <typename Ref>
bool assignmentMatchesResolver(
    const std::map<Ref, timing_edit::NativeSlotBinding>& bindings,
    const timing_edit::SlotAssignment<Ref>& assignment)
{
  const auto found = bindings.find(assignment.ref);
  return found != bindings.end()
         && found->second.slot == assignment.slot
         && found->second.tombstone == assignment.tombstone;
}

bool exactResolverBinding(
    const timing_edit::TimingEditResolverState& staged,
    const AffectedCapturePlan& plan)
{
  if (staged.highWater() != plan.after)
    return false;
  for (const auto& assignment : plan.slot_plan.nodes)
    if (!assignmentMatchesResolver(staged.nodeBindings(), assignment))
      return false;
  for (const auto& assignment : plan.slot_plan.nets)
    if (!assignmentMatchesResolver(staged.netBindings(), assignment))
      return false;
  for (const auto& assignment : plan.slot_plan.arcs)
    if (!assignmentMatchesResolver(staged.arcBindings(), assignment))
      return false;
  for (const auto& item : plan.token_materializations) {
    const auto found = staged.materializations().find(item.creation_token);
    if (found == staged.materializations().end() || found->second != item)
      return false;
  }
  for (const auto& request : plan.nodes) {
    const auto found = staged.nodeBindings().find(request.ref);
    if (found == staged.nodeBindings().end() || found->second.tombstone
        || found->second.native_name != request.expected_key.full_name)
      return false;
  }
  for (const auto& request : plan.load_nets) {
    const auto found = staged.netBindings().find(request.ref);
    if (found == staged.netBindings().end() || found->second.tombstone
        || found->second.native_name != request.expected_name)
      return false;
  }
  for (const auto& request : plan.arcs) {
    const auto found = staged.arcBindings().find(request.ref);
    if (found == staged.arcBindings().end()
        || found->second.slot.value() >= plan.after.arc_slots
        || found->second.tombstone
               != (request.expected_post_multiplicity == 0))
      return false;
  }
  return true;
}

std::optional<NarrowCaptureError> validateNodeRequests(
    const AffectedCapturePlan& plan)
{
  std::vector<timing_edit::StableNodeRef> refs;
  std::vector<csr::NodeKey> keys;
  refs.reserve(plan.nodes.size());
  keys.reserve(plan.nodes.size());
  for (const auto& request : plan.nodes) {
    if (std::ranges::find(refs, request.ref) != refs.end()
        || std::ranges::find(keys, request.expected_key) != keys.end())
      return NarrowCaptureError::kDuplicateRequest;
    refs.push_back(request.ref);
    keys.push_back(request.expected_key);
  }
  for (const auto& assignment : plan.slot_plan.nodes) {
    const auto matches = std::ranges::count_if(
        plan.nodes, [&](const auto& request) {
          return request.ref == assignment.ref;
        });
    if ((!assignment.tombstone && matches != 1)
        || (assignment.tombstone && matches != 0)
        || assignment.slot.value() < plan.before.node_slots
        || assignment.slot.value() >= plan.after.node_slots)
      return NarrowCaptureError::kInvalidHighWater;
  }
  return std::nullopt;
}

std::optional<NarrowCaptureError> validateNetRequests(
    const AffectedCapturePlan& plan)
{
  std::vector<timing_edit::StableNetRef> refs;
  std::vector<std::string> names;
  for (const auto& request : plan.load_nets) {
    if (request.expected_name.empty()
        || std::ranges::find(refs, request.ref) != refs.end()
        || std::ranges::find(names, request.expected_name) != names.end())
      return NarrowCaptureError::kDuplicateRequest;
    if (std::ranges::count(plan.invalidation_closure.load_nets,
                           request.ref) != 1)
      return NarrowCaptureError::kUnplannedDependency;
    refs.push_back(request.ref);
    names.push_back(request.expected_name);
  }
  if (plan.load_nets.size()
      != plan.invalidation_closure.load_nets.size())
    return NarrowCaptureError::kUnplannedDependency;
  return std::nullopt;
}

bool exactIndexBoundary(const DualCsrArcGroupIndex& index,
                        const AffectedCapturePlan& plan)
{
  return index.session_token == plan.session_token
         && index.generation == plan.expected_generation
         && index.arc_high_water == plan.before.arc_slots
         && index.slots.size() == plan.before.arc_slots;
}

std::optional<NarrowCaptureError> validateModelRequest(
    const AffectedNodeModelCapture& request,
    const DualSemanticReadView& committed,
    const timing_edit::TimingEditResolverState& staged,
    const AffectedCapturePlan& plan)
{
  if (request.expected_key.kind != csr::NodeKind::kInstancePin
      || request.expected_key.full_name.empty()
      || request.expected_key.pin_name != request.expected_port
      || request.expected_key.instance_name.empty()
      || request.expected_cell.empty())
    return NarrowCaptureError::kUnplannedDependency;
  const auto binding = staged.nodeBindings().find(request.ref);
  if (binding == staged.nodeBindings().end()
      || binding->second.tombstone
      || binding->second.native_name
             != request.expected_key.full_name
      || binding->second.slot.value() >= plan.after.node_slots)
    return NarrowCaptureError::kBatchBindingMismatch;
  const auto id = csr::NodeId{binding->second.slot.value()};
  if (id.value() < plan.before.node_slots) {
    const auto node = committed.nodeById(id);
    if (!node || node.value().key != request.expected_key
        || node.value().instance_cell != request.expected_cell)
      return NarrowCaptureError::kBatchBindingMismatch;
    return std::nullopt;
  }
  const auto planned = std::ranges::find(
      plan.nodes, request.ref, &AffectedNodeCapture::ref);
  if (planned == plan.nodes.end()
      || planned->expected_key != request.expected_key)
    return NarrowCaptureError::kUnplannedDependency;
  const auto* created
      = std::get_if<timing_edit::CreatedNodeRef>(&request.ref);
  if (created == nullptr)
    return NarrowCaptureError::kUnplannedDependency;
  const auto edit = std::ranges::find(
      plan.normalized_edits, created->creation_token,
      &timing_edit::BufferInsertionMutation::creation_token);
  if (edit == plan.normalized_edits.end()
      || edit->library_cell.cell_name != request.expected_cell)
    return NarrowCaptureError::kBatchBindingMismatch;
  return std::nullopt;
}

std::optional<NarrowCaptureError> validateModelRequests(
    const DualSemanticReadView& committed,
    const timing_edit::TimingEditResolverState& staged,
    const AffectedCapturePlan& plan)
{
  const auto validateSet = [&](const auto& requests) {
    std::set<timing_edit::StableNodeRef> refs;
    std::vector<csr::NodeKey> keys;
    for (const auto& request : requests) {
      if (!refs.insert(request.ref).second
          || std::ranges::find(keys, request.expected_key)
                 != keys.end())
        return std::optional<NarrowCaptureError>{
            NarrowCaptureError::kDuplicateRequest};
      if (const auto error = validateModelRequest(
              request, committed, staged, plan))
        return error;
      keys.push_back(request.expected_key);
    }
    return std::optional<NarrowCaptureError>{};
  };
  if (const auto error = validateSet(plan.node_roles))
    return error;
  if (const auto error = validateSet(plan.cell_pin_capacitances))
    return error;

  std::set<timing_edit::StableNodeRef> expected_roles;
  for (const auto& materialization : plan.token_materializations) {
    expected_roles.insert(timing_edit::StableNodeRef{
        timing_edit::CreatedNodeRef{
            materialization.creation_token,
            timing_edit::CreatedNodeRole::kInput}});
    expected_roles.insert(timing_edit::StableNodeRef{
        timing_edit::CreatedNodeRef{
            materialization.creation_token,
            timing_edit::CreatedNodeRole::kOutput}});
  }
  if (expected_roles.size() != plan.node_roles.size()
      || std::ranges::any_of(plan.node_roles, [&](const auto& request) {
           return !expected_roles.contains(request.ref);
         }))
    return NarrowCaptureError::kUnplannedDependency;

  std::set<timing_edit::StableNodeRef> expected_caps = expected_roles;
  for (const auto& node : plan.nodes)
    if (node.expected_key.kind == csr::NodeKind::kInstancePin)
      expected_caps.insert(node.ref);
  for (const auto& arc : plan.arcs) {
    if (arc.expected_post_multiplicity == 0
        || arc.expected_post_base.kind != csr::ArcKind::kNet
        || arc.expected_post_base.sink.kind
               != csr::NodeKind::kInstancePin
        || std::ranges::none_of(plan.load_nets, [&](const auto& net) {
             return net.expected_name
                    == arc.expected_post_base.owner_name;
           }))
      continue;
    expected_caps.insert(
        detail::stableArcEndpointRefs(arc.ref).sink);
  }
  if (expected_caps.size() != plan.cell_pin_capacitances.size()
      || std::ranges::any_of(
          plan.cell_pin_capacitances, [&](const auto& request) {
            return !expected_caps.contains(request.ref);
          }))
    return NarrowCaptureError::kUnplannedDependency;
  return std::nullopt;
}

bool exactEndpointNode(
    const timing_edit::StableNodeRef& ref,
    const csr::NodeKey& key,
    const DualSemanticReadView& committed,
    const timing_edit::TimingEditResolverState& staged,
    const AffectedCapturePlan& plan)
{
  const auto binding = staged.nodeBindings().find(ref);
  if (binding == staged.nodeBindings().end()
      || binding->second.tombstone
      || binding->second.native_name != key.full_name
      || binding->second.slot.value() >= plan.after.node_slots)
    return false;
  const csr::NodeId id{binding->second.slot.value()};
  if (id.value() < plan.before.node_slots) {
    const auto node = committed.nodeById(id);
    return node && node.value().key == key;
  }
  return std::ranges::count_if(
             plan.nodes, [&](const auto& request) {
               return request.ref == ref
                      && request.expected_key == key;
             })
         == 1;
}

std::optional<NarrowCaptureError> validateArcRequests(
    const DualSemanticReadView& committed,
    const DualCsrArcGroupIndex& index,
    const timing_edit::TimingEditResolverState& staged,
    const AffectedCapturePlan& plan)
{
  std::vector<timing_edit::StableArcRef> refs;
  refs.reserve(plan.arcs.size());
  for (const auto& request : plan.arcs) {
    if (std::ranges::find(refs, request.ref) != refs.end())
      return NarrowCaptureError::kDuplicateRequest;
    if (csr::validateArcKeyBase(request.expected_post_base))
      return NarrowCaptureError::kArcOrdinalMismatch;
    const auto endpoints = detail::stableArcEndpointRefs(request.ref);
    if (!exactEndpointNode(endpoints.source,
                           request.expected_post_base.source,
                           committed, staged, plan)
        || !exactEndpointNode(endpoints.sink,
                              request.expected_post_base.sink,
                              committed, staged, plan))
      return NarrowCaptureError::kUnplannedDependency;

    const auto binding = staged.arcBindings().find(request.ref);
    if (binding == staged.arcBindings().end()
        || binding->second.slot.value() >= plan.after.arc_slots)
      return NarrowCaptureError::kInvalidHighWater;
    const auto id = csr::ArcId{binding->second.slot.value()};
    if (id.value() < plan.before.arc_slots) {
      const auto* slot = index.slot(id);
      if (!request.expected_current_key.has_value()
          || slot == nullptr || slot->ref != request.ref
          || slot->current_active_key
                 != request.expected_current_key)
        return NarrowCaptureError::kBatchBindingMismatch;
      const auto payload = committed.arcById(id);
      if (!payload
          || payload.value().key != *request.expected_current_key)
        return NarrowCaptureError::kBatchBindingMismatch;
    } else if (request.expected_current_key.has_value()) {
      return NarrowCaptureError::kBatchBindingMismatch;
    }
    const bool deactivated
        = request.expected_post_multiplicity == 0;
    if (binding->second.tombstone != deactivated)
      return NarrowCaptureError::kBatchBindingMismatch;
    if (deactivated) {
      if (!request.expected_current_key.has_value()
          || request.expected_current_key->base
                 != request.expected_post_base)
        return NarrowCaptureError::kArcKeyMismatch;
    } else {
      if (request.expected_current_key.has_value()
          && request.expected_current_key->base
                 != request.expected_post_base)
        return NarrowCaptureError::kArcKeyMismatch;
    }
    refs.push_back(request.ref);
  }

  for (const auto& request : plan.arcs) {
    if (request.expected_post_multiplicity == 0)
      continue;
    std::uint64_t peers = 0;
    for (const auto& peer : plan.arcs) {
      if (peer.expected_post_multiplicity > 0
          && peer.expected_post_base == request.expected_post_base) {
        if (peer.expected_post_multiplicity
            != request.expected_post_multiplicity)
          return NarrowCaptureError::kArcMultiplicityMismatch;
        ++peers;
      }
    }
    if (peers != request.expected_post_multiplicity)
      return NarrowCaptureError::kArcMultiplicityMismatch;
  }

  for (const auto& assignment : plan.slot_plan.arcs) {
    const auto request = std::ranges::find_if(
        plan.arcs, [&](const auto& request) {
          return request.ref == assignment.ref;
        });
    if (request == plan.arcs.end()
        || assignment.tombstone
               != (request->expected_post_multiplicity == 0)
        || assignment.slot.value() >= plan.after.arc_slots
        || (assignment.tombstone
            && assignment.slot.value() >= plan.before.arc_slots))
      return NarrowCaptureError::kInvalidHighWater;
  }
  return std::nullopt;
}

}  // namespace

AffectedCapturePlan makeAffectedCapturePlan(
    const timing_edit::PreparedBatch& batch,
    std::vector<AffectedNodeCapture> nodes,
    std::vector<AffectedNetCapture> load_nets,
    std::vector<AffectedNodeModelCapture> node_roles,
    std::vector<AffectedNodeModelCapture>
        cell_pin_capacitances,
    std::vector<AffectedArcCapture> arcs)
{
  return {.session_token = batch.sessionToken(),
          .expected_generation = batch.expectedGeneration(),
          .target_generation = batch.targetGeneration(),
          .before = batch.slotPlan().before,
          .after = batch.slotPlan().after,
          .normalized_edits = batch.normalizedEdits(),
          .normalized_forward = batch.forwardDelta(),
          .normalized_reverse = batch.reverseDelta(),
          .slot_plan = batch.slotPlan(),
          .token_materializations = batch.tokenMaterializations(),
          .invalidation_closure = batch.invalidationClosure(),
          .policy_receipt = batch.policyReceipt(),
          .nodes = std::move(nodes),
          .load_nets = std::move(load_nets),
          .node_roles = std::move(node_roles),
          .cell_pin_capacitances
              = std::move(cell_pin_capacitances),
          .arcs = std::move(arcs)};
}

CandidateBoundCapture::CandidateBoundCapture(
    std::unique_ptr<detail::CandidateCaptureStorage> storage) noexcept
    : _storage(std::move(storage))
{
}

CandidateBoundCapture::CandidateBoundCapture(
    CandidateBoundCapture&&) noexcept = default;
CandidateBoundCapture& CandidateBoundCapture::operator=(
    CandidateBoundCapture&&) noexcept = default;
CandidateBoundCapture::~CandidateBoundCapture() = default;

bool CandidateBoundCapture::success() const noexcept
{
  return _storage != nullptr && _storage->result.success();
}

std::optional<NarrowCaptureError> CandidateBoundCapture::error()
    const noexcept
{
  if (_storage == nullptr)
    return NarrowCaptureError::kBatchBindingMismatch;
  return _storage->result.error;
}

const NarrowCaptureAudit* CandidateBoundCapture::audit() const noexcept
{
  return _storage == nullptr ? nullptr : &_storage->result.audit;
}

CandidateBoundCapture NarrowTimingRecordReader::read(
    const ista::Database& database, const DualSemanticReadView& committed,
    const DualCsrArcGroupIndex& index,
    const timing_edit::TimingEditResolverState& staged_resolver,
    const timing_edit::PreparedBatch& batch,
    const AffectedCapturePlan& plan) const
{
  const auto bound = [&](detail::NativeCaptureResult result) {
    return CandidateBoundCapture{
        std::make_unique<detail::CandidateCaptureStorage>(
            detail::CandidateCaptureStorage{plan, std::move(result)})};
  };
  if (!exactBatchBinding(batch, plan))
    return bound(reject(plan, NarrowCaptureError::kBatchBindingMismatch));
  if (plan.session_token != committed.sessionToken()
      || plan.expected_generation != committed.generation()
      || plan.expected_generation.value()
             == std::numeric_limits<std::uint64_t>::max()
      || plan.target_generation.value()
             != plan.expected_generation.value() + 1)
    return bound(reject(plan, NarrowCaptureError::kInvalidGeneration));
  if (!validHighWater(plan) || plan.before != committed.highWater())
    return bound(reject(plan, NarrowCaptureError::kInvalidHighWater));
  if (!exactIndexBoundary(index, plan))
    return bound(reject(plan, NarrowCaptureError::kBatchBindingMismatch));
  if (!exactResolverBinding(staged_resolver, plan))
    return bound(reject(plan, NarrowCaptureError::kBatchBindingMismatch));
  if (!targetedNoParasitic(plan))
    return bound(reject(plan, NarrowCaptureError::kUnsupportedPolicy));
  if (const auto error = validateNodeRequests(plan))
    return bound(reject(plan, *error));
  if (const auto error = validateNetRequests(plan))
    return bound(reject(plan, *error));
  if (const auto error = validateModelRequests(
          committed, staged_resolver, plan))
    return bound(reject(plan, *error));
  if (const auto error = validateArcRequests(
          committed, index, staged_resolver, plan))
    return bound(reject(plan, *error));
  return bound(detail::captureNativeDualCsrPayload(
      database, committed, index, staged_resolver, plan));
}

}  // namespace ista::timing_dual_csr
