// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "DualCsrStructuredBatchPreparer.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "DualCsrStructuredBeforeBindingInternal.hpp"
#include "DualCsrStructuredCapturePlanInternal.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditValidation.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr {
namespace {

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("dual csr structured prepare: "
                              + std::move(message));
}

template <typename Errors>
[[noreturn]] void invalidFirst(const Errors& errors)
{
  if (errors.empty())
    invalid("validation failed without a diagnostic");
  invalid(errors.front().field + ": " + errors.front().message);
}

timing_edit::EditBatch normalizePlan(
    timing_edit::EditBatch plan)
{
  auto normalized = timing_edit::normalizeEditBatch(std::move(plan));
  if (const auto* errors
      = std::get_if<timing_edit::TimingEditContractViolations>(
          &normalized))
    invalidFirst(*errors);
  return std::get<timing_edit::EditBatch>(std::move(normalized));
}

timing_edit::TimingGraphDelta normalizeDelta(
    timing_edit::TimingGraphDelta delta, std::string_view field)
{
  auto normalized
      = timing_edit::normalizeTimingGraphDelta(std::move(delta), field);
  if (const auto* errors
      = std::get_if<timing_edit::TimingEditContractViolations>(
          &normalized))
    invalidFirst(*errors);
  return std::get<timing_edit::TimingGraphDelta>(std::move(normalized));
}

timing_edit::AppendOnlySlot appendSlot(std::uint64_t& next)
{
  if (next == std::numeric_limits<std::uint64_t>::max())
    invalid("append-only slot capacity is exhausted");
  return timing_edit::AppendOnlySlot{next++};
}

template <typename Ref, typename Encoder>
void canonicalizeAssignments(
    std::vector<timing_edit::SlotAssignment<Ref>>& values,
    Encoder encode)
{
  std::ranges::sort(values, [&](const auto& lhs, const auto& rhs) {
    return encode(lhs.ref) < encode(rhs.ref);
  });
}

template <typename Ref, typename Encoder>
void canonicalizeRefs(std::vector<Ref>& values, Encoder encode)
{
  std::ranges::sort(values, [&](const auto& lhs, const auto& rhs) {
    return encode(lhs) < encode(rhs);
  });
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

const timing_edit::NativeSlotBinding& nodeBinding(
    timing_edit::TimingEditResolverView resolver,
    const timing_edit::StableNodeRef& ref)
{
  const auto* found = resolver.findNodeBinding(ref);
  if (found == nullptr || found->tombstone)
    invalid("node reference is not a live committed resolver binding");
  return *found;
}

const timing_edit::NativeSlotBinding& arcBinding(
    timing_edit::TimingEditResolverView resolver,
    const timing_edit::StableArcRef& ref, bool require_live)
{
  const auto* found = resolver.findArcBinding(ref);
  if (found == nullptr
      || (require_live && found->tombstone))
    invalid("arc reference is not a compatible resolver binding");
  return *found;
}

std::string pinPort(const timing_edit::TokenMaterialization& value,
                    timing_edit::CreatedNodeRole role)
{
  const std::string& full = role == timing_edit::CreatedNodeRole::kInput
                                ? value.input_pin_name
                                : value.output_pin_name;
  const std::string prefix = value.instance_name + ":";
  if (!full.starts_with(prefix) || full.size() == prefix.size())
    invalid("created node materialization is not instance:port");
  return full.substr(prefix.size());
}

csr::NodeKey nodeKeyFor(
    const timing_edit::StableNodeRef& ref,
    timing_edit::TimingEditResolverView resolver)
{
  if (const auto* existing
      = std::get_if<timing_edit::ExistingNodeRef>(&ref))
    return existing->baseline_key;
  const auto& created = std::get<timing_edit::CreatedNodeRef>(ref);
  const auto& value = resolver.materialization(created.creation_token);
  return {csr::NodeKind::kInstancePin,
          created.role == timing_edit::CreatedNodeRole::kInput
              ? value.input_pin_name
              : value.output_pin_name,
          value.instance_name, pinPort(value, created.role)};
}

csr::ArcKey arcKeyFor(
    const timing_edit::StableArcRef& ref,
    timing_edit::TimingEditResolverView resolver)
{
  return std::visit(
      [&](const auto& value) -> csr::ArcKey {
        using Ref = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Ref,
                                     timing_edit::ExistingArcRef>) {
          return value.baseline_key;
        } else if constexpr (std::is_same_v<
                                 Ref, timing_edit::CreatedCellArcRef>) {
          const timing_edit::StableNodeRef source{
              timing_edit::CreatedNodeRef{
                  value.creation_token,
                  timing_edit::CreatedNodeRole::kInput}};
          const timing_edit::StableNodeRef sink{
              timing_edit::CreatedNodeRef{
                  value.creation_token,
                  timing_edit::CreatedNodeRole::kOutput}};
          const auto& materialization
              = resolver.materialization(value.creation_token);
          return {{nodeKeyFor(source, resolver),
                   nodeKeyFor(sink, resolver), csr::ArcKind::kCell,
                   materialization.instance_name,
                   value.library_source_port, value.library_sink_port,
                   value.ordered_library_arcs},
                  value.stable_equivalence_ordinal};
        } else if constexpr (std::is_same_v<
                                 Ref, timing_edit::CreatedOldNetArcRef>) {
          return {{nodeKeyFor(value.driver, resolver),
                   nodeKeyFor(value.sink, resolver), csr::ArcKind::kNet,
                   resolver.resolveNet(value.net), {}, {}, {}},
                  value.stable_ordinal};
        } else {
          const timing_edit::StableNetRef net{
              timing_edit::CreatedNetRef{
                  value.creation_token,
                  timing_edit::CreatedNetRole::kOutputNet}};
          return {{nodeKeyFor(value.driver, resolver),
                   nodeKeyFor(value.sink, resolver), csr::ArcKind::kNet,
                   resolver.resolveNet(net), {}, {}, {}},
                  value.stable_ordinal};
        }
      },
      ref);
}

