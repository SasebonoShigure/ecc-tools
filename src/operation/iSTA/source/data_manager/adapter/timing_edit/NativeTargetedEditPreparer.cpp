// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
#include "NativeTimingEditPreparer.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

#include "Arc.hpp"
#include "Database.hpp"
#include "DataManager.hpp"
#include "NativeCandidateExecutor.hpp"
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
  csr::ArcId cell, input;
  std::vector<std::pair<csr::ArcId, csr::ArcId>> replaced;
};
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
  const bool rc = database.get_parasitic_library().get_net_map().contains(net);
  const auto capture = [&](const std::string& pin) {
    const auto row = database.get_incoming_arc_list_map().find(pin);
    if (row != database.get_incoming_arc_list_map().end())
      for (auto rank : row->second)
        if (rc || database.get_arc_list().at(rank).get_type() == ArcType::kCell)
          payloads.insert(state.native_arc_ids.at(rank));
  };
  const auto& native_net = database.get_net_map().at(net);
  if (rc)
    for (const auto& pin : native_net.get_pin_name_list()) capture(pin);
  else
    capture(native_net.get_driver_pin());
}
}  // namespace

NativeTargetedEditState NativeTargetedEditPreparer::bindBaseline(const Database& database, const csr::CsrReferenceInput& input,
                                                                 const BaselineSemanticIdentity& baseline, csr::detail::NativeLibraryModels& models)
{
  models.requireDatabase(database);
  NativeTimingEditPreparer::requireBaselineIdentity(database, input, baseline);
  NativeTargetedEditState state;
  state.models = &models;
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
  state.arc_refs.resize(input.arcs.size());
  for (const auto& [ref, binding] : state.resolver.arcBindings())
    state.arc_refs.at(binding.slot.value()) = &ref;
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
  detail::bindTargetedBaselineParasitics(state, input);
  return state;
}

