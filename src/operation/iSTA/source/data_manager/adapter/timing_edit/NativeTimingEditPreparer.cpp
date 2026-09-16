// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeTimingEditPreparer.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "Database.hpp"
#include "NativeBufferCellValidation.hpp"
#include "NativeBufferInsertionInternal.hpp"
#include "TimingEditCanonicalEncoding.hpp"
#include "TimingEditResolver.hpp"
#include "TimingEditValidation.hpp"
#include "TimingGraphCsrAdapter.hpp"

namespace ista::timing_edit {
namespace {

using Bytes = std::vector<std::uint8_t>;

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("native timing edit prepare: " + std::move(message));
}

[[noreturn]] void invalidContract(const TimingEditContractViolations& errors)
{
  if (errors.empty()) {
    invalid("contract normalization failed without a violation");
  }
  invalid(errors.front().field + ": " + errors.front().message);
}

struct CurrentCapture
{
  csr::CsrReferenceInput input;
  CommittedEditIdentity identity;
};

ComponentFingerprints fingerprintsOf(const csr::CsrSnapshotIdentity& identity)
{
  return {.schema_version = identity.fingerprint_schema_version,
          .topology = identity.topology_fingerprint,
          .clock_constraint = identity.clock_constraint_fingerprint,
          .library = identity.library_fingerprint,
          .corner = identity.corner_fingerprint,
          .parasitic = identity.parasitic_fingerprint,
          .timing_input = identity.timing_input_fingerprint};
}

CanonicalPreStateEvidence evidenceOf(const Database& database, const csr::CsrReferenceInput& input)
{
  CanonicalPreStateEvidence evidence;
  evidence.instances.reserve(database.get_instance_map().size());
  for (const auto& [name, instance] : database.get_instance_map()) {
    static_cast<void>(instance);
    evidence.instances.push_back(name);
  }
  evidence.nets.reserve(database.get_net_map().size());
  for (const auto& [name, net] : database.get_net_map()) {
    static_cast<void>(net);
    evidence.nets.push_back(name);
  }
  evidence.nodes.reserve(input.nodes.size());
  for (const auto& node : input.nodes) {
    evidence.nodes.push_back(node.key);
  }
  evidence.arcs.reserve(input.arcs.size());
  for (const auto& arc : input.arcs) {
    evidence.arcs.push_back(arc.key);
  }
  std::ranges::sort(evidence.nodes, csr::nodeKeyLess);
  std::ranges::sort(evidence.arcs, csr::arcKeyLess);
  return evidence;
}

CurrentCapture capture(const Database& database, TransactionGeneration generation,
                       const csr::StaAnalysisContext& analysis_context, TimingEditSessionToken session_token)
{
  if (session_token.value() == 0) {
    invalid("session token must be nonzero");
  }
  csr::CsrReferenceInputExportResult exported
      = csr::exportCsrReferenceInputCandidate(database, analysis_context, session_token.value());
  auto* input = std::get_if<csr::CsrReferenceInput>(&exported);
  if (input == nullptr) {
    const auto& failure = std::get<csr::CsrReferenceInputExportFailure>(exported);
    invalid("active CSR re-export failed at phase " + std::to_string(static_cast<std::uint8_t>(failure.failed_phase)));
  }

  CurrentCapture result{std::move(*input), {}};
  result.identity.generation = generation;
  result.identity.current_fingerprints = fingerprintsOf(result.input.source_identity);
  result.identity.canonical_pre_state = evidenceOf(database, result.input);
  if (auto errors = validateCommittedEditIdentity(result.identity, "captured_current_identity"); !errors.empty()) {
    invalidContract(errors);
  }
  return result;
}

DesignEditPlan requireNormalizedPlan(DesignEditPlan plan)
{
  DesignEditPlanNormalization normalized = normalizeDesignEditPlan(std::move(plan));
  if (const auto* errors = std::get_if<TimingEditContractViolations>(&normalized)) {
    invalidContract(*errors);
  }
  return std::get<DesignEditPlan>(std::move(normalized));
}

TimingGraphDelta requireNormalizedDelta(TimingGraphDelta delta, std::string_view field)
{
  TimingGraphDeltaNormalization normalized = normalizeTimingGraphDelta(std::move(delta), field);
  if (const auto* errors = std::get_if<TimingEditContractViolations>(&normalized)) {
    invalidContract(*errors);
  }
  return std::get<TimingGraphDelta>(std::move(normalized));
}

bool isDriver(const Pin& pin)
{
  return pin.get_is_port() ? pin.get_direction() == PinDirection::kInput : pin.get_direction() == PinDirection::kOutput;
}

void requireDataNet(const Database& database, const csr::CsrReferenceInput& input, const Net& net)
{
  const std::string& net_name = net.get_net_name();
  if (std::ranges::any_of(input.arcs, [&](const csr::ArcRecord& arc) {
        return arc.key.base.kind == csr::ArcKind::kNet && arc.key.base.owner_name == net_name && arc.eligibility.clock;
      })) {
    invalid("clock net is outside the Stage 3 data-only boundary");
  }

  for (const auto& [clock_key, clock] : database.get_timing_constraint().get_clock_map()) {
    static_cast<void>(clock_key);
    if (std::ranges::any_of(clock.get_source_list(), [&](const std::string& source) {
          return std::ranges::find(net.get_pin_name_list(), source) != net.get_pin_name_list().end();
        })) {
      invalid("clock-source net is outside the Stage 3 data-only boundary");
    }
  }

  for (const std::string& pin_name : net.get_pin_name_list()) {
    const auto pin = database.get_pin_map().find(pin_name);
    if (pin == database.get_pin_map().end()) {
      invalid("source net contains an unresolved pin while checking its data-only boundary");
    }
    if (pin->second.get_is_port()) {
      continue;
    }
    const auto instance = database.get_instance_map().find(pin->second.get_instance_name());
    if (instance == database.get_instance_map().end()) {
      invalid("source net contains a pin with an unresolved instance");
    }
    const auto cell = database.get_timing_library().get_cell_map().find(instance->second.get_cell_name());
    if (cell == database.get_timing_library().get_cell_map().end()) {
      invalid("source net contains a pin with an unresolved library cell");
    }
    const auto port = cell->second.get_port_map().find(pin->second.get_pin_name());
    if (port == cell->second.get_port_map().end()) {
      invalid("source net contains a pin with an unresolved library port");
    }
    if (port->second.get_is_clock()) {
      invalid("net with a library clock port is outside the Stage 3 data-only boundary");
    }
  }
}

std::string requireSingleDriver(const Database& database, const Net& net)
{
  std::string driver;
  std::set<std::string> members;
  for (const auto& pin_name : net.get_pin_name_list()) {
    if (!members.insert(pin_name).second) {
      invalid("source net raw membership contains a duplicate pin");
    }
    const auto pin = database.get_pin_map().find(pin_name);
    if (pin == database.get_pin_map().end() || pin->second.get_net_name() != net.get_net_name()) {
      invalid("source net raw membership is not bidirectional");
    }
    if (!isDriver(pin->second)) {
      continue;
    }
    if (!driver.empty()) {
      invalid("source net has multiple drivers");
    }
    driver = pin_name;
  }
  if (driver.empty()) {
    invalid("source net has no driver");
  }
  return driver;
}

void requireMaterializationAbsent(const Database& database, const TimingEditResolverState& resolver,
                                  const TokenMaterialization& value, std::set<std::string>& batch_names)
{
  if (resolver.materializations().contains(value.creation_token)) {
    invalid("creation token was already materialized");
  }
  for (const auto* name : {&value.instance_name, &value.output_net_name, &value.input_pin_name, &value.output_pin_name}) {
    if (!batch_names.insert(*name).second || database.get_instance_map().contains(*name) || database.get_net_map().contains(*name)
        || database.get_pin_map().contains(*name)) {
      invalid("canonical token materialization collides with a live native name");
    }
  }
}

const csr::ArcKey& requireCurrentNetArc(const csr::CsrReferenceInput& input, const std::string& net_name,
                                       const std::string& driver_name, const std::string& sink_name)
{
  const csr::ArcKey* result = nullptr;
  for (const auto& arc : input.arcs) {
    const auto& key = arc.key;
    if (arc.kind != csr::ArcKind::kNet || key.base.kind != csr::ArcKind::kNet || key.base.owner_name != net_name
        || key.base.source.full_name != driver_name || key.base.sink.full_name != sink_name) {
      continue;
    }
    if (result != nullptr) {
      invalid("current CSR graph has multiple net arcs for one driver/sink connection");
    }
    result = &key;
  }
  if (result == nullptr) {
    invalid("current CSR graph is missing the moved sink net arc");
  }
  return *result;
}

bool matchesCurrentArc(const StableArcRef& candidate, const csr::ArcKey& current_key, const StableNetRef& net,
                       const StableNodeRef& driver, const StableNodeRef& sink)
{
  return std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref, ExistingArcRef>) {
          return ref.baseline_key == current_key;
        } else if constexpr (std::is_same_v<Ref, CreatedOldNetArcRef>) {
          return ref.net == net && ref.driver == driver && ref.sink == sink;
        } else if constexpr (std::is_same_v<Ref, CreatedNewNetArcRef>) {
          const StableNetRef created_net{CreatedNetRef{ref.creation_token, CreatedNetRole::kOutputNet}};
          return created_net == net && ref.driver == driver && ref.sink == sink;
        } else {
          return false;
        }
      },
      candidate);
}

