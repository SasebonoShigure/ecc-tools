// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include "TimingEditDualCsrDescriptorBuilder.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "CsrParasiticTree.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "Database.hpp"
#include "DataManager.hpp"
#include "NativeCandidateExecutor.hpp"
#include "DualCsrStructuredCapturePlanInternal.hpp"
#include "NativeBufferCellValidation.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "NarrowTimingCaptureInternal.hpp"

namespace ista::timing_dual_csr {
namespace {
using namespace timing_edit;

void demand(bool condition, const char* message)
{
  if (!condition) throw std::invalid_argument(std::string{"direct CSR buffer edit: "} + message);
}

void requireFreshNames(const PendingStructuredBatch& pending)
{
  const auto& before = pending.preparedBatch().baseline().canonical_pre_state;
  for (const auto& names : pending.preparedBatch().tokenMaterializations()) {
    for (const auto* name : {&names.instance_name, &names.output_net_name,
                            &names.input_pin_name, &names.output_pin_name}) {
      const auto less = csr::detail::referenceByteStringLess;
      demand(!std::binary_search(before.instances.begin(), before.instances.end(), *name, less)
                 && !std::binary_search(before.nets.begin(), before.nets.end(), *name, less),
             "materialized name collides with the baseline");
      for (const auto kind : {csr::NodeKind::kPort, csr::NodeKind::kInstancePin}) {
        csr::NodeKey key{kind, *name, {}, {}};
        const auto found = std::lower_bound(before.nodes.begin(), before.nodes.end(), key, csr::nodeKeyLess);
        demand(found == before.nodes.end() || found->kind != kind || found->full_name != *name,
               "materialized pin name collides with the baseline");
      }
      // ponytail: linear in created buffers, not baseline size. Reuse a
      // resolver reverse index if long-trace profiling warrants one.
      for (const auto& [token, previous] : pending.expectedBefore().resolver().materializations()) {
        static_cast<void>(token);
        demand(*name != previous.instance_name && *name != previous.output_net_name
                   && *name != previous.input_pin_name && *name != previous.output_pin_name,
               "materialized name collides with an earlier batch");
      }
    }
  }
}

class DirectEdit
{
  struct PlannedArc
  {
    const StableArcRef* ref;
    csr::ArcId id;
    bool active;
    const csr::ArcSemanticPayload* payload;
  };

