#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "CanonicalTimingRecordProjectorInternal.hpp"
#include "Database.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "NativeDualCsrAffectedArcIndex.hpp"
#include "NativeDualCsrArcBijectionInternal.hpp"
#include "NativeDualCsrCaptureRunInternal.hpp"
#include "NativeDualCsrDataNetPolicyInternal.hpp"
#include "NativeDualCsrOutgoingOrderInternal.hpp"
#include "NativeDualCsrPayloadCaptureInternal.hpp"
#include "NativeDualCsrReferenceCapture.hpp"
#include "NativeDualCsrParasiticCapture.hpp"
#include "NativeTargetedEditPreparer.hpp"
#include "NativeDualCsrTypedCaptureInternal.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::timing_dual_csr {
namespace {

using detail::abortCapture;
using detail::CaptureAbort;
using detail::CaptureRun;

template <typename T, typename Predicate>
const T* findUnique(const std::vector<T>& values, Predicate predicate)
{
  const T* result = nullptr;
  for (const auto& value : values) {
    if (!predicate(value))
      continue;
    if (result != nullptr)
      abortCapture(NarrowCaptureError::kDuplicateRequest);
    result = &value;
  }
  return result;
}

class CaptureBuilder final
{
 public:
  CaptureBuilder(const ista::Database& database, csr::detail::NativeLibraryModels& models,
                 const DualSemanticReadView& committed,
                 const DualCsrArcGroupIndex& committed_index,
                 timing_edit::TimingEditResolverView staged,
                 const AffectedCapturePlan& plan, CaptureRun& run,
                 const timing_edit::NativeTargetedEditBatch* native)
      : _database(database), _models(models),
        _committed(committed),
        _committed_index(committed_index),
        _staged(staged),
        _plan(plan),
        _run(run), _native(native),
        _nodes(plan.after.node_slots)
  {
  }

  void build()
  {
    _models.requireDatabase(_database);
    validateParasiticMode();
    if (const auto error = detail::validateNativeDataOnlyNets(
            _database, _plan))
      abortCapture(*error);
    auto indexed = detail::indexPlannedNodeCapture(_plan);
    if (const auto* error = std::get_if<NarrowCaptureError>(&indexed))
      abortCapture(*error);
    _planned_nodes = std::get<detail::PlannedNodeCaptureIndex>(std::move(indexed));
    for (const auto& name : _database.get_start_point_list())
      ++_starts[name];
    for (const auto& name : _database.get_end_point_list())
      ++_ends[name];
    for (const auto& constraint : _committed.referencePayload().port_constraints) {
      if (!constraint.resolved_node)
        continue;
      auto& entry = _port_loads[*constraint.resolved_node];
      ++entry.first;
      entry.second = constraint.output_load;
    }
    for (const auto& ref : _plan.invalidation_closure.arc_delays)
      ++_delay_invalidations[ref];
    captureLibraries();
    captureRequestedNodes();
    captureInstances();
    captureNetLoads();
    captureArcs();
    if (_run.records().node_roles.size() != _plan.node_roles.size() || _run.records().cell_pin_capacitances.size() != _plan.cell_pin_capacitances.size())
      abortCapture(NarrowCaptureError::kUnplannedDependency);
    std::vector<std::optional<csr::NodeKey>> captured_node_keys(
        _plan.after.node_slots);
    for (const auto& [name, node] : _node_by_name) {
      static_cast<void>(name);
      if (node.id.value() >= captured_node_keys.size()
          || captured_node_keys[node.id.value()])
        abortCapture(NarrowCaptureError::kMalformedNativeRecord);
      captured_node_keys[node.id.value()] = node.key;
    }
    if (const auto error = detail::finalizeNativeDualCsrReferenceCapture(
            _committed, _plan, captured_node_keys, _run.records()))
      abortCapture(*error);
    if (_native)
      if (const auto error = detail::captureNativeDualCsrParasitics(
              _database, *_native, _committed, _plan, _run.records()))
        abortCapture(*error);
  }