bool sameNetAndSink(const timing_edit::StableArcRef& arc,
                    const timing_edit::StableNetRef& net,
                    const timing_edit::StableNodeRef& sink)
{
  return std::visit(
      [&](const auto& value) {
        using Ref = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Ref,
                                     timing_edit::ExistingArcRef>) {
          return value.baseline_key.base.kind == csr::ArcKind::kNet
                 && timing_edit::StableNetRef{
                        timing_edit::ExistingNetRef{
                            value.baseline_key.base.owner_name}}
                        == net
                 && timing_edit::StableNodeRef{
                        timing_edit::ExistingNodeRef{
                            value.baseline_key.base.sink}}
                        == sink;
        } else if constexpr (std::is_same_v<
                                 Ref, timing_edit::CreatedOldNetArcRef>) {
          return value.net == net && value.sink == sink;
        } else if constexpr (std::is_same_v<
                                 Ref, timing_edit::CreatedNewNetArcRef>) {
          return timing_edit::StableNetRef{
                     timing_edit::CreatedNetRef{
                         value.creation_token,
                         timing_edit::CreatedNetRole::kOutputNet}}
                     == net
                 && value.sink == sink;
        }
        return false;
      },
      arc);
}

timing_edit::StableNodeRef driverOf(
    const timing_edit::StableArcRef& arc)
{
  return std::visit(
      [](const auto& value) -> timing_edit::StableNodeRef {
        using Ref = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Ref,
                                     timing_edit::ExistingArcRef>)
          return timing_edit::ExistingNodeRef{
              value.baseline_key.base.source};
        else if constexpr (std::is_same_v<
                               Ref, timing_edit::CreatedOldNetArcRef>
                           || std::is_same_v<
                               Ref, timing_edit::CreatedNewNetArcRef>)
          return value.driver;
        else
          invalid("cell arc cannot identify a net driver");
      },
      arc);
}

timing_edit::StableArcRef findCurrentNetArc(
    timing_edit::TimingEditResolverView resolver,
    const DualSemanticReadView& view,
    const DualCsrArcGroupIndex& index,
    const timing_edit::StableNetRef& net,
    const timing_edit::StableNodeRef& sink)
{
  const auto owner_name = resolver.resolveNet(net);
  const auto sink_key = nodeKeyFor(sink, resolver);
  const auto* group = index.activeNetSinkGroup(owner_name, sink_key);
  if (group == nullptr || group->active_arc_ids.size() != 1)
    invalid("moved sink has no live stable net arc on original net");
  const auto id = group->active_arc_ids.front();
  const auto* indexed = index.slot(id);
  if (indexed == nullptr || !indexed->current_active_key.has_value()
      || !sameNetAndSink(indexed->ref, net, sink))
    invalid("directed net/sink index has an invalid stable ref");
  const auto* binding = resolver.findArcBinding(indexed->ref);
  const auto payload = view.arcById(id);
  if (binding == nullptr
      || binding->tombstone
      || binding->slot.value() != id.value() || !payload
      || payload.value().key != *indexed->current_active_key
      || payload.value().key.base.kind != csr::ArcKind::kNet
      || payload.value().key.base.owner_name != owner_name
      || payload.value().key.base.sink != sink_key)
    invalid("keyed view, index and resolver disagree for a moved-sink arc");
  return indexed->ref;
}