StableArcRef requireCurrentStableArc(const TimingEditResolverState& resolver, const csr::ArcKey& current_key,
                                     const StableNetRef& net, const StableNodeRef& driver, const StableNodeRef& sink)
{
  const StableArcRef* result = nullptr;
  for (const auto& [ref, binding] : resolver.arcBindings()) {
    if (binding.tombstone || !matchesCurrentArc(ref, current_key, net, driver, sink)) {
      continue;
    }
    if (result != nullptr) {
      invalid("current net arc has multiple live stable resolver bindings");
    }
    result = &ref;
  }
  if (result == nullptr) {
    invalid("current net arc has no live stable resolver binding");
  }
  return *result;
}

AppendOnlySlot appendSlot(std::uint64_t& cursor)
{
  if (cursor == std::numeric_limits<std::uint64_t>::max()) {
    invalid("append-only slot capacity is exhausted");
  }
  return AppendOnlySlot{cursor++};
}

template <typename Ref, typename Encoder>
void canonicalizeAssignments(std::vector<SlotAssignment<Ref>>& assignments, Encoder encode)
{
  std::ranges::sort(assignments, [&](const auto& lhs, const auto& rhs) { return encode(lhs.ref) < encode(rhs.ref); });
}

template <typename Ref, typename Encoder>
void canonicalizeRefs(std::vector<Ref>& refs, Encoder encode)
{
  std::ranges::sort(refs, [&](const Ref& lhs, const Ref& rhs) { return encode(lhs) < encode(rhs); });
  refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
}