 public:
  DirectEdit(const PendingStructuredBatch& pending, const Database& library,
             csr::detail::NativeLibraryModels& models)
      : pending(pending), graph(pending.expectedBefore().graph()),
        resolver(pending.stagedResolver()), models(models)
  {
    models.requireDatabase(library);
    const auto& batch = pending.preparedBatch();
    demand(graph.session_token == batch.sessionToken() && graph.generation == batch.expectedGeneration()
               && graph.high_water == batch.slotPlan().before
               && graph.nodes.size() == graph.high_water.node_slots
               && graph.arc_slots.size() == graph.high_water.arc_slots, "stale pending batch");
    requireFreshNames(pending);
    for (const auto& edit : batch.normalizedEdits()) {
      const auto* rc = std::get_if<ParasiticRcInputs>(&edit.rc_inputs);
      demand(!rc || rc->parasitic_policy == ParasiticEditPolicy::kPreRouteTree,
             "requires NoParasitic or prepared pre-route trees");
      const auto& cell = requireLegalNativeBufferCell(library, edit.library_cell);
      demand(std::ranges::count_if(cell.get_cell_arc_list(),
                 [](const auto& arc) { return arc.get_is_timing_graph_arc(); }) == 1,
             "additional disabled buffer graph arcs are unsupported");
      const auto& model = models.cell(edit.library_cell.cell_name);
      cells.emplace(edit.library_cell.cell_name, &model);
      const auto child = std::ranges::find_if(cell.get_cell_arc_list(),
          [](const auto& arc) { return arc.get_is_timing_graph_arc(); });
      cell_arcs.emplace(edit.library_cell.cell_name, models.arcOrdinal(edit.library_cell.cell_name, *child));
      const auto& names = resolver.materialization(edit.creation_token);
      const auto input = nodeId(StableNodeRef{CreatedNodeRef{edit.creation_token, CreatedNodeRole::kInput}});
      const auto output = nodeId(StableNodeRef{CreatedNodeRef{edit.creation_token, CreatedNodeRole::kOutput}});
      csr::InstanceRecord instance;
      instance.instance_name = names.instance_name;
      instance.cell_name = edit.library_cell.cell_name;
      instance.ordered_pins = names.input_pin_name < names.output_pin_name
          ? std::vector<csr::NodeId>{input, output} : std::vector<csr::NodeId>{output, input};
      csr::detail::applyCanonicalInstanceLibraryRoles(model, instance);
      records.instances.push_back({edit.creation_token, std::move(instance)});
      addNode(input, {csr::NodeKind::kInstancePin, names.input_pin_name, names.instance_name, edit.library_cell.input_port},
              csr::PinDirection::kInput, model, resolver.resolveNet(edit.original_net));
      addNode(output, {csr::NodeKind::kInstancePin, names.output_pin_name, names.instance_name, edit.library_cell.output_port},
              csr::PinDirection::kOutput, model, names.output_net_name);
      for (const auto& ref : edit.moved_sinks) {
        const auto id = nodeId(ref);
        const auto& before = graph.nodes.at(id.value());
        demand(before.net_name == resolver.resolveNet(edit.original_net)
                   && (before.is_port ? before.direction == csr::PinDirection::kOutput
                                      : before.direction == csr::PinDirection::kInput),
               "moved sink has incompatible membership or direction");
        if (!before.is_port)
          timing_edit::detail::requireSupportedMovedSink(before, library.get_timing_library().get_cell_map().at(before.instance_cell));
        auto after = before;
        after.net_name = names.output_net_name;
        demand(nodes.emplace(id, std::move(after)).second, "sink edited twice in one batch");
      }
    }
    for (const auto& assignment : batch.slotPlan().nodes)
      records.nodes.push_back({assignment.ref, csr::NodeId{assignment.slot.value()}, nodes.at(csr::NodeId{assignment.slot.value()})});
    for (const auto& edit : batch.normalizedEdits())
      for (const auto& ref : edit.moved_sinks) {
        const auto id = nodeId(ref);
        records.nodes.push_back({ref, id, nodes.at(id)});
      }
    for (const auto& ref : batch.invalidationClosure().load_nets)
      load_nets.emplace(ref, resolver.resolveNet(ref));

    const auto& index = pending.expectedBefore().arcGroupIndex();
    std::size_t arc_count = batch.slotPlan().arcs.size();
    for (const auto& [ref, net] : load_nets)
      if (const auto* group = index.activeSinkNetGroup(net))
        arc_count += group->active_arc_ids.size();
    arcs.reserve(arc_count);
    std::set<csr::ArcId> structural_ids;
    for (const auto& assignment : batch.slotPlan().arcs) {
      const csr::ArcId id{assignment.slot.value()};
      structural_ids.insert(id);
      const csr::ArcSemanticPayload* selected = nullptr;
      if (id.value() < graph.arc_slots.size()) {
        const auto& old = graph.arc_slots.at(id.value());
        const auto* indexed = index.slot(id);
        demand(assignment.tombstone && old.active && indexed && indexed->ref == assignment.ref
                   && indexed->current_active_key == old.payload.key, "stale arc payload");
        selected = &old.payload;
      } else {
        csr::ArcSemanticPayload payload;
        const auto key = detail::materializedArcKey(assignment.ref, resolver);
        payload.key = key;
        payload.source = nodeId(key.base.source);
        payload.sink = nodeId(key.base.sink);
        payload.kind = key.base.kind;
        if (payload.kind == csr::ArcKind::kCell) {
          const auto& name = node(payload.source).instance_cell;
          const auto ordinal = cell_arcs.at(name);
          const auto& arc = models.cell(name).cell_arcs.at(ordinal);
          demand(arc.source_port == key.base.library_source_port && arc.sink_port == key.base.library_sink_port,
                 "buffer graph arc endpoints differ");
          payload.native_cell_arc_ordinal = ordinal;
          payload.transfer = arc.transfer;
          payload.is_clock_arc = arc.transfer.is_clock_arc;
          payload.is_disabled = arc.transfer.is_disabled;
        } else {
          payload.transfer.delay = payload.transfer.delay_min = payload.transfer.delay_max = 0.0;
        }
        csr::ArcRecord arc;
        arc.kind = payload.kind;
        arc.is_disabled = payload.is_disabled;
        arc.is_clock_arc = payload.is_clock_arc;
        payload.eligibility = csr::detail::projectCanonicalArcEligibility(arc, node(payload.sink), false, false);
        selected = &changed_payloads.emplace(id, std::move(payload)).first->second;
      }
      arcs.push_back({&assignment.ref, id, !assignment.tombstone, selected});
    }
    // Sink-net groups are disjoint. Borrow unchanged payloads until preparation
    // ends; native capture keys and parallel-group proofs are not needed here.
    for (const auto& [ref, net] : load_nets) {
      const auto* group = index.activeSinkNetGroup(net);
      if (!group) continue;
      for (const auto id : group->active_arc_ids) {
        if (structural_ids.contains(id)) continue;
        arcs.push_back({&index.slots.at(id.value()).ref, id, true, &graph.arc_slots.at(id.value()).payload});
      }
    }
  }