void requireDataOnlyNet(
    const timing_edit::StableNetRef& net,
    timing_edit::TimingEditResolverView resolver,
    const DualSemanticReadView& view,
    const DualCsrArcGroupIndex& index)
{
  const auto& name = resolver.resolveNet(net);
  const auto* group = index.activeSinkNetGroup(name);
  if (group == nullptr || group->active_arc_ids.empty())
    invalid("source net has no indexed active timing adjacency");
  bool saw_owned_net_arc = false;
  for (const auto id : group->active_arc_ids) {
    const auto* indexed = index.slot(id);
    const auto payload = view.arcById(id);
    if (indexed == nullptr || !indexed->current_active_key || !payload
        || payload.value().key != *indexed->current_active_key)
      invalid("source net policy index disagrees with keyed view");
    if (payload.value().kind == csr::ArcKind::kNet
        && payload.value().key.base.owner_name == name) {
      saw_owned_net_arc = true;
      if (payload.value().eligibility.clock)
        invalid("clock net is outside the Stage 5 data-only boundary");
    }
    for (const auto node_id :
         {payload.value().source, payload.value().sink}) {
      const auto node = view.nodeById(node_id);
      if (!node)
        invalid("source net policy arc has an unresolved endpoint");
      if (node.value().net_name != name)
        continue;
      if (node.value().is_clock_pin)
        invalid("net with a library clock port is outside the Stage 5 data-only boundary");
      if (index.isClockSourceNode(node_id))
        invalid("clock-source net is outside the Stage 5 data-only boundary");
    }
  }
  if (!saw_owned_net_arc)
    invalid("source net has no indexed owned net arc");
}

void invalidateNet(timing_edit::TimingGraphDelta& delta,
                   const timing_edit::StableNetRef& net)
{
  delta.operations.emplace_back(timing_edit::InvalidateLoad{net});
  delta.operations.emplace_back(timing_edit::InvalidateParasitic{net});
}

void invalidateArc(timing_edit::TimingGraphDelta& delta,
                   const timing_edit::StableArcRef& arc)
{
  delta.operations.emplace_back(timing_edit::InvalidateArcDelay{arc});
}

timing_edit::InvalidationClosure closureFrom(
    const timing_edit::TimingGraphDelta& delta)
{
  timing_edit::InvalidationClosure closure;
  for (const auto& operation : delta.operations) {
    if (const auto* load
        = std::get_if<timing_edit::InvalidateLoad>(&operation))
      closure.load_nets.push_back(load->net);
    else if (const auto* parasitic
             = std::get_if<timing_edit::InvalidateParasitic>(&operation))
      closure.parasitic_nets.push_back(parasitic->net);
    else if (const auto* delay
             = std::get_if<timing_edit::InvalidateArcDelay>(&operation))
      closure.arc_delays.push_back(delay->arc);
  }
  canonicalizeRefs(closure.load_nets,
                   timing_edit::canonicalStableNetRefBytes);
  canonicalizeRefs(closure.parasitic_nets,
                   timing_edit::canonicalStableNetRefBytes);
  canonicalizeRefs(closure.arc_delays,
                   timing_edit::canonicalStableArcRefBytes);
  return closure;
}

struct Expansion
{
  explicit Expansion(const timing_edit::AppendOnlyHighWater& before)
      : next_node(before.node_slots),
        next_net(before.net_slots),
        next_arc(before.arc_slots)
  {
    slots.before = before;
    slots.after = before;
  }

  timing_edit::TimingGraphDelta forward;
  timing_edit::TimingGraphDelta reverse;
  timing_edit::AppendOnlySlotPlan slots;
  std::vector<timing_edit::TokenMaterialization> materializations;
  std::uint64_t next_node;
  std::uint64_t next_net;
  std::uint64_t next_arc;
  bool requires_full{false};
};