InvalidationClosure closureFrom(const TimingGraphDelta& delta)
{
  InvalidationClosure closure;
  for (const auto& operation : delta.operations) {
    std::visit(
        [&](const auto& value) {
          using Operation = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Operation, InvalidateLoad>) {
            closure.load_nets.push_back(value.net);
          } else if constexpr (std::is_same_v<Operation, InvalidateParasitic>) {
            closure.parasitic_nets.push_back(value.net);
          } else if constexpr (std::is_same_v<Operation, InvalidateArcDelay>) {
            closure.arc_delays.push_back(value.arc);
          }
        },
        operation);
  }
  canonicalizeRefs(closure.load_nets, canonicalStableNetRefBytes);
  canonicalizeRefs(closure.parasitic_nets, canonicalStableNetRefBytes);
  canonicalizeRefs(closure.arc_delays, canonicalStableArcRefBytes);
  return closure;
}

void invalidateNet(TimingGraphDelta& delta, const StableNetRef& net)
{
  delta.operations.emplace_back(InvalidateLoad{net});
  delta.operations.emplace_back(InvalidateParasitic{net});
}

void invalidateArc(TimingGraphDelta& delta, const StableArcRef& arc)
{
  delta.operations.emplace_back(InvalidateArcDelay{arc});
}

struct BatchBuilder
{
  explicit BatchBuilder(const AppendOnlyHighWater& before)
      : slots{.before = before, .after = before, .nodes = {}, .nets = {}, .arcs = {}},
        next_node(before.node_slots),
        next_net(before.net_slots),
        next_arc(before.arc_slots)
  {
  }

  TimingGraphDelta forward;
  TimingGraphDelta reverse;
  AppendOnlySlotPlan slots;
  std::vector<TokenMaterialization> materializations;
  std::uint64_t next_node;
  std::uint64_t next_net;
  std::uint64_t next_arc;
  bool requires_full{false};
};