  detail::NarrowTimingRecords complete(std::span<const csr::ParasiticNetModel> affected_rc)
  {
    prepareLoads(affected_rc);
    // Borrowed payloads live through this exclusive preparation; only changes
    // need owned records for descriptor construction; transactions capture undo separately.
    for (const auto& arc : arcs)
      if (!arc.active || changed_payloads.contains(arc.id))
        records.arcs.push_back({*arc.ref, arc.id, arc.active, *arc.payload});
    prepareRows();
    const auto& before = graph.reference_payload;
    std::ranges::sort(records.instances, [](const auto& a, const auto& b) {
      return csr::detail::referenceByteStringLess(a.record.instance_name, b.record.instance_name);
    });
    RecordAppend<csr::InstanceRecord> instances{before.instances.size(), {}};
    for (const auto& value : records.instances) instances.values.push_back(value.record);
    if (!instances.values.empty()) records.reference_payload.instances = std::move(instances);
    RecordAppend<csr::ReferencedLibraryCellModel> appended_cells{before.referenced_library_cells.size(), {}};
    for (const auto& [name, cell] : cells)
      if (std::ranges::find(before.referenced_library_cells, name, &csr::ReferencedLibraryCellModel::cell_name)
          == before.referenced_library_cells.end())
        appended_cells.values.push_back(*cell);
    if (!appended_cells.values.empty()) records.reference_payload.referenced_library_cells = std::move(appended_cells);
    RecordAppend<csr::CellPinCapacitanceModel> caps{before.cell_pin_capacitances.size(), {}};
    std::vector<const csr::NodeRecord*> appended_nodes;
    for (const auto& [id, value] : nodes)
      if (id.value() >= graph.nodes.size()) appended_nodes.push_back(&value);
    std::ranges::sort(appended_nodes, [](const auto* a, const auto* b) { return csr::nodeKeyLess(a->key, b->key); });
    for (const auto* value : appended_nodes) caps.values.push_back(capacitance(*value));
    if (!caps.values.empty()) records.reference_payload.cell_pin_capacitances = std::move(caps);
    const auto count = pending.preparedBatch().slotPlan().after.node_slots - graph.nodes.size();
    demand(before.clock_owner_by_node.size() == graph.nodes.size(), "clock owner size differs from graph");
    if (count)
      records.reference_payload.clock_owner_by_node = RecordAppend<std::optional<std::string>>{
          graph.nodes.size(), std::vector<std::optional<std::string>>(count)};
    return std::move(records);
  }

