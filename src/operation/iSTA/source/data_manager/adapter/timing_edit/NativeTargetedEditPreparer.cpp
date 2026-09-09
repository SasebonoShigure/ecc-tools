// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

#include "Arc.hpp"
#include "CanonicalGraphConstruction.hpp"
#include "Database.hpp"
#include "NativeBufferCellValidation.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "NativeTargetedEditInternal.hpp"
#include "TimingEditValidation.hpp"

namespace ista::timing_edit {
namespace {
void demand(bool value, const char* message)
{
  if (!value)
    throw std::invalid_argument(message);
}
AppendOnlySlot append(std::uint64_t& value)
{
  demand(value != std::numeric_limits<std::uint64_t>::max(), "targeted append high-water exhausted");
  return AppendOnlySlot{value++};
}
struct Insertion
{
  const BufferInsertionMutation* edit;
  TokenMaterialization names;
  std::string driver;
  csr::ArcId cell, input;
  std::vector<std::pair<csr::ArcId, csr::ArcId>> replaced;
};
void requireDataNet(const Database& database, const std::string& name)
{
  const auto& net = database.get_net_map().at(name);
  demand(!database.get_parasitic_library().get_net_map().contains(name), "targeted buffer requires NoParasitic source net");
  demand(net.get_driver_pin_list().size() == 1 && !net.get_driver_pin().empty(), "targeted source requires one driver");
  for (const auto& [clock_name, clock] : database.get_timing_constraint().get_clock_map()) {
    static_cast<void>(clock_name);
    for (const auto& source : clock.get_source_list())
      demand(std::ranges::find(net.get_pin_name_list(), source) == net.get_pin_name_list().end(), "targeted edit touches clock source");
  }
  for (const auto& name : net.get_pin_name_list()) {
    const auto& pin = database.get_pin_map().at(name);
    demand(pin.get_net_name() == net.get_net_name(), "targeted net membership mismatch");
    if (pin.get_is_port())
      continue;
    const auto& instance = database.get_instance_map().at(pin.get_instance_name());
    const auto& cell = database.get_timing_library().get_cell_map().at(instance.get_cell_name());
    demand(!cell.get_port_map().at(pin.get_pin_name()).get_is_clock(), "targeted edit touches library clock pin");
  }
}
std::size_t movedRank(const Database& database, const std::string& driver, const std::string& sink, const std::string& net)
{
  std::optional<std::size_t> result;
  for (const auto rank : database.get_outgoing_arc_list_map().at(driver)) {
    const auto& arc = database.get_arc_list().at(rank);
    if (arc.get_type() != ArcType::kNet || arc.get_sink_pin() != sink || arc.get_owner_name() != net)
      continue;
    demand(!result, "targeted moved sink has ambiguous native arc");
    result = rank;
  }
  demand(result.has_value(), "targeted moved sink has no native arc");
  return *result;
}
void affectedNet(const Database& database, const NativeTargetedEditState& state, const std::string& net, std::set<csr::ArcId>& payloads)
{
  for (const auto& pin : database.get_net_map().at(net).get_pin_name_list()) {
    const auto row = database.get_incoming_arc_list_map().find(pin);
    if (row != database.get_incoming_arc_list_map().end())
      for (auto rank : row->second)
        payloads.insert(state.native_arc_ids.at(rank));
  }
}
}  // namespace

NativeTargetedEditState NativeTargetedEditPreparer::bindBaseline(const Database& database, const csr::CsrReferenceInput& input,
                                                                 const BaselineSemanticIdentity& baseline)
{
  demand(input.source_identity.session_token != 0, "targeted baseline session token is zero");
  NativeTargetedEditState state;
  state.session_token = TimingEditSessionToken{input.source_identity.session_token};
  state.generation = TransactionGeneration{0};
  state.baseline = std::make_shared<const BaselineSemanticIdentity>(baseline);
  state.resolver = TimingEditResolverState::bindReferenceBaseline(database, input);
  demand(state.resolver.highWater().node_slots == input.nodes.size() && state.resolver.highWater().arc_slots == input.arcs.size(),
         "targeted baseline resolver high-water differs from input");
  for (const auto& node : input.nodes) {
    state.node_names.push_back(node.key.full_name);
    demand(state.node_ids.emplace(node.key.full_name, node.id).second, "targeted duplicate baseline node");
  }
  state.arc_native_ranks.resize(input.arcs.size());
  state.native_arc_ids.resize(input.arcs.size());
  auto clock_arcs = std::make_shared<std::set<csr::ArcId>>();
  for (const auto& arc : input.arcs) {
    if (arc.eligibility.clock)
      clock_arcs->insert(arc.id);
    state.arc_native_ranks.at(arc.id.value()) = arc.native_traversal_rank;
    state.native_arc_ids.at(arc.native_traversal_rank) = arc.id;
  }
  state.clock_arcs = std::move(clock_arcs);
  state.base_out_offsets = input.out_row_offsets;
  state.base_in_offsets = input.in_row_offsets;
  return state;
}

NativeTargetedEditBatch NativeTargetedEditPreparer::prepare(const Database& database, DesignEditPlan plan, const NativeTargetedEditState& current)
{
  auto normalized = normalizeDesignEditPlan(std::move(plan));
  if (const auto* failures = std::get_if<TimingEditContractViolations>(&normalized))
    throw std::invalid_argument("targeted plan normalization: " + failures->front().message);
  plan = std::get<DesignEditPlan>(std::move(normalized));
  demand(current.session_token.value() != 0 && current.baseline && plan.baseline == *current.baseline, "targeted plan baseline/session mismatch");
  demand(plan.expected_generation == current.generation && current.generation.value() != std::numeric_limits<std::uint64_t>::max(),
         "targeted plan stale generation or generation exhausted");
  AppendOnlySlotPlan slots;
  slots.before = current.resolver.highWater();
  slots.after = slots.before;
  demand(current.node_names.size() == slots.before.node_slots && current.arc_native_ranks.size() == slots.before.arc_slots,
         "targeted address metadata high-water mismatch");
  std::map<csr::ArcId, StableArcRef> refs;
  for (const auto& [ref, binding] : current.resolver.arcBindings())
    if (!binding.tombstone)
      refs.emplace(csr::ArcId{binding.slot.value()}, ref);
  std::vector<TokenMaterialization> materializations;
  std::vector<Insertion> insertions;
  std::set<std::string> names;
  std::set<csr::NodeId> changed_rows;
  std::set<csr::ArcId> payloads;
  for (const auto& edit : plan.edits) {
    demand(std::holds_alternative<NoParasiticRcInputs>(edit.rc_inputs), "targeted buffer only supports NoParasitic edits");
    const auto& cell = requireLegalNativeBufferCell(database, edit.library_cell);
    demand(std::ranges::count_if(cell.get_cell_arc_list(), [](const auto& arc) { return arc.get_is_timing_graph_arc(); }) == 1,
           "targeted buffer protocol does not support additional disabled graph arcs");
    auto materialization = defaultTokenMaterialization(edit.creation_token, edit.library_cell);
    demand(!current.resolver.materializations().contains(edit.creation_token), "targeted creation token is already materialized");
    for (auto* name : {&materialization.instance_name, &materialization.output_net_name, &materialization.input_pin_name, &materialization.output_pin_name}) {
      demand(names.insert(*name).second && !database.get_instance_map().contains(*name) && !database.get_net_map().contains(*name)
                 && !database.get_pin_map().contains(*name),
             "targeted materialized name collision");
    }
    const auto net = current.resolver.resolveNet(edit.original_net);
    requireDataNet(database, net);
    const auto driver_name = database.get_net_map().at(net).get_driver_pin();
    demand(current.clock_arcs != nullptr, "targeted clock identity metadata is absent");
    for (const auto rank : database.get_outgoing_arc_list_map().at(driver_name)) {
      if (database.get_arc_list().at(rank).get_owner_name() == net)
        demand(!current.clock_arcs->contains(current.native_arc_ids.at(rank)), "targeted edit touches propagated clock net");
    }
    const auto driver = current.resolver.stableNodeForNativeName(driver_name);
    const StableNodeRef input{CreatedNodeRef{edit.creation_token, CreatedNodeRole::kInput}};
    const StableNodeRef output{CreatedNodeRef{edit.creation_token, CreatedNodeRole::kOutput}};
    const StableArcRef cell_ref{
        CreatedCellArcRef{edit.creation_token, edit.library_cell.input_port, edit.library_cell.output_port, edit.library_cell.ordered_arc_signatures, 0}};
    const StableArcRef input_ref{CreatedOldNetArcRef{edit.original_net, driver, input, 0}};
    slots.nodes.push_back({input, append(slots.after.node_slots), false});
    slots.nodes.push_back({output, append(slots.after.node_slots), false});
    slots.nets.push_back({CreatedNetRef{edit.creation_token, CreatedNetRole::kOutputNet}, append(slots.after.net_slots), false});
    const auto cell_slot = append(slots.after.arc_slots), input_slot = append(slots.after.arc_slots);
    slots.arcs.push_back({cell_ref, cell_slot, false});
    slots.arcs.push_back({input_ref, input_slot, false});
    Insertion insertion{&edit, materialization, driver_name, csr::ArcId{cell_slot.value()}, csr::ArcId{input_slot.value()}, {}};
    changed_rows.insert(current.node_ids.at(driver_name));
    affectedNet(database, current, net, payloads);
    for (const auto& sink : edit.moved_sinks) {
      const auto sink_name = current.resolver.resolveNode(sink);
      const auto rank = movedRank(database, driver_name, sink_name, net);
      const auto old = current.native_arc_ids.at(rank);
      const auto next = append(slots.after.arc_slots);
      slots.arcs.push_back({refs.at(old), AppendOnlySlot{old.value()}, true});
      slots.arcs.push_back({CreatedNewNetArcRef{edit.creation_token, output, sink, 0}, next, false});
      insertion.replaced.emplace_back(old, csr::ArcId{next.value()});
      changed_rows.insert(current.node_ids.at(sink_name));
    }
    materializations.push_back(materialization);
    insertions.push_back(std::move(insertion));
  }
  auto staged = current;
  staged.resolver = current.resolver.stageAssignments(slots, materializations);
  staged.generation = TransactionGeneration{current.generation.value() + 1};
  staged.node_names.resize(slots.after.node_slots);
  staged.arc_native_ranks.resize(slots.after.arc_slots);
  for (const auto& assignment : slots.nodes) {
    const auto& name = staged.resolver.resolveNode(assignment.ref);
    staged.node_names.at(assignment.slot.value()) = name;
    staged.node_ids.emplace(name, csr::NodeId{assignment.slot.value()});
    changed_rows.insert(csr::NodeId{assignment.slot.value()});
  }
  auto candidate = NativeDatabaseCandidate::createPreservingGraph(database);
  auto& target = candidate.database();
  auto instance_id = detail::nextNativeInstanceId(target);
  auto appendArc = [&](Arc arc, csr::ArcId id) {
    const auto rank = canonical_graph::appendArc(target, std::move(arc));
    staged.arc_native_ranks.at(id.value()) = rank;
    staged.native_arc_ids.push_back(id);
  };
  for (const auto& insertion : insertions) {
    const auto& edit = *insertion.edit;
    detail::applyNativeBufferInsertion(target, edit, staged.resolver, instance_id);
    demand(instance_id != std::numeric_limits<std::uint64_t>::max() || &insertion == &insertions.back(), "targeted instance ID exhausted");
    ++instance_id;
    target.get_timing_point_map().emplace(insertion.names.input_pin_name, TimingPoint{});
    target.get_timing_point_map().emplace(insertion.names.output_pin_name, TimingPoint{});
    canonical_graph::refreshNetDriverLoad(target, target.get_net_map().at(staged.resolver.resolveNet(edit.original_net)));
    canonical_graph::refreshNetDriverLoad(target, target.get_net_map().at(insertion.names.output_net_name));
    const auto cell_rank = target.get_arc_list().size();
    demand(canonical_graph::appendLibraryCellArcs(target, target.get_instance_map().at(insertion.names.instance_name)), "targeted missing library graph arc");
    staged.arc_native_ranks.at(insertion.cell.value()) = cell_rank;
    staged.native_arc_ids.push_back(insertion.cell);
    appendArc(
        canonical_graph::makeNetArc(target, insertion.driver, insertion.names.input_pin_name, ArcType::kNet, staged.resolver.resolveNet(edit.original_net)),
        insertion.input);
    for (const auto [old, replacement] : insertion.replaced) {
      const auto rank = *staged.arc_native_ranks.at(old.value());
      auto& arc = target.get_arc_list().at(rank);
      auto& outgoing = target.get_outgoing_arc_list_map().at(arc.get_source_pin());
      outgoing.erase(std::ranges::find(outgoing, rank));
      arc = canonical_graph::makeNetArc(target, insertion.names.output_pin_name, arc.get_sink_pin(), ArcType::kNet, insertion.names.output_net_name);
      target.get_outgoing_arc_list_map()[insertion.names.output_pin_name].push_back(rank);
      staged.arc_native_ranks.at(old.value()).reset();
      staged.arc_native_ranks.at(replacement.value()) = rank;
      staged.native_arc_ids.at(rank) = replacement;
    }
  }
  detail::canonicalizeTargetedNativeOrder(target, staged);
  std::map<std::string, csr::ArcOutputLoadMatrix> old_loads, new_loads;
  csr::CudaDualCsrPatch patch;
  patch.session_token = current.session_token;
  patch.expected_generation = current.generation;
  patch.target_generation = staged.generation;
  patch.before = slots.before;
  patch.after = slots.after;
  for (std::uint64_t id = slots.before.node_slots; id < slots.after.node_slots; ++id) {
    auto node = detail::targetedNode(target, staged, staged.node_names.at(id));
    patch.node_appends.push_back({csr::NodeId{id}, node.key, std::move(node)});
  }
  for (std::uint64_t id = slots.before.arc_slots; id < slots.after.arc_slots; ++id) {
    auto arc = detail::targetedArc(target, staged, *staged.arc_native_ranks.at(id), new_loads);
    patch.arc_appends.push_back({csr::ArcId{id}, arc.key, arc.source, arc.sink, std::move(arc)});
  }
  for (const auto& insertion : insertions)
    for (const auto [old, replacement] : insertion.replaced) {
      static_cast<void>(replacement);
      const auto arc = detail::targetedArc(database, current, *current.arc_native_ranks.at(old.value()), old_loads);
      patch.logical_deactivations.push_back({old, arc.key, arc.source, arc.sink, timing_dual_csr::LogicalArcDeactivateReason::kReplaced, true});
    }
  for (auto id : payloads) {
    if (!staged.arc_native_ranks.at(id.value()))
      continue;
    auto old = detail::targetedArc(database, current, *current.arc_native_ranks.at(id.value()), old_loads);
    auto next = detail::targetedArc(target, staged, *staged.arc_native_ranks.at(id.value()), new_loads);
    if (old != next)
      patch.payload_changes.push_back({id, std::move(old), std::move(next)});
  }
  for (const auto node : changed_rows) {
    const auto next_out = detail::targetedRow(target, staged, node, false);
    const auto next_in = detail::targetedRow(target, staged, node, true);
    if (node.value() >= slots.before.node_slots) {
      patch.appended_base_out_rows.push_back({node, next_out});
      patch.appended_base_in_rows.push_back({node, next_in});
      staged.base_out_offsets.push_back(staged.base_out_offsets.back() + next_out.size());
      staged.base_in_offsets.push_back(staged.base_in_offsets.back() + next_in.size());
      continue;
    }
    const auto old_out = detail::targetedRow(database, current, node, false);
    const auto old_in = detail::targetedRow(database, current, node, true);
    const auto begin = current.base_out_offsets.at(node.value()), capacity = current.base_out_offsets.at(node.value() + 1) - begin;
    for (std::uint64_t i = 0; i < capacity; ++i) {
      const auto old = i < old_out.size() ? std::optional{old_out[i]} : std::nullopt;
      const auto next = i < next_out.size() ? std::optional{next_out[i]} : std::nullopt;
      if (old == next)
        continue;
      if (next)
        patch.changes.push_back({node, begin + i, old, *next});
      else if (old)
        patch.removes.push_back({node, begin + i, *old});
    }
    const std::vector<csr::ArcId> old_aux(old_out.begin() + std::min<std::size_t>(capacity, old_out.size()), old_out.end());
    const std::vector<csr::ArcId> next_aux(next_out.begin() + std::min<std::size_t>(capacity, next_out.size()), next_out.end());
    if (old_aux != next_aux)
      patch.aux_rows.push_back({node, old_aux, next_aux});
    demand(old_in.size() == next_in.size(), "targeted edit changed existing BaseIn capacity");
    for (std::size_t i = 0; i < old_in.size(); ++i)
      if (old_in[i] != next_in[i])
        patch.reverse_changes.push_back({node, current.base_in_offsets.at(node.value()) + i, old_in[i], next_in[i]});
  }
  return {std::move(candidate), std::move(staged), std::move(patch)};
}
}  // namespace ista::timing_edit