void appendExpansion(BatchBuilder& builder, const Database& database, const csr::CsrReferenceInput& input,
                     const TimingEditResolverState& resolver, const BufferInsertionMutation& edit,
                     std::set<std::string>& materialized_names)
{
  static_cast<void>(requireLegalNativeBufferCell(database, edit.library_cell));
  const TokenMaterialization materialization = defaultTokenMaterialization(edit.creation_token, edit.library_cell);
  requireMaterializationAbsent(database, resolver, materialization, materialized_names);
  builder.materializations.push_back(materialization);

  const std::string source_net_name = resolver.resolveNet(edit.original_net);
  if (resolver.stableNetForNativeName(source_net_name) != edit.original_net) {
    invalid("original net reference is not the unique live stable binding for its native name");
  }
  const auto source_net = database.get_net_map().find(source_net_name);
  if (source_net == database.get_net_map().end()) {
    invalid("original net does not exist in the active Database");
  }
  if (std::holds_alternative<NoParasiticRcInputs>(edit.rc_inputs)
      && database.get_parasitic_library().get_net_map().contains(source_net_name)) {
    invalid("NoParasitic input disagrees with the active parasitic library");
  }
  if (const auto* rc = std::get_if<ParasiticRcInputs>(&edit.rc_inputs)) {
    if (rc->parasitic_policy != ParasiticEditPolicy::kFullWithRecord) {
      invalid("unsupported parasitic edit policy reached native expansion");
    }
    builder.requires_full = true;
  }
  requireDataNet(database, input, source_net->second);

  const std::string driver_name = requireSingleDriver(database, source_net->second);
  const StableNodeRef driver = resolver.stableNodeForNativeName(driver_name);
  const CreatedNodeRef input_node{edit.creation_token, CreatedNodeRole::kInput};
  const CreatedNodeRef output_node{edit.creation_token, CreatedNodeRole::kOutput};
  const StableNodeRef stable_input{input_node};
  const StableNodeRef stable_output{output_node};
  const StableNetRef output_net{CreatedNetRef{edit.creation_token, CreatedNetRole::kOutputNet}};
  const CreatedCellArcRef cell_arc{edit.creation_token, edit.library_cell.input_port, edit.library_cell.output_port,
                                   edit.library_cell.ordered_arc_signatures, 0};
  const StableArcRef stable_cell_arc{cell_arc};
  const StableArcRef driver_to_input{CreatedOldNetArcRef{edit.original_net, driver, stable_input, 0}};

  builder.forward.operations.emplace_back(AddInstance{edit.creation_token, edit.library_cell, edit.placement_inputs});
  builder.forward.operations.emplace_back(AddTimingPoint{input_node, csr::PinDirection::kInput});
  builder.forward.operations.emplace_back(AddTimingPoint{output_node, csr::PinDirection::kOutput});
  builder.forward.operations.emplace_back(AddCellArc{cell_arc});
  builder.forward.operations.emplace_back(AddNetArc{driver_to_input});
  builder.reverse.operations.emplace_back(RemoveInstance{edit.creation_token});
  builder.reverse.operations.emplace_back(RemoveTimingPoint{input_node});
  builder.reverse.operations.emplace_back(RemoveTimingPoint{output_node});
  builder.reverse.operations.emplace_back(RemoveCellArc{cell_arc});
  builder.reverse.operations.emplace_back(RemoveNetArc{driver_to_input});

  builder.slots.nodes.push_back({stable_input, appendSlot(builder.next_node), false});
  builder.slots.nodes.push_back({stable_output, appendSlot(builder.next_node), false});
  builder.slots.nets.push_back({output_net, appendSlot(builder.next_net), false});
  builder.slots.arcs.push_back({stable_cell_arc, appendSlot(builder.next_arc), false});
  builder.slots.arcs.push_back({driver_to_input, appendSlot(builder.next_arc), false});

  invalidateNet(builder.forward, edit.original_net);
  invalidateNet(builder.forward, output_net);
  invalidateNet(builder.reverse, edit.original_net);
  invalidateNet(builder.reverse, output_net);
  invalidateArc(builder.forward, stable_cell_arc);
  invalidateArc(builder.forward, driver_to_input);
  invalidateArc(builder.reverse, stable_cell_arc);
  invalidateArc(builder.reverse, driver_to_input);

  for (const auto& sink : edit.moved_sinks) {
    const std::string sink_name = resolver.resolveNode(sink);
    if (resolver.stableNodeForNativeName(sink_name) != sink) {
      invalid("moved sink is not the unique live stable binding for its native name");
    }
    const auto native_sink = database.get_pin_map().find(sink_name);
    if (native_sink == database.get_pin_map().end() || native_sink->second.get_net_name() != source_net_name
        || isDriver(native_sink->second)
        || std::ranges::count(source_net->second.get_pin_name_list(), sink_name) != 1) {
      invalid("moved sink is not exactly one load member of the original net");
    }
    detail::requireSupportedMovedSink(database, native_sink->second);

    const csr::ArcKey& current_key = requireCurrentNetArc(input, source_net_name, driver_name, sink_name);
    const StableArcRef old_arc = requireCurrentStableArc(resolver, current_key, edit.original_net, driver, sink);
    const StableArcRef new_arc{CreatedNewNetArcRef{edit.creation_token, stable_output, sink, 0}};

    builder.forward.operations.emplace_back(RemoveNetArc{old_arc});
    builder.forward.operations.emplace_back(AddNetArc{new_arc});
    builder.forward.operations.emplace_back(ReconnectSink{sink, edit.original_net, output_net});
    builder.reverse.operations.emplace_back(AddNetArc{old_arc});
    builder.reverse.operations.emplace_back(RemoveNetArc{new_arc});
    builder.reverse.operations.emplace_back(ReconnectSink{sink, output_net, edit.original_net});

    builder.slots.arcs.push_back({old_arc, resolver.resolveArcSlot(old_arc), true});
    builder.slots.arcs.push_back({new_arc, appendSlot(builder.next_arc), false});
    invalidateArc(builder.forward, old_arc);
    invalidateArc(builder.forward, new_arc);
    invalidateArc(builder.reverse, old_arc);
    invalidateArc(builder.reverse, new_arc);
  }
}