 private:
  csr::NodeId nodeId(const StableNodeRef& ref) const
  {
    const auto* binding = resolver.findNodeBinding(ref);
    demand(binding && !binding->tombstone, "node has no live binding");
    return csr::NodeId{binding->slot.value()};
  }
  csr::NodeId nodeId(const csr::NodeKey& key) const
  {
    const auto* binding = resolver.findNodeBindingForKey(key);
    demand(binding && !binding->tombstone, "arc endpoint has no live binding");
    return csr::NodeId{binding->slot.value()};
  }
  const csr::NodeRecord& node(csr::NodeId id) const
  {
    const auto found = nodes.find(id);
    return found == nodes.end() ? graph.nodes.at(id.value()) : found->second;
  }
  void addNode(csr::NodeId id, csr::NodeKey key, csr::PinDirection direction,
               const csr::ReferencedLibraryCellModel& cell, const std::string& net)
  {
    csr::NodeRecord value;
    value.id = id; value.key = std::move(key); value.direction = direction;
    value.instance_cell = cell.cell_name; value.net_name = net;
    csr::detail::applyCanonicalNodeLibraryRoles(cell, value);
    demand(nodes.emplace(id, std::move(value)).second, "created NodeId is repeated");
  }
  csr::CellPinCapacitanceModel capacitance(const csr::NodeRecord& value)
  {
    return csr::detail::projectCanonicalCellPinCapacitance(models.cell(value.instance_cell), value.id, value.key.pin_name);
  }
  csr::detail::CanonicalLoadPinFacts loadFacts(const csr::NodeRecord& value)
  {
    csr::detail::CanonicalLoadPinFacts result;
    result.direction = value.direction; result.is_port = value.is_port;
    if (value.is_port) {
      const auto& constraints = graph.reference_payload.port_constraints;
      const auto found = std::ranges::find(constraints, value.key.full_name, &csr::PortConstraintModel::map_key);
      if (found != constraints.end()) result.port_output_load = found->output_load;
    } else {
      result.cell_pin_capacitance = capacitance(value);
    }
    return result;
  }
  void prepareLoads(std::span<const csr::ParasiticNetModel> affected_rc)
  {
    const auto& before = graph.reference_payload;
    const bool rc_mode = !before.parasitics.nets.empty();
    demand(rc_mode ? affected_rc.size() == load_nets.size() : affected_rc.empty(),
           "prepared RC coverage differs from affected nets");
    std::map<std::string_view, const csr::ParasiticNetModel*> raw;
    for (const auto& value : affected_rc)
      demand(raw.emplace(value.map_key, &value).second, "duplicate prepared RC net");
    std::map<std::string_view, std::vector<const PlannedArc*>> net_arcs;
    std::map<std::string_view, std::vector<PlannedArc*>> load_arcs;
    for (auto& arc : arcs) {
      if (!arc.active) continue;
      if (arc.payload->kind == csr::ArcKind::kNet)
        net_arcs[arc.payload->key.base.owner_name].push_back(&arc);
      if (rc_mode || arc.payload->kind == csr::ArcKind::kCell)
        load_arcs[node(arc.payload->sink).net_name].push_back(&arc);
    }
    ParasiticNetDelta rc_delta{before.parasitics.nets.size(), {}, {}};
    StartSeedDelta seeds{before.start_seeds.size(), {}, {}};
    for (const auto& [ref, net] : load_nets) {
      demand(!net_arcs[net].empty(), "affected net has no driver");
      const auto& driver_node = node(net_arcs[net].front()->payload->source);
      // Net arcs consume no capacitive load; only cells and deferred Q do.
      if (!rc_mode && load_arcs[net].empty()
          && !(driver_node.is_sequential && driver_node.is_output_pin))
        continue;
      using Sink = std::pair<std::string_view, csr::NodeId>;
      std::vector<Sink> sinks;
      sinks.reserve(net_arcs[net].size());
      std::optional<csr::NodeId> driver;
      for (const auto* captured : net_arcs[net]) {
        const auto& arc = *captured;
        demand(!driver || *driver == arc.payload->source, "net has multiple drivers");
        driver = arc.payload->source;
        sinks.emplace_back(node(arc.payload->sink).key.full_name, arc.payload->sink);
      }
      std::ranges::sort(sinks, {}, &Sink::first);
      sinks.erase(std::unique(sinks.begin(), sinks.end(),
                              [](const auto& a, const auto& b) { return a.first == b.first; }), sinks.end());
      demand(driver.has_value(), "affected net has no driver");
      const auto& source = node(*driver);
      demand(source.net_name == net
                 && (source.is_port ? source.direction == csr::PinDirection::kInput
                                    : source.direction == csr::PinDirection::kOutput),
             "affected net driver has incompatible membership or direction");
      for (const auto& [name, id] : sinks) {
        static_cast<void>(name);
        demand(node(id).net_name == net, "affected sink membership mismatch");
      }
      csr::ArcOutputLoadMatrix load{};
      std::map<csr::NodeId, csr::CsrParasiticBinding> bindings;
      if (rc_mode) {
        demand(before.analysis_context.rc_corner == "pre_route", "unsupported RC context");
        const auto found = raw.find(net);
        demand(found != raw.end(), "affected RC net is missing");
        std::vector<csr::CsrRcPin> pins{{*driver, source.key.full_name, {}}};
        const auto source_load = loadFacts(source);
        if (source.is_port) {
          const auto value = source_load.port_output_load.value_or(0.0);
          csr::detail::requireNonnegativeReference(value);
          for (auto& lane : pins.front().capacitance) lane.fill(value);
        } else {
          csr::detail::addCanonicalLoad(source_load, pins.front().capacitance);
        }
        for (const auto& [name, id] : sinks) {
          csr::ArcOutputLoadMatrix cap{};
          csr::detail::addCanonicalLoad(loadFacts(node(id)), cap);
          pins.push_back({id, name, cap});
        }
        auto tree = std::make_shared<const csr::CsrParasiticTree>(
            csr::prepareCsrParasiticTree(*found->second, *driver, pins));
        for (std::size_t i = 0; i < tree->terminals.size(); ++i)
          bindings.emplace(tree->terminals[i], csr::CsrParasiticBinding{tree, i});
        if (const auto slot = graph.parasitic_net_slots.find(net);
            slot != graph.parasitic_net_slots.end()) {
          const auto& expected = before.parasitics.nets.at(slot->second);
          if (expected != *found->second) rc_delta.changes.push_back({slot->second, expected, *found->second});
        } else {
          rc_delta.appends.push_back(*found->second);
        }

      } else {
        for (const auto& [name, id] : sinks) {
          static_cast<void>(name);
          csr::detail::addCanonicalLoad(loadFacts(node(id)), load);
        }
      }
      const std::optional<csr::ArcOutputLoadMatrix> next_load = rc_mode ? std::nullopt : std::optional{load};
      const auto seed = std::lower_bound(before.start_seeds.begin(), before.start_seeds.end(), source.key.full_name,
          [&](const auto& value, const auto& name) {
            return csr::detail::referenceByteStringLess(graph.nodes.at(value.seed_node.value()).key.full_name, name);
          });
      if (seed != before.start_seeds.end() && seed->seed_node == *driver
          && seed->availability == csr::StartSeedAvailability::kDeferredSequential && source.is_output_pin) {
        auto after = *seed;
        after.parasitic = rc_mode ? bindings.at(*driver) : csr::CsrParasiticBinding{};
        after.output_load = next_load;
        if (after != *seed)
          seeds.changes.push_back({static_cast<std::uint64_t>(seed - before.start_seeds.begin()), *seed, std::move(after)});
      }
      for (auto* captured : load_arcs[net]) {
        auto& arc = *captured;
        const auto next_rc = rc_mode ? bindings.at(arc.payload->sink) : csr::CsrParasiticBinding{};
        if (rc_mode)
          demand((arc.payload->kind == csr::ArcKind::kCell ? arc.payload->sink : arc.payload->source) == *driver,
                 "RC arc has a stale driver");
        if (arc.payload->output_load == next_load && arc.payload->parasitic == next_rc)
          continue;
        auto& replacement = changed_payloads.try_emplace(arc.id, *arc.payload).first->second;
        replacement.output_load = next_load;
        replacement.parasitic = next_rc;
        arc.payload = &replacement;
      }
    }
    if (!rc_delta.changes.empty() || !rc_delta.appends.empty()) records.reference_payload.parasitics = std::move(rc_delta);
    if (!seeds.changes.empty()) records.reference_payload.start_seeds = std::move(seeds);
  }
  void prepareRows()
  {
    std::map<csr::ArcId, const detail::CapturedArc*> changed;
    std::map<csr::NodeId, std::vector<csr::ArcId>> appended_outgoing, appended_incoming;
    std::set<csr::NodeId> sources, sinks;
    for (const auto& [id, value] : nodes) {
      static_cast<void>(value);
      if (id.value() >= graph.nodes.size()) { sources.insert(id); sinks.insert(id); }
    }
    for (const auto& arc : records.arcs) {
      changed.emplace(arc.id, &arc);
      if (arc.active && arc.id.value() >= graph.arc_slots.size()) {
        appended_outgoing[arc.payload.source].push_back(arc.id);
        appended_incoming[arc.payload.sink].push_back(arc.id);
      }
      if (!arc.active || arc.id.value() >= graph.arc_slots.size()) {
        sources.insert(arc.payload.source); sinks.insert(arc.payload.sink);
      }
    }
    const auto payload = [&](csr::ArcId id) -> const csr::ArcSemanticPayload& {
      const auto found = changed.find(id);
      return found == changed.end() ? graph.arc_slots.at(id.value()).payload : found->second->payload;
    };
    const auto order = [&](csr::ArcId id) {
      const auto& arc = payload(id);
      const bool net = arc.kind == csr::ArcKind::kNet;
      return std::tuple{net, std::string_view{arc.key.base.owner_name}, net ? 0 : arc.native_cell_arc_ordinal.value_or(0),
                        std::string_view{arc.key.base.source.full_name}, std::string_view{arc.key.base.sink.full_name}, id.value()};
    };
    const auto row = [&](csr::NodeId id, bool incoming) {
      std::vector<csr::ArcId> result;
      if (id.value() < graph.nodes.size()) {
        const auto& base = incoming ? graph.base_in : graph.base_out;
        for (auto i = base.row_offsets.at(id.value()); i < base.row_offsets.at(id.value() + 1); ++i)
          if (base.physical_slots.at(i)) result.push_back(*base.physical_slots[i]);
        if (!incoming) {
          const auto aux = graph.aux_out.source_to_aux_row.at(id.value());
          if (aux >= 0) {
            const auto& values = graph.aux_out.row_arcs.at(static_cast<std::size_t>(aux));
            result.insert(result.end(), values.begin(), values.end());
          }
        }
        std::erase_if(result, [&](csr::ArcId arc) { const auto it = changed.find(arc); return it != changed.end() && !it->second->active; });
      }
      const auto existing_size = result.size();
      const auto& appends = incoming ? appended_incoming : appended_outgoing;
      if (const auto found = appends.find(id); found != appends.end())
        result.insert(result.end(), found->second.begin(), found->second.end());
      if (result.size() > 1) {
        using OrderedArc = std::pair<decltype(order(csr::ArcId{0})), csr::ArcId>;
        std::vector<OrderedArc> ordered;
        ordered.reserve(result.size());
        for (const auto arc : result) ordered.emplace_back(order(arc), arc);
        const auto middle = ordered.begin() + existing_size;
        // Retained rows normally keep their order; imported Base/Aux rows may not.
        if (!std::ranges::is_sorted(ordered.begin(), middle, {}, &OrderedArc::first))
          std::ranges::sort(ordered.begin(), middle, {}, &OrderedArc::first);
        std::ranges::sort(middle, ordered.end(), {}, &OrderedArc::first);
        std::ranges::inplace_merge(ordered, middle, {}, &OrderedArc::first);
        for (std::size_t i = 0; i < result.size(); ++i) result[i] = ordered[i].second;
      }
      return result;
    };
    for (const auto source : sources) records.outgoing_rows.push_back({source, row(source, false)});
    for (const auto sink : sinks) records.incoming_rows.push_back({sink, row(sink, true)});
  }

  const PendingStructuredBatch& pending;
  const DualCsrState& graph;
  TimingEditResolverView resolver;
  csr::detail::NativeLibraryModels& models;
  std::map<csr::NodeId, csr::NodeRecord> nodes;
  std::map<std::string, const csr::ReferencedLibraryCellModel*> cells;
  std::map<std::string, std::uint64_t> cell_arcs;
  std::map<StableNetRef, std::string> load_nets;
  std::vector<PlannedArc> arcs;
  std::map<csr::ArcId, csr::ArcSemanticPayload> changed_payloads;
  detail::NarrowTimingRecords records;
};
}  // namespace

VerifiedStructuredBatch TimingEditDualCsrDescriptorBuilder::buildDirect(
    PendingStructuredBatch pending, const NativeExecutionAuthority& authority,
    csr::detail::NativeLibraryModels& models, std::span<const csr::ParasiticNetModel> affected_rc)
{
  demand(authority.active() && authority.sessionToken() == pending.preparedBatch().sessionToken(),
         "requires the matching session operation authority");
  auto records = DirectEdit(pending, STADM.getDatabase(), models).complete(affected_rc);
  return finish(std::move(pending), std::move(records), std::nullopt);
}
}  // namespace ista::timing_dual_csr