void requireFreshMaterialization(
    timing_edit::TimingEditResolverView resolver,
    const timing_edit::TokenMaterialization& value,
    std::set<std::string>& batch_names)
{
  if (resolver.findMaterialization(value.creation_token) != nullptr)
    invalid("creation token was already materialized");
  for (const auto* name : {&value.instance_name, &value.output_net_name,
                           &value.input_pin_name,
                           &value.output_pin_name}) {
    if (!batch_names.insert(*name).second)
      invalid("canonical materialization collides within the batch");
  }
}

void expandEdit(
    Expansion& result, const timing_edit::BufferInsertionMutation& edit,
    timing_edit::TimingEditResolverView resolver,
    const DualSemanticReadView& view,
    const DualCsrArcGroupIndex& arc_group_index,
    std::set<std::string>& materialized_names)
{
  requireDataOnlyNet(edit.original_net, resolver, view,
                     arc_group_index);
  for (const auto& sink : edit.moved_sinks)
    static_cast<void>(nodeBinding(resolver, sink));

  const auto materialization
      = timing_edit::defaultTokenMaterialization(edit.creation_token,
                                                 edit.library_cell);
  requireFreshMaterialization(resolver, materialization, materialized_names);
  result.materializations.push_back(materialization);
  if (const auto* rc
      = std::get_if<timing_edit::ParasiticRcInputs>(&edit.rc_inputs))
    result.requires_full
        = result.requires_full
          || rc->parasitic_policy
                 == timing_edit::ParasiticEditPolicy::kFullWithRecord;

  std::vector<timing_edit::StableArcRef> old_arcs;
  old_arcs.reserve(edit.moved_sinks.size());
  std::optional<timing_edit::StableNodeRef> driver;
  for (const auto& sink : edit.moved_sinks) {
    auto old = findCurrentNetArc(resolver, view, arc_group_index,
                                 edit.original_net, sink);
    auto current_driver = driverOf(old);
    if (driver && *driver != current_driver)
      invalid("one original net resolves to multiple semantic drivers");
    driver = std::move(current_driver);
    old_arcs.push_back(std::move(old));
  }
  if (!driver)
    invalid("buffer insertion has no moved-sink driver");

  const timing_edit::CreatedNodeRef input_node{
      edit.creation_token, timing_edit::CreatedNodeRole::kInput};
  const timing_edit::CreatedNodeRef output_node{
      edit.creation_token, timing_edit::CreatedNodeRole::kOutput};
  const timing_edit::StableNodeRef stable_input{input_node};
  const timing_edit::StableNodeRef stable_output{output_node};
  const timing_edit::StableNetRef output_net{
      timing_edit::CreatedNetRef{
          edit.creation_token,
          timing_edit::CreatedNetRole::kOutputNet}};
  const timing_edit::StableArcRef cell_arc{
      timing_edit::CreatedCellArcRef{
          edit.creation_token, edit.library_cell.input_port,
          edit.library_cell.output_port,
          edit.library_cell.ordered_arc_signatures, 0}};
  const timing_edit::StableArcRef driver_to_input{
      timing_edit::CreatedOldNetArcRef{
          edit.original_net, *driver, stable_input, 0}};

  result.forward.operations.emplace_back(timing_edit::AddInstance{
      edit.creation_token, edit.library_cell, edit.placement_inputs});
  result.forward.operations.emplace_back(timing_edit::AddTimingPoint{
      input_node, csr::PinDirection::kInput});
  result.forward.operations.emplace_back(timing_edit::AddTimingPoint{
      output_node, csr::PinDirection::kOutput});
  result.forward.operations.emplace_back(
      timing_edit::AddCellArc{std::get<timing_edit::CreatedCellArcRef>(
          cell_arc)});
  result.forward.operations.emplace_back(
      timing_edit::AddNetArc{driver_to_input});
  result.reverse.operations.emplace_back(
      timing_edit::RemoveInstance{edit.creation_token});
  result.reverse.operations.emplace_back(
      timing_edit::RemoveTimingPoint{input_node});
  result.reverse.operations.emplace_back(
      timing_edit::RemoveTimingPoint{output_node});
  result.reverse.operations.emplace_back(
      timing_edit::RemoveCellArc{
          std::get<timing_edit::CreatedCellArcRef>(cell_arc)});
  result.reverse.operations.emplace_back(
      timing_edit::RemoveNetArc{driver_to_input});

  result.slots.nodes.push_back(
      {stable_input, appendSlot(result.next_node), false});
  result.slots.nodes.push_back(
      {stable_output, appendSlot(result.next_node), false});
  result.slots.nets.push_back(
      {output_net, appendSlot(result.next_net), false});
  result.slots.arcs.push_back(
      {cell_arc, appendSlot(result.next_arc), false});
  result.slots.arcs.push_back(
      {driver_to_input, appendSlot(result.next_arc), false});

  invalidateNet(result.forward, edit.original_net);
  invalidateNet(result.forward, output_net);
  invalidateNet(result.reverse, edit.original_net);
  invalidateNet(result.reverse, output_net);
  invalidateArc(result.forward, cell_arc);
  invalidateArc(result.forward, driver_to_input);
  invalidateArc(result.reverse, cell_arc);
  invalidateArc(result.reverse, driver_to_input);

  for (std::size_t index = 0; index < edit.moved_sinks.size(); ++index) {
    const auto& sink = edit.moved_sinks[index];
    const auto& old_arc = old_arcs[index];
    const timing_edit::StableArcRef new_arc{
        timing_edit::CreatedNewNetArcRef{
            edit.creation_token, stable_output, sink, 0}};
    result.forward.operations.emplace_back(
        timing_edit::RemoveNetArc{old_arc});
    result.forward.operations.emplace_back(
        timing_edit::AddNetArc{new_arc});
    result.forward.operations.emplace_back(timing_edit::ReconnectSink{
        sink, edit.original_net, output_net});
    result.reverse.operations.emplace_back(
        timing_edit::AddNetArc{old_arc});
    result.reverse.operations.emplace_back(
        timing_edit::RemoveNetArc{new_arc});
    result.reverse.operations.emplace_back(timing_edit::ReconnectSink{
        sink, output_net, edit.original_net});
    result.slots.arcs.push_back(
        {old_arc, arcBinding(resolver, old_arc, true).slot, true});
    result.slots.arcs.push_back(
        {new_arc, appendSlot(result.next_arc), false});
    invalidateArc(result.forward, old_arc);
    invalidateArc(result.forward, new_arc);
    invalidateArc(result.reverse, old_arc);
    invalidateArc(result.reverse, new_arc);
  }
}

}  // namespace