bool matchesCurrentGraphArc(const StableArcRef& candidate, const csr::ArcKey& key, const TimingEditResolverState& resolver)
{
  return std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref, ExistingArcRef>) {
          return ref.baseline_key == key;
        } else if constexpr (std::is_same_v<Ref, CreatedCellArcRef>) {
          if (key.base.kind != csr::ArcKind::kCell || ref.stable_equivalence_ordinal != key.parallel_ordinal) {
            return false;
          }
          const auto& materialization = resolver.materialization(ref.creation_token);
          return key.base.owner_name == materialization.instance_name && key.base.source.full_name == materialization.input_pin_name
                 && key.base.sink.full_name == materialization.output_pin_name
                 && key.base.library_source_port == ref.library_source_port && key.base.library_sink_port == ref.library_sink_port
                 && key.base.library_arcs == ref.ordered_library_arcs;
        } else if constexpr (std::is_same_v<Ref, CreatedOldNetArcRef>) {
          return key.base.kind == csr::ArcKind::kNet && ref.stable_ordinal == key.parallel_ordinal
                 && resolver.resolveNet(ref.net) == key.base.owner_name && resolver.resolveNode(ref.driver) == key.base.source.full_name
                 && resolver.resolveNode(ref.sink) == key.base.sink.full_name;
        } else {
          const auto& materialization = resolver.materialization(ref.creation_token);
          return key.base.kind == csr::ArcKind::kNet && ref.stable_ordinal == key.parallel_ordinal
                 && materialization.output_net_name == key.base.owner_name
                 && resolver.resolveNode(ref.driver) == key.base.source.full_name
                 && resolver.resolveNode(ref.sink) == key.base.sink.full_name;
        }
      },
      candidate);
}

void requireResolverCoverage(const Database& database, const csr::CsrReferenceInput& input,
                             const TimingEditResolverState& resolver)
{
  std::size_t live_nodes = 0;
  for (const auto& [ref, binding] : resolver.nodeBindings()) {
    static_cast<void>(ref);
    live_nodes += !binding.tombstone;
  }
  std::size_t live_nets = 0;
  for (const auto& [ref, binding] : resolver.netBindings()) {
    static_cast<void>(ref);
    live_nets += !binding.tombstone;
  }
  std::size_t live_arcs = 0;
  for (const auto& [ref, binding] : resolver.arcBindings()) {
    static_cast<void>(ref);
    live_arcs += !binding.tombstone;
  }
  if (live_nodes != database.get_pin_map().size() || live_nets != database.get_net_map().size()
      || live_arcs != input.arcs.size()) {
    invalid("resolver live node/net/arc cardinality differs from the active CSR graph");
  }

  for (const auto& [name, pin] : database.get_pin_map()) {
    static_cast<void>(pin);
    static_cast<void>(resolver.stableNodeForNativeName(name));
  }
  for (const auto& [name, net] : database.get_net_map()) {
    static_cast<void>(net);
    static_cast<void>(resolver.stableNetForNativeName(name));
  }
  for (const auto& arc : input.arcs) {
    std::size_t matches = 0;
    for (const auto& [ref, binding] : resolver.arcBindings()) {
      if (!binding.tombstone && matchesCurrentGraphArc(ref, arc.key, resolver)) {
        ++matches;
      }
    }
    if (matches != 1) {
      invalid("current CSR arc does not have exactly one live stable resolver binding");
    }
  }
}

}  // namespace