 private:
  void validateParasiticMode() const
  {
    const bool has_rc = !_database.get_parasitic_library().get_net_map().empty();
    const auto& before = _committed.referencePayload();
    if (has_rc != !before.parasitics.nets.empty()
        || (has_rc && (!_native || before.analysis_context.rc_corner != "pre_route")))
      abortCapture(NarrowCaptureError::kParasiticModeMismatch);
    for (const auto& edit : _plan.normalized_edits)
      if (has_rc != std::holds_alternative<timing_edit::ParasiticRcInputs>(edit.rc_inputs))
        abortCapture(NarrowCaptureError::kParasiticModeMismatch);
  }

  const timing_edit::BufferInsertionMutation& editForToken(
      const timing_edit::CreationToken& token) const
  {
    const auto* result = findUnique(
        _plan.normalized_edits,
        [&](const auto& item) { return item.creation_token == token; });
    if (result == nullptr)
      abortCapture(NarrowCaptureError::kUnplannedDependency);
    return *result;
  }

  csr::NodeId nodeIdForRequest(const AffectedNodeCapture& request) const
  {
    const auto* binding = _staged.findNodeBinding(request.ref);
    if (binding == nullptr
        || binding->tombstone
        || binding->native_name
               != request.expected_key.full_name
        || binding->slot.value() >= _plan.after.node_slots)
      abortCapture(NarrowCaptureError::kInvalidHighWater);
    return csr::NodeId{binding->slot.value()};
  }

  csr::ArcId arcIdForRequest(const AffectedArcCapture& request) const
  {
    const auto* binding = _staged.findArcBinding(request.ref);
    if (binding == nullptr
        || binding->slot.value() >= _plan.after.arc_slots
        || binding->tombstone
               != (request.expected_post_multiplicity == 0))
      abortCapture(NarrowCaptureError::kInvalidHighWater);
    return csr::ArcId{binding->slot.value()};
  }

  const csr::ArcSemanticPayload* committedArcForRequest(
      const AffectedArcCapture& request) const
  {
    const auto id = arcIdForRequest(request);
    if (id.value() >= _plan.before.arc_slots) {
      if (request.expected_current_key)
        abortCapture(NarrowCaptureError::kBatchBindingMismatch);
      return nullptr;
    }
    const auto* indexed = _committed_index.slot(id);
    if (!request.expected_current_key || indexed == nullptr
        || indexed->ref != request.ref
        || indexed->current_active_key
               != request.expected_current_key)
      abortCapture(NarrowCaptureError::kBatchBindingMismatch);
    const auto old = _committed.arcById(id);
    if (!old || old.value().key != *request.expected_current_key)
      abortCapture(NarrowCaptureError::kMalformedNativeRecord);
    return &old.value();
  }

  csr::NodeId resolveNodeId(const csr::NodeKey& key) const
  {
    auto resolved = detail::resolvePlannedNodeId(
        _committed, _staged, _plan, _planned_nodes, key);
    if (const auto* error = std::get_if<NarrowCaptureError>(&resolved))
      abortCapture(*error);
    return std::get<csr::NodeId>(resolved);
  }

  const csr::ReferencedLibraryCellModel& requireCapturedCell(
      const std::string& cell_name) const
  {
    const auto found = _library_by_name.find(cell_name);
    if (found == _library_by_name.end())
      abortCapture(NarrowCaptureError::kUnplannedDependency);
    return *found->second;
  }