csr::NodeKey detail::stableNodeKey(
    const timing_edit::StableNodeRef& ref,
    timing_edit::TimingEditResolverView resolver)
{
  return nodeKeyFor(ref, resolver);
}

csr::ArcKey detail::materializedArcKey(
    const timing_edit::StableArcRef& ref,
    timing_edit::TimingEditResolverView resolver)
{
  return arcKeyFor(ref, resolver);
}

csr::ArcKey detail::committedArcKey(
    const timing_edit::StableArcRef& ref,
    timing_edit::TimingEditResolverView resolver,
    const DualSemanticReadView& view)
{
  const auto& binding = arcBinding(resolver, ref, true);
  const auto payload = view.arcById(csr::ArcId{binding.slot.value()});
  if (!payload)
    invalid("committed stable arc has no active logical ArcId payload");
  return payload.value().key;
}

PendingStructuredBatch DualCsrStructuredBatchPreparer::prepare(
    const timing_edit::DesignEditPlan& plan,
    const timing_edit::TimingEditResolverState& committed_resolver,
    const DualSemanticReadView& committed_view,
    const DualCsrState& exact_committed_graph,
    const std::optional<timing_edit::CommittedEditIdentity>& exact_committed_identity,
    const DualCsrArcGroupIndex& committed_arc_group_index)
{
  if (plan.schema_version != timing_edit::kDesignEditPlanSchema)
    invalid("unknown design plan schema");
  auto baseline = std::make_shared<const timing_edit::BaselineSemanticIdentity>(plan.baseline);
  if (const auto errors = timing_edit::validateBaselineSemanticIdentity(*baseline); !errors.empty())
    invalidFirst(errors);
  if (plan.expected_generation.value() == 0
      && (!exact_committed_identity || baseline->fingerprints
              != exact_committed_identity->current_fingerprints
          || baseline->canonical_pre_state
                 != exact_committed_identity->canonical_pre_state))
    invalid("generation-zero plan baseline does not match committed state");
  return prepareBound({plan.expected_generation, plan.edits}, std::move(baseline), committed_resolver,
                      committed_view, exact_committed_graph, exact_committed_identity, committed_arc_group_index);
}