CommittedEditIdentity NativeTimingEditPreparer::captureCurrentIdentity(const Database& active_database,
                                                                        TransactionGeneration generation,
                                                                        const csr::StaAnalysisContext& analysis_context,
                                                                        TimingEditSessionToken session_token)
{
  return capture(active_database, generation, analysis_context, session_token).identity;
}

bool NativeTimingEditPreparer::matchesDesignName(Database& active_database, std::string_view expected_design_name)
{
  return active_database.get_design_name() == expected_design_name;
}

void NativeTimingEditPreparer::requireExactResolverCoverage(const Database& active_database,
                                                            const csr::CsrReferenceInput& current_input,
                                                            const TimingEditResolverState& resolver)
{
  requireResolverCoverage(active_database, current_input, resolver);
}

PreparedBatch NativeTimingEditPreparer::prepare(const Database& active_database, DesignEditPlan plan,
                                                 const TimingEditPrepareContext& context,
                                                 const TimingEditResolverState& resolver)
{
  DesignEditPlan normalized_plan = requireNormalizedPlan(std::move(plan));
  if (normalized_plan.baseline != context.session_baseline) {
    invalid("plan baseline identity does not match the active session baseline");
  }
  if (normalized_plan.expected_generation != context.current_identity.generation) {
    invalid("plan generation does not match the committed generation");
  }

  // This re-export is the authoritative pre-write stale-state gate. Equality
  // includes exact canonical names/keys as well as component fingerprints.
  CurrentCapture current = capture(active_database, context.current_identity.generation, context.analysis_context,
                                   context.session_token);
  if (current.identity != context.current_identity) {
    invalid("active native state no longer matches the committed identity");
  }
  requireResolverCoverage(active_database, current.input, resolver);

  BatchBuilder builder(resolver.highWater());
  std::set<std::string> materialized_names;
  for (const auto& edit : normalized_plan.edits) {
    appendExpansion(builder, active_database, current.input, resolver, edit, materialized_names);
  }
  builder.slots.after = {builder.next_node, builder.next_net, builder.next_arc};
  canonicalizeAssignments(builder.slots.nodes, canonicalStableNodeRefBytes);
  canonicalizeAssignments(builder.slots.nets, canonicalStableNetRefBytes);
  canonicalizeAssignments(builder.slots.arcs, canonicalStableArcRefBytes);
  builder.forward = requireNormalizedDelta(std::move(builder.forward), "prepared.normalized_forward");
  builder.reverse = requireNormalizedDelta(std::move(builder.reverse), "prepared.normalized_reverse");

  PolicyReceipt policy;
  if (builder.requires_full) {
    policy.disposition = EditDisposition::kFullWithRecord;
    policy.reasons = {"source net parasitics require a recorded full rebuild"};
  }

  PreparedBatchData data;
  data.session_token = context.session_token;
  data.expected_generation = normalized_plan.expected_generation;
  data.target_generation = TransactionGeneration{normalized_plan.expected_generation.value() + 1};
  data.baseline = normalized_plan.baseline;
  data.expected_pre_committed_identity = context.current_identity;
  data.normalized_edits = std::move(normalized_plan.edits);
  data.normalized_forward = std::move(builder.forward);
  data.normalized_reverse = std::move(builder.reverse);
  data.slot_plan = std::move(builder.slots);
  data.token_materializations = std::move(builder.materializations);
  data.invalidation_closure = closureFrom(data.normalized_forward);
  data.policy_receipt = std::move(policy);

  PreparedBatch prepared = PreparedBatch::create(std::move(data));
  static_cast<void>(resolver.stage(prepared));
  return prepared;
}

}  // namespace ista::timing_edit