NativeTargetedEditBatch NativeTargetedEditPreparer::prepare(Database& database, const NativeExecutionAuthority& authority, EditBatch plan, NativeTargetedEditState& current,
    std::map<std::string, ParasiticNet> prepared_rc)
{
  demand(authority.active() && authority.sessionToken() == current.session_token && &database == &STADM.getDatabase(),
         "targeted edit requires the canonical Database and matching operation authority");
  demand(current.models != nullptr, "targeted state has no bound Liberty models");
  current.models->requireDatabase(database);
  auto transaction = NativeEditTransaction::beginTargeted(authority);
  auto normalized = normalizeEditBatch(std::move(plan));
  if (const auto* failures = std::get_if<TimingEditContractViolations>(&normalized))
    throw std::invalid_argument("targeted plan normalization: " + failures->front().message);
  plan = std::get<EditBatch>(std::move(normalized));
  demand(current.session_token.value() != 0 && current.baseline, "targeted plan baseline/session mismatch");
  demand(plan.expected_generation == current.generation && current.generation.value() != std::numeric_limits<std::uint64_t>::max(),
         "targeted plan stale generation or generation exhausted");
  AppendOnlySlotPlan slots;
  slots.before = current.resolver.highWater();
  slots.after = slots.before;
  demand(current.node_names.size() == slots.before.node_slots && current.arc_native_ranks.size() == slots.before.arc_slots
             && current.arc_refs.size() == slots.before.arc_slots,
         "targeted address metadata high-water mismatch");
  std::vector<TokenMaterialization> materializations;
  std::vector<Insertion> insertions;
  std::set<std::string> names;
  std::set<csr::NodeId> changed_rows;
  std::set<csr::ArcId> payloads;
  for (const auto& edit : plan.edits) {
    const auto* rc = std::get_if<ParasiticRcInputs>(&edit.rc_inputs);
    demand(!rc || rc->parasitic_policy == ParasiticEditPolicy::kPreRouteTree,
           "targeted buffer requires NoParasitic or prepared pre-route trees");
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
    detail::requireDataOnlyNet(database, net);
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
    Insertion insertion{csr::ArcId{cell_slot.value()}, csr::ArcId{input_slot.value()}, {}};
    changed_rows.insert(current.node_ids.at(driver_name));
    affectedNet(database, current, net, payloads);
    for (const auto& sink : edit.moved_sinks) {
      const auto sink_name = current.resolver.resolveNode(sink);
      const auto rank = movedRank(database, driver_name, sink_name, net);
      const auto old = current.native_arc_ids.at(rank);
      const auto next = append(slots.after.arc_slots);
      demand(current.arc_refs.at(old.value()) != nullptr, "targeted arc reference is absent");
      slots.arcs.push_back({*current.arc_refs.at(old.value()), AppendOnlySlot{old.value()}, true});
      slots.arcs.push_back({CreatedNewNetArcRef{edit.creation_token, output, sink, 0}, next, false});
      insertion.replaced.emplace_back(old, csr::ArcId{next.value()});
      changed_rows.insert(current.node_ids.at(sink_name));
    }
    materializations.push_back(materialization);
    insertions.push_back(std::move(insertion));
  }
  const auto expected_generation = current.generation;
  auto metadata = std::make_unique<detail::PreparedNativeTargetedMetadata>(current, slots, materializations);
  for (const auto& assignment : slots.nodes)
    changed_rows.insert(csr::NodeId{assignment.slot.value()});
  // Data-only insertion preserves start/end membership across this transaction.
  const std::set<std::string> starts(database.get_start_point_list().begin(), database.get_start_point_list().end());
  const std::set<std::string> ends(database.get_end_point_list().begin(), database.get_end_point_list().end());
  // Capture only expected payloads and affected rows before applying the edit.
  std::map<std::string, csr::ArcOutputLoadMatrix> old_loads, new_loads;
  std::map<csr::ArcId, csr::ArcSemanticPayload> old_payloads;
  std::map<csr::NodeId, std::string> q_load_nets;
  for (const auto& edit : plan.edits) {
    const auto net = current.resolver.resolveNet(edit.original_net);
    if (database.get_parasitic_library().get_net_map().contains(net)) continue;
    const auto& driver = database.get_net_map().at(net).get_driver_pin();
    const auto node = detail::targetedNode(database, current, driver, starts, ends);
    if (!node.is_sequential || !node.is_output_pin || !starts.contains(driver)) continue;
    if (q_load_nets.emplace(node.id, net).second)
      old_loads.emplace(net, detail::targetedNetLoad(database, current, net, starts, ends));
  }
  auto capture_old = [&](csr::ArcId id) {
    if (!old_payloads.contains(id))
      old_payloads.emplace(id, detail::targetedArc(database, current, *current.arc_native_ranks.at(id.value()), old_loads, starts, ends));
  };
  for (const auto id : payloads)
    capture_old(id);
  for (const auto& insertion : insertions)
    for (const auto [old, replacement] : insertion.replaced) {
      static_cast<void>(replacement);
      capture_old(old);
    }
  std::map<csr::NodeId, std::pair<std::vector<csr::ArcId>, std::vector<csr::ArcId>>> old_rows;
  for (const auto node : changed_rows)
    if (node.value() < slots.before.node_slots)
      old_rows.emplace(node, std::pair{detail::targetedRow(database, current, node, false),
                                      detail::targetedRow(database, current, node, true)});

  std::vector<std::string> changed_rc_nets;
  changed_rc_nets.reserve(prepared_rc.size());
  for (const auto& [name, tree] : prepared_rc) {
    static_cast<void>(tree);
    changed_rc_nets.push_back(name);
  }
  auto raw = detail::prepareNativeBufferInsertions(database, plan.edits, metadata->resolver(),
                                                   detail::nextNativeInstanceId(database), std::move(prepared_rc));
  auto graph = detail::prepareNativeBufferGraph(database, raw, plan.edits, metadata->resolver());
  std::vector<std::pair<csr::ArcId, std::size_t>> new_ranks;
  new_ranks.reserve(slots.after.arc_slots - slots.before.arc_slots);
  for (std::size_t index = 0; index < insertions.size(); ++index) {
    const auto& insertion = insertions[index];
    new_ranks.emplace_back(insertion.cell, graph.arc_count_before + index * 2);
    new_ranks.emplace_back(insertion.input, graph.arc_count_before + index * 2 + 1);
    for (const auto [old, replacement] : insertion.replaced)
      new_ranks.emplace_back(replacement, *current.arc_native_ranks.at(old.value()));
  }
  auto* metadata_undo = metadata.get();
  transaction.applyMetadata(std::move(metadata));
  for (const auto& insertion : insertions)
    for (const auto& [old, replacement] : insertion.replaced) {
      static_cast<void>(replacement);
      current.arc_native_ranks[old.value()].reset();
    }
  for (const auto& [id, rank] : new_ranks) {
    current.arc_native_ranks[id.value()] = rank;
    if (rank < current.native_arc_ids.size())
      current.native_arc_ids[rank] = id;
    else
      current.native_arc_ids.push_back(id);
  }
  transaction.applyTargeted(std::move(raw), std::move(graph));
  auto& staged = current;
  auto& target = transaction.database();
  csr::CudaDualCsrPatch patch;
  patch.seed_load_changes.reserve(q_load_nets.size());
  for (const auto& [node, net] : q_load_nets) {
    const auto load = detail::targetedNetLoad(target, staged, net, starts, ends);
    new_loads.emplace(net, load);
    if (old_loads.at(net) != load)
      patch.seed_load_changes.push_back({node, old_loads.at(net), load});
  }
  auto bindings = detail::prepareTargetedParasitics(target, staged, changed_rc_nets);
  patch.rc_bindings.reserve(bindings.size());
  for (const auto& [node, next] : bindings) {
    const auto previous = staged.parasitics.find(node);
    patch.rc_bindings.push_back({node, previous == staged.parasitics.end() ? csr::CsrParasiticBinding{} : previous->second, next});
  }
  metadata_undo->applyParasitics(std::move(bindings));
  patch.session_token = current.session_token;
  patch.expected_generation = expected_generation;
  patch.target_generation = staged.generation;
  patch.before = slots.before;
  patch.after = slots.after;
  for (std::uint64_t id = slots.before.node_slots; id < slots.after.node_slots; ++id) {
    auto node = detail::targetedNode(target, staged, staged.node_names.at(id), starts, ends);
    if (!node.is_port && staged.parasitics.contains(node.id))
      patch.rc_load_cells.try_emplace(node.instance_cell, std::cref(staged.models->cell(node.instance_cell)));
    patch.node_appends.push_back({csr::NodeId{id}, node.key, std::move(node)});
  }
  for (std::uint64_t id = slots.before.arc_slots; id < slots.after.arc_slots; ++id) {
    auto arc = detail::targetedArc(target, staged, *staged.arc_native_ranks.at(id), new_loads, starts, ends);
    patch.arc_appends.push_back({csr::ArcId{id}, arc.key, arc.source, arc.sink, std::move(arc)});
  }
  for (const auto& insertion : insertions)
    for (const auto [old, replacement] : insertion.replaced) {
      static_cast<void>(replacement);
      const auto& arc = old_payloads.at(old);
      patch.logical_deactivations.push_back({old, arc.key, arc.source, arc.sink, timing_dual_csr::LogicalArcDeactivateReason::kReplaced, true});
    }
  for (auto id : payloads) {
    if (!staged.arc_native_ranks.at(id.value()))
      continue;
    auto old = std::move(old_payloads.at(id));
    auto next = detail::targetedArc(target, staged, *staged.arc_native_ranks.at(id.value()), new_loads, starts, ends);
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
    const auto& old_out = old_rows.at(node).first;
    const auto& old_in = old_rows.at(node).second;
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
  return {std::move(transaction), staged, std::move(patch)};
}
}  // namespace ista::timing_edit