PendingStructuredBatch DualCsrStructuredBatchPreparer::prepareBound(
    timing_edit::EditBatch plan,
    std::shared_ptr<const timing_edit::BaselineSemanticIdentity> baseline,
    const timing_edit::TimingEditResolverState& committed_resolver,
    const DualSemanticReadView& committed_view,
    const DualCsrState& exact_committed_graph,
    const std::optional<timing_edit::CommittedEditIdentity>& exact_committed_identity,
    const DualCsrArcGroupIndex& committed_arc_group_index)
{
  detail::requireStructuredExactBefore(exact_committed_graph,
                                       committed_resolver,
                                       exact_committed_identity,
                                       committed_arc_group_index,
                                       committed_view);
  auto normalized = normalizePlan(std::move(plan));
  if (normalized.expected_generation != committed_view.generation())
    invalid("plan generation does not match committed generation");


  Expansion expansion(committed_resolver.highWater());
  std::set<std::string> materialized_names;
  for (const auto& edit : normalized.edits)
    expandEdit(expansion, edit, committed_resolver, committed_view,
               committed_arc_group_index, materialized_names);
  expansion.slots.after
      = {expansion.next_node, expansion.next_net, expansion.next_arc};
  canonicalizeAssignments(expansion.slots.nodes,
                          timing_edit::canonicalStableNodeRefBytes);
  canonicalizeAssignments(expansion.slots.nets,
                          timing_edit::canonicalStableNetRefBytes);
  canonicalizeAssignments(expansion.slots.arcs,
                          timing_edit::canonicalStableArcRefBytes);
  expansion.forward = normalizeDelta(std::move(expansion.forward),
                                     "prepared.normalized_forward");
  expansion.reverse = normalizeDelta(std::move(expansion.reverse),
                                     "prepared.normalized_reverse");

  timing_edit::PolicyReceipt policy;
  if (expansion.requires_full) {
    policy.disposition = timing_edit::EditDisposition::kFullWithRecord;
    policy.reasons
        = {"source net parasitics require a recorded full rebuild"};
  }
  timing_edit::PreparedBatchData data;
  data.session_token = committed_view.sessionToken();
  data.expected_generation = normalized.expected_generation;
  data.target_generation = timing_edit::TransactionGeneration{
      normalized.expected_generation.value() + 1};
  data.baseline = std::move(baseline);
  if constexpr (incregpusta_debug_checks_enabled)
    data.expected_pre_committed_identity = exact_committed_identity;
  data.normalized_edits = std::move(normalized.edits);
  data.normalized_forward = std::move(expansion.forward);
  data.normalized_reverse = std::move(expansion.reverse);
  data.slot_plan = std::move(expansion.slots);
  data.token_materializations = std::move(expansion.materializations);
  data.invalidation_closure = closureFrom(data.normalized_forward);
  data.policy_receipt = std::move(policy);

  // Plan, local expansion, slots and policy were checked while constructing
  // them above. Full arbitrary PreparedBatch validation remains at create().
  auto prepared = timing_edit::PreparedBatch(std::move(data));
  auto staged = committed_resolver.prepare(prepared);
  ExpectedBeforeBinding binding{exact_committed_graph, committed_resolver, committed_arc_group_index};
  PendingStructuredBatch pending{std::move(prepared), std::move(staged), std::move(binding)};
  if constexpr (incregpusta_debug_checks_enabled)
    static_cast<void>(pending.capturePlan());
  return pending;
}

const AffectedCapturePlan& PendingStructuredBatch::capturePlan() const
{
  const auto& graph = _expected_before.graph();
  const auto& index = _expected_before.arcGroupIndex();
  if (graph.session_token != _prepared_batch.sessionToken()
      || graph.generation != _prepared_batch.expectedGeneration()
      || graph.high_water != _prepared_batch.slotPlan().before
      || index.session_token != graph.session_token || index.generation != graph.generation
      || _expected_before.resolver().highWater() != graph.high_water)
    invalid("native capture requested after the bound state changed");
  if (!_capture_plan) {
    const DualSemanticReadView view(graph);
    auto plan = detail::makeStructuredCapturePlan(_prepared_batch, _expected_before.resolver(),
                                                 stagedResolver(), index, view);
    detail::requireStructuredSelectedBefore(graph, _expected_before.resolver(), index, view, plan);
    _capture_plan = std::move(plan);
  }
  return *_capture_plan;
}

}  // namespace ista::timing_dual_csr