  void captureLibraries()
  {
    std::vector<timing_edit::LibraryCellRef> requested;
    for (const auto& edit : _plan.normalized_edits) {
      const auto sameCell = [&](const auto& value) {
        return value.cell_name == edit.library_cell.cell_name;
      };
      const auto found = std::ranges::find_if(requested, sameCell);
      if (found != requested.end()) {
        if (*found != edit.library_cell)
          abortCapture(NarrowCaptureError::kDuplicateRequest);
        continue;
      }
      requested.push_back(edit.library_cell);
    }
    if constexpr (incregpusta_debug_checks_enabled)
      _run.records().audit.requested_library_cells = requested.size();
    for (const auto& item : requested) {
      const auto& model = _models.cell(item.cell_name);
      if constexpr (incregpusta_debug_checks_enabled)
        ++_run.records().audit.library_cell_reads;
      if (const auto error = detail::validateCapturedLibraryCell(item, model))
        abortCapture(*error);
      if (!_library_by_name.emplace(item.cell_name, &model).second)
        abortCapture(NarrowCaptureError::kDuplicateRequest);
      _run.records().library_cells.push_back({item, model});
      if constexpr (incregpusta_debug_checks_enabled)
        ++_run.records().audit.accepted_library_cells;
    }
  }

  bool listedOnce(const std::unordered_map<std::string_view, std::size_t>& names,
                  const std::string& name) const
  {
    const auto found = names.find(name);
    const auto count = found == names.end() ? 0 : found->second;
    if (count > 1)
      abortCapture(NarrowCaptureError::kMalformedNativeRecord);
    return count == 1;
  }

  const csr::NodeRecord& projectNode(const std::string& full_name)
  {
    if (const auto found = _node_by_name.find(full_name);
        found != _node_by_name.end())
      return found->second;
    const auto pin = _database.get_pin_map().find(full_name);
    if (pin == _database.get_pin_map().end())
      abortCapture(NarrowCaptureError::kNativeNodeNotFound);
    std::set<std::string> starts;
    std::set<std::string> ends;
    if (listedOnce(_starts, full_name))
      starts.insert(full_name);
    if (listedOnce(_ends, full_name))
      ends.insert(full_name);
    std::unordered_map<std::string, std::string> owners;
    if (!pin->second.get_is_port()) {
      const auto instance
          = _database.get_instance_map().find(
              pin->second.get_instance_name());
      if (instance == _database.get_instance_map().end()
          || std::ranges::count(instance->second.get_pin_name_list(),
                                full_name) != 1)
        abortCapture(NarrowCaptureError::kMalformedNativeRecord);
      owners.emplace(full_name, instance->first);
    }
    auto record = csr::detail::projectCanonicalNodeRecord(
        _database, full_name, pin->second, starts, ends, owners);
    record.id = resolveNodeId(record.key);
    if (!record.is_port) {
      auto projected = detail::projectPlannedNodeRole(
          plannedModel(record.key, true), _staged, _plan.after,
          _library_by_name, record);
      if (const auto* error
          = std::get_if<NarrowCaptureError>(&projected))
        abortCapture(*error);
      auto role = std::get<std::optional<detail::CapturedNodeRole>>(
          std::move(projected));
      if (role) {
        if constexpr (incregpusta_debug_checks_enabled)
          ++_run.records().audit.node_role_reads;
        _run.records().node_roles.push_back(std::move(*role));
        if constexpr (incregpusta_debug_checks_enabled)
          ++_run.records().audit.accepted_node_roles;
      }
    }
    if (record.id.value() >= _nodes.size())
      abortCapture(NarrowCaptureError::kInvalidHighWater);
    const auto occupied = _node_name_by_id.find(record.id.value());
    if (occupied != _node_name_by_id.end()
        && occupied->second != full_name)
      abortCapture(NarrowCaptureError::kDuplicateRequest);
    _nodes[record.id.value()] = record;
    _node_ids.emplace(full_name, record.id);
    _node_name_by_id.emplace(record.id.value(), full_name);
    if (!record.is_port)
      _ordered_pin_owners.emplace(full_name,
                                  record.key.instance_name);
    if constexpr (incregpusta_debug_checks_enabled)
      ++_run.records().audit.node_reads;
    return _node_by_name.emplace(full_name, std::move(record))
        .first->second;
  }

  const AffectedNodeModelCapture* plannedModel(const csr::NodeKey& key, bool role) const
  {
    const auto found = _planned_nodes.find(key.full_name);
    if (found == _planned_nodes.end())
      return nullptr;
    return role ? found->second.role : found->second.capacitance;
  }

  const csr::CellPinCapacitanceModel* captureCapacitance(
      const csr::NodeRecord& node)
  {
    if (node.is_port)
      return nullptr;
    const auto existing = _captured_capacitances.find(node.id);
    if (existing != _captured_capacitances.end())
      return &_run.records().cell_pin_capacitances[existing->second].model;
    auto projected = detail::projectPlannedCapacitance(
        _database, plannedModel(node.key, false), _staged,
        _plan.after, node);
    if (const auto* error
        = std::get_if<NarrowCaptureError>(&projected))
      abortCapture(*error);
    auto captured = std::get<detail::CapturedCellPinCapacitance>(
        std::move(projected));
    if constexpr (incregpusta_debug_checks_enabled)
      ++_run.records().audit.cell_pin_capacitance_reads;
    _captured_capacitances.emplace(node.id, _run.records().cell_pin_capacitances.size());
    _run.records().cell_pin_capacitances.push_back(
        std::move(captured));
    if constexpr (incregpusta_debug_checks_enabled)
      ++_run.records().audit.accepted_cell_pin_capacitances;
    return &_run.records().cell_pin_capacitances.back().model;
  }

  void captureRequestedNodes()
  {
    for (const auto& request : _plan.nodes) {
      const auto& node = projectNode(request.expected_key.full_name);
      if (node.key != request.expected_key
          || node.id != nodeIdForRequest(request))
        abortCapture(NarrowCaptureError::kMalformedNativeRecord);
      if (node.id.value() >= _plan.before.node_slots
          && (node.is_startpoint || node.is_endpoint
              || node.is_clock_pin))
        abortCapture(NarrowCaptureError::kUnsupportedPolicy);
      captureCapacitance(node);
      _run.records().nodes.push_back(
          {request.ref, node.id, node});
      if constexpr (incregpusta_debug_checks_enabled)
        ++_run.records().audit.accepted_nodes;
    }
  }

  void captureInstances()
  {
    for (const auto& item : _plan.token_materializations) {
      const auto& edit = editForToken(item.creation_token);
      const auto native = _database.get_instance_map().find(
          item.instance_name);
      if (native == _database.get_instance_map().end())
        abortCapture(NarrowCaptureError::kNativeInstanceNotFound);
      if (native->second.get_cell_name()
              != edit.library_cell.cell_name)
        abortCapture(NarrowCaptureError::kMalformedNativeRecord);
      std::map<std::string, csr::NodeId> ids;
      for (const auto& pin_name : native->second.get_pin_name_list()) {
        const auto& node = projectNode(pin_name);
        if (!ids.emplace(pin_name, node.id).second)
          abortCapture(NarrowCaptureError::kMalformedNativeRecord);
        captureCapacitance(node);
      }
      auto record = csr::detail::projectCanonicalInstanceRecord(
          _database, item.instance_name, native->second, ids);
      const auto& cell = requireCapturedCell(record.cell_name);
      csr::detail::applyCanonicalInstanceLibraryRoles(cell, record);
      if (record.has_timing_checks)
        abortCapture(NarrowCaptureError::kUnsupportedPolicy);
      _run.records().instances.push_back(
          {item.creation_token, std::move(record)});
      if constexpr (incregpusta_debug_checks_enabled)
        ++_run.records().audit.instance_reads;
      if constexpr (incregpusta_debug_checks_enabled)
        ++_run.records().audit.accepted_instances;
    }
  }

  std::optional<double> portOutputLoad(csr::NodeId id) const
  {
    const auto found = _port_loads.find(id);
    if (found == _port_loads.end())
      return std::nullopt;
    if (found->second.first != 1)
      abortCapture(NarrowCaptureError::kMalformedNativeRecord);
    return found->second.second;
  }

  void captureNetLoads()
  {
    for (const auto& request : _plan.load_nets) {
      const auto net = _database.get_net_map().find(
          request.expected_name);
      if (net == _database.get_net_map().end()
          || net->second.get_net_name() != request.expected_name)
        abortCapture(NarrowCaptureError::kNativeNetNotFound);
      std::map<std::string, csr::detail::CanonicalLoadPinFacts> facts;
      for (const auto& pin_name : net->second.get_load_pin_list()) {
        const auto& node = projectNode(pin_name);
        csr::detail::CanonicalLoadPinFacts pin;
        pin.key = node.key;
        pin.direction = node.direction;
        pin.is_port = node.is_port;
        if (node.is_port) {
          if (node.direction == csr::PinDirection::kOutput
              || node.direction == csr::PinDirection::kInout)
            pin.port_output_load = portOutputLoad(node.id);
        } else {
          const auto* capacitance = captureCapacitance(node);
          if (capacitance == nullptr)
            abortCapture(NarrowCaptureError::kMalformedNativeRecord);
          pin.cell_pin_capacitance = *capacitance;
        }
        if (!facts.emplace(pin_name, std::move(pin)).second)
          abortCapture(NarrowCaptureError::kMalformedNativeRecord);
      }
      const auto load = _database.get_parasitic_library().get_net_map().contains(request.expected_name)
          ? std::optional<csr::ArcOutputLoadMatrix>{}
          : std::optional{csr::detail::projectCanonicalNoParasiticNetLoad(_database, request.expected_name, facts)};
      if (!_load_by_net.emplace(request.expected_name, load).second)
        abortCapture(NarrowCaptureError::kDuplicateRequest);
      _run.records().net_loads.push_back(
          {request.ref, request.expected_name, load});
      if constexpr (incregpusta_debug_checks_enabled)
        ++_run.records().audit.net_reads;
      if constexpr (incregpusta_debug_checks_enabled)
        ++_run.records().audit.accepted_load_nets;
    }
  }
  std::vector<detail::NativeCandidateArcOrderFact> projectAffectedArcs()
  {
    const auto collected = detail::collectNativeAffectedArcAdjacency(
        _database, _committed_index, _plan);
    if (const auto* error = std::get_if<NarrowCaptureError>(&collected))
      abortCapture(*error);
    const auto& adjacency
        = std::get<detail::NativeAffectedArcAdjacency>(collected);
    if constexpr (incregpusta_debug_checks_enabled)
      _run.records().audit.index_group_lookups = adjacency.index_group_lookups;
    if constexpr (incregpusta_debug_checks_enabled)
      _run.records().audit.native_arc_candidate_upper_bound = adjacency.candidate_neighbor_upper_bound;
    const auto& indices = adjacency.indices_in_native_order;
    std::vector<const ista::Arc*> native;
    std::vector<csr::NodeId> sources;
    std::vector<csr::NodeId> sinks;
    std::vector<csr::ArcKeyBase> bases;
    native.reserve(indices.size());
    bases.reserve(indices.size());
    for (std::size_t index : indices) {
      const auto& arc = _database.get_arc_list().at(index);
      const auto& source = projectNode(arc.get_source_pin());
      const auto& sink = projectNode(arc.get_sink_pin());
      sources.push_back(source.id);
      sinks.push_back(sink.id);
      csr::detail::CanonicalArcKeyContext context{
          _database, _nodes, _node_ids, _ordered_pin_owners};
      bases.push_back(
          csr::detail::projectCanonicalArcKeyBase(context, arc));
      native.push_back(&arc);
      if constexpr (incregpusta_debug_checks_enabled)
        ++_run.records().audit.native_arc_candidate_reads;
    }
    const auto keys = csr::assignParallelOrdinals(bases);
    std::vector<detail::NativeCandidateArcOrderFact> result;
    result.reserve(keys.size());
    for (std::size_t index = 0; index < keys.size(); ++index) {
      csr::ArcRecord semantic;
      semantic.key = keys[index];
      semantic.source = sources[index];
      semantic.sink = sinks[index];
      semantic.kind = keys[index].base.kind;
      semantic.is_clock_arc = native[index]->get_is_clock_arc();
      semantic.is_disabled = native[index]->get_is_disable_arc();
      semantic.is_loop_disabled
          = native[index]->get_is_loop_disable();
      csr::detail::projectCanonicalArcTransfer(
          _database, _models, semantic, *native[index]);
      if (semantic.kind == csr::ArcKind::kCell
          && semantic.transfer.children().size()
                 != semantic.key.base.library_arcs.size())
        abortCapture(NarrowCaptureError::kMalformedNativeRecord);
      result.push_back({indices[index], semantic.key, semantic.source,
                        semantic.sink, semantic.is_clock_arc,
                        semantic.is_disabled, semantic.is_loop_disabled,
                        std::move(semantic.transfer), std::nullopt, semantic.native_cell_arc_ordinal});
    }
    return result;
  }
  csr::ArcSemanticPayload projectArcPayload(
      const detail::NativeCandidateArcOrderFact& projected,
      const AffectedArcCapture& request)
  {
    const auto& sink = _nodes.at(projected.sink.value());
    const auto* old = committedArcForRequest(request);
    csr::ArcRecord record;
    record.id = arcIdForRequest(request);
    record.key = projected.key;
    record.native_cell_arc_ordinal = projected.native_cell_arc_ordinal;
    record.source = projected.source;
    record.sink = projected.sink;
    record.kind = projected.key.base.kind;
    record.is_clock_arc = projected.is_clock_arc;
    record.is_disabled = projected.is_disabled;
    record.is_loop_disabled = projected.is_loop_disabled;
    bool sink_clock_reachable = false;
    bool replayed_clock_arc = false;
    if (old != nullptr) {
      replayed_clock_arc = old->eligibility.clock;
      const bool active = !old->is_disabled
                          && !old->is_loop_disabled;
      sink_clock_reachable
          = active && sink.is_sequential && sink.is_clock_pin
            && !old->eligibility.data_slew;
    } else if (sink.is_sequential && sink.is_clock_pin) {
      abortCapture(NarrowCaptureError::kUnsupportedPolicy);
    }
    record.eligibility = csr::detail::projectCanonicalArcEligibility(
        record, sink, sink_clock_reachable, replayed_clock_arc);
    record.transfer = projected.transfer;
    const auto load = _load_by_net.find(sink.net_name);
    if (load != _load_by_net.end()) {
      record.output_load = load->second;
    } else if (old != nullptr) {
      record.output_load = old->output_load;
    } else {
      abortCapture(NarrowCaptureError::kUnplannedDependency);
    }
    if (_native) {
      const auto binding = _native->staged.parasitics.find(record.sink);
      if (binding != _native->staged.parasitics.end()) {
        record.parasitic = binding->second;
        if (!record.parasitic.tree || record.parasitic.tree->net_name != sink.net_name
            || record.parasitic.terminal >= record.parasitic.tree->terminals.size()
            || record.parasitic.tree->terminals[record.parasitic.terminal] != record.sink
            || record.parasitic.tree->terminals.front() != (record.kind == csr::ArcKind::kNet ? record.source : record.sink)
            || record.output_load)
          abortCapture(NarrowCaptureError::kMalformedNativeRecord);
      } else if (!record.output_load)
        abortCapture(NarrowCaptureError::kUnplannedDependency);
    }
    return {record.key,
            record.source,
            record.sink,
            record.kind,
            record.is_clock_arc,
            record.is_disabled,
            record.is_loop_disabled,
            record.eligibility,
            record.transfer,
            record.output_load, record.native_cell_arc_ordinal, record.parasitic};
  }
  bool candidateMatchesRequest(const AffectedArcCapture& request,
                               const detail::NativeCandidateArcOrderFact&
                                   projected)
  {
    if (projected.key.base != request.expected_post_base)
      return false;
    const auto candidate = projectArcPayload(projected, request);
    const auto* old = committedArcForRequest(request);
    if (old == nullptr)
      return true;
    const auto delay = _delay_invalidations.find(request.ref);
    const auto delay_count = delay == _delay_invalidations.end() ? 0 : delay->second;
    if (delay_count > 1)
      abortCapture(NarrowCaptureError::kBatchBindingMismatch);
    const bool load_mutable = _load_by_net.contains(
        _nodes.at(candidate.sink.value()).net_name);
    return candidate.kind == old->kind
           && candidate.is_clock_arc == old->is_clock_arc
           && candidate.is_disabled == old->is_disabled
           && candidate.is_loop_disabled == old->is_loop_disabled
           && candidate.eligibility == old->eligibility
           && candidate.native_cell_arc_ordinal == old->native_cell_arc_ordinal
           && (delay_count == 1 || candidate.transfer == old->transfer)
           && (load_mutable
               || (candidate.output_load == old->output_load && candidate.parasitic == old->parasitic));
  }
  // For existing arcs candidateMatchesRequest has already fixed the native
  // identity. Only new, value-equal candidates with no old identity may use the
  // existing ArcId/native-order tie break; the assigned ordinal is retained.
  static bool semanticallyEquivalent(
      const detail::NativeCandidateArcOrderFact& lhs,
      const detail::NativeCandidateArcOrderFact& rhs)
  {
    return lhs.key.base == rhs.key.base && lhs.source == rhs.source
           && lhs.sink == rhs.sink
           && lhs.is_clock_arc == rhs.is_clock_arc
           && lhs.is_disabled == rhs.is_disabled
           && lhs.is_loop_disabled == rhs.is_loop_disabled
           && lhs.transfer == rhs.transfer;
  }
  void captureArcs()
  {
    auto projected = projectAffectedArcs();
    std::set<csr::ArcKeyBase, decltype(&csr::arcKeyBaseLess)> planned_bases(csr::arcKeyBaseLess);
    for (const auto& request : _plan.arcs)
      if (request.expected_post_multiplicity > 0)
        planned_bases.insert(request.expected_post_base);
    for (const auto& actual : projected) {
      const bool affected_load = _load_by_net.contains(
          _nodes.at(actual.sink.value()).net_name);
      const bool planned_base = planned_bases.contains(actual.key.base);
      if (affected_load && !planned_base)
        abortCapture(NarrowCaptureError::kArcMultiplicityMismatch);
    }
    std::vector<csr::ArcKey> candidate_keys;
    for (const auto& candidate : projected)
      candidate_keys.push_back(candidate.key);
    std::vector<csr::ArcId> request_ids;
    for (const auto& request : _plan.arcs)
      request_ids.push_back(arcIdForRequest(request));
    const auto matched = detail::matchNativeAffectedArcs(
        _plan.arcs, candidate_keys, request_ids,
        [&](std::size_t request, std::size_t candidate) {
          return candidateMatchesRequest(_plan.arcs[request],
                                         projected[candidate]);
        },
        [&](std::size_t lhs, std::size_t rhs) {
          return semanticallyEquivalent(projected[lhs], projected[rhs]);
        });
    if (const auto* error = std::get_if<NarrowCaptureError>(&matched))
      abortCapture(*error);
    const auto& bijection = std::get<detail::NativeArcBijection>(matched);
    if constexpr (incregpusta_debug_checks_enabled)
      _run.records().audit.arc_identity_tie_breaks = bijection.identity_tie_breaks;
    for (std::size_t request = 0; request < _plan.arcs.size(); ++request) {
      if (!bijection.candidate_by_request[request])
        continue;
      auto& assigned = projected[
          *bijection.candidate_by_request[request]].assigned_id;
      if (assigned)
        abortCapture(NarrowCaptureError::kDuplicateRequest);
      assigned = request_ids[request];
    }
    auto rows = detail::captureNativeAffectedArcRows(
        _database, _committed, _committed_index, _staged, _plan,
        projected);
    if (const auto* error = std::get_if<NarrowCaptureError>(&rows))
      abortCapture(*error);
    auto ordered = std::get<detail::NativeAffectedArcRows>(std::move(rows));
    _run.records().outgoing_rows = std::move(ordered.outgoing);
    _run.records().incoming_rows = std::move(ordered.incoming);
    std::set<std::uint64_t> captured_ids;
    for (std::size_t index = 0; index < _plan.arcs.size(); ++index) {
      const auto& request = _plan.arcs[index];
      const auto id = request_ids[index];
      if (!captured_ids.insert(id.value()).second)
        abortCapture(NarrowCaptureError::kDuplicateRequest);
      if (request.expected_post_multiplicity == 0) {
        const auto* old = committedArcForRequest(request);
        if (old == nullptr || !request.expected_current_key
            || old->key != *request.expected_current_key)
          abortCapture(NarrowCaptureError::kUnplannedDependency);
        _run.records().arcs.push_back(
            {request.ref, id, false, *old});
        if constexpr (incregpusta_debug_checks_enabled)
          ++_run.records().audit.deactivated_arc_before_reads;
        if constexpr (incregpusta_debug_checks_enabled)
          ++_run.records().audit.accepted_deactivations;
        if constexpr (incregpusta_debug_checks_enabled)
          ++_run.records().audit.captured_arc_records;
        continue;
      }
      if (!bijection.candidate_by_request[index])
        abortCapture(NarrowCaptureError::kArcMultiplicityMismatch);
      const auto& candidate
          = projected[*bijection.candidate_by_request[index]];
      auto payload = projectArcPayload(candidate, request);
      if (payload.key.base != request.expected_post_base)
        abortCapture(NarrowCaptureError::kArcKeyMismatch);
      _run.records().arcs.push_back(
          {request.ref, id, true, std::move(payload)});
      if constexpr (incregpusta_debug_checks_enabled)
        ++_run.records().audit.accepted_arcs;
      if constexpr (incregpusta_debug_checks_enabled)
        ++_run.records().audit.captured_arc_records;
    }
    if (_run.records().arcs.size() != _plan.arcs.size())
      abortCapture(NarrowCaptureError::kArcMultiplicityMismatch);
  }
  const ista::Database& _database;
  csr::detail::NativeLibraryModels& _models;
  const DualSemanticReadView& _committed;
  const DualCsrArcGroupIndex& _committed_index;
  timing_edit::TimingEditResolverView _staged;
  const AffectedCapturePlan& _plan;
  CaptureRun& _run;
  const timing_edit::NativeTargetedEditBatch* _native;
  std::map<timing_edit::StableArcRef, std::size_t> _delay_invalidations;
  detail::PlannedNodeCaptureIndex _planned_nodes;
  std::unordered_map<std::string_view, std::size_t> _starts, _ends;
  std::map<csr::NodeId, std::size_t> _captured_capacitances;
  std::map<csr::NodeId, std::pair<std::size_t, std::optional<double>>> _port_loads;
  std::vector<csr::NodeRecord> _nodes;
  std::map<std::string, csr::NodeRecord> _node_by_name;
  std::map<std::string, csr::NodeId> _node_ids;
  std::map<std::uint64_t, std::string> _node_name_by_id;
  std::unordered_map<std::string, std::string> _ordered_pin_owners;
  std::map<std::string, const csr::ReferencedLibraryCellModel*> _library_by_name;
  std::map<std::string, std::optional<csr::ArcOutputLoadMatrix>> _load_by_net;
};
}
detail::NativeCaptureResult detail::captureNativeDualCsrPayload(
    const ista::Database& database, csr::detail::NativeLibraryModels& models, const DualSemanticReadView& committed,
    const DualCsrArcGroupIndex& committed_index,
    timing_edit::TimingEditResolverView staged,
    const AffectedCapturePlan& plan, const timing_edit::NativeTargetedEditBatch* native)
{
  CaptureRun run(plan);
  try {
    CaptureBuilder{database, models, committed, committed_index, staged, plan,
                   run, native}.build();
    return run.succeed();
  } catch (const CaptureAbort& error) {
    return run.fail(error.error);
  } catch (...) {
    return run.fail(NarrowCaptureError::kMalformedNativeRecord);
  }
}
}  // namespace ista::timing_dual_csr
