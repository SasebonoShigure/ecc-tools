// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "DualCsrPropertySupport.hpp"

namespace ista::timing_dual_csr::test {
namespace {

constexpr std::size_t kSampleCount = 24;
constexpr std::array<std::uint64_t, 5> kXorShiftSeeds{
    1ULL, 7ULL, 42ULL, 20260901ULL, 0xdeadbeefULL};

struct Random
{
  std::uint64_t state;

  std::uint64_t next() noexcept
  {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }

  std::size_t pick(std::size_t bound)
  {
    require(bound != 0, "random bound must be nonzero");
    return static_cast<std::size_t>(next() % bound);
  }
};

std::string nodeName(std::size_t id)
{
  return "p" + std::string(id < 10 ? "0" : "")
         + std::to_string(id) + "_node";
}

csr::NodeRecord portNode(std::size_t id,
                         const std::string& net,
                         csr::PinDirection direction)
{
  csr::NodeRecord result;
  result.id = csr::NodeId{id};
  result.key = nodeKey(nodeName(id));
  result.direction = direction;
  result.net_name = net;
  result.is_port = true;
  result.is_startpoint = direction == csr::PinDirection::kInput;
  result.is_endpoint = direction == csr::PinDirection::kOutput;
  return result;
}

csr::ArcSemanticPayload netPayload(
    const csr::NodeRecord& source, const csr::NodeRecord& sink,
    double load, std::uint64_t ordinal = 0)
{
  auto result = arcPayload(source.key, sink.key, source.id, sink.id,
                           ordinal);
  result.key.base.owner_name = source.net_name;
  result.output_load = csr::test::outputLoad(load);
  return result;
}

csr::ArcRecord arcRecordFrom(const csr::ArcSemanticPayload& payload,
                             std::size_t id)
{
  csr::ArcRecord result;
  result.id = csr::ArcId{id};
  result.key = payload.key;
  result.source = payload.source;
  result.sink = payload.sink;
  result.kind = payload.kind;
  result.native_traversal_rank = id;
  result.is_clock_arc = payload.is_clock_arc;
  result.is_disabled = payload.is_disabled;
  result.is_loop_disabled = payload.is_loop_disabled;
  result.eligibility = payload.eligibility;
  result.transfer = payload.transfer;
  result.output_load = payload.output_load;
  return result;
}

struct Group
{
  csr::NodeId source;
  std::vector<csr::NodeId> loads;
  std::string net;
  double output_load{0.0};
};

struct Generated
{
  DualCsrState state;
  PropertyOracle oracle;
  std::vector<Group> groups;
};

Generated generateBaseline(Random& random)
{
  const std::size_t node_count = 3 + random.pick(10);
  const std::size_t max_groups
      = std::min<std::size_t>(3, 1 + (node_count - 3) / 2);
  const std::size_t group_count = 1 + random.pick(max_groups);
  std::vector<std::size_t> sizes(group_count, 2);
  sizes[0] = 3;
  std::size_t assigned = 3 + 2 * (group_count - 1);
  while (assigned++ < node_count)
    ++sizes[random.pick(group_count)];

  csr::CsrReferenceInput input = csr::test::referenceInput();
  input.nodes.clear();
  input.node_ids_by_key.clear();
  input.instances.clear();
  input.arcs.clear();
  input.arc_ids_by_key.clear();
  input.out_row_offsets.clear();
  input.out_arc_ids.clear();
  input.in_row_offsets.clear();
  input.in_arc_ids.clear();
  input.ordered_root_ids.clear();
  input.ordered_start_ids.clear();
  input.ordered_end_ids.clear();
  input.clocks.clear();
  input.port_constraints.clear();
  input.case_analysis_constraints.clear();
  input.timing_check_endpoints.clear();
  input.referenced_library_cells.clear();
  input.start_seeds.clear();
  input.endpoint_required.clear();
  input.cell_pin_capacitances.clear();
  input.unsupported_reasons.clear();
  input.parasitics = {};

  std::vector<Group> groups;
  std::size_t cursor = 0;
  for (std::size_t group = 0; group < group_count; ++group) {
    Group generated;
    generated.source = csr::NodeId{cursor};
    generated.net = "net_" + std::to_string(group);
    generated.output_load
        = 0.25 * static_cast<double>(1 + random.pick(4));
    input.nodes.push_back(portNode(cursor, generated.net,
                                   csr::PinDirection::kInput));
    ++cursor;
    for (std::size_t load = 1; load < sizes[group]; ++load) {
      generated.loads.emplace_back(cursor);
      input.nodes.push_back(portNode(cursor, generated.net,
                                     csr::PinDirection::kOutput));
      ++cursor;
    }
    groups.push_back(std::move(generated));
  }
  for (const auto& record : input.nodes)
    input.node_ids_by_key.push_back({record.key, record.id});

  for (const auto& group : groups) {
    input.ordered_root_ids.push_back(group.source);
    input.ordered_start_ids.push_back(group.source);
    csr::PortConstraintModel source_constraint;
    const auto& source = input.nodes[group.source.value()];
    const double arrival
        = 0.5 + 0.25 * static_cast<double>(random.pick(4));
    const double slew
        = 0.1 + 0.05 * static_cast<double>(random.pick(3));
    source_constraint.map_key = source.key.full_name;
    source_constraint.embedded_port_name = source.key.full_name;
    source_constraint.resolved_node = group.source;
    source_constraint.input_delay = {arrival, arrival};
    source_constraint.input_transition = slew;
    input.port_constraints.push_back(source_constraint);

    csr::StartSeedModel seed;
    seed.seed_node = group.source;
    seed.path_start_node = group.source;
    seed.path_source = csr::PathSource::kInput;
    seed.clock_tag = "clk";
    for (std::size_t analysis = 0; analysis < csr::kAnalysisCount;
         ++analysis)
      for (std::size_t transition = 0;
           transition < csr::kTransitionCount; ++transition) {
        seed.arrival[analysis][transition] = arrival;
        seed.slew[analysis][transition] = slew;
      }
    input.start_seeds.push_back(std::move(seed));

    csr::PortConstraintModel load_constraint;
    const auto first_load = group.loads.front();
    const auto& load_record = input.nodes[first_load.value()];
    load_constraint.map_key = load_record.key.full_name;
    load_constraint.embedded_port_name = load_record.key.full_name;
    load_constraint.resolved_node = first_load;
    load_constraint.output_load = group.output_load;
    input.port_constraints.push_back(std::move(load_constraint));
    for (const auto load : group.loads) {
      input.ordered_end_ids.push_back(load);
      csr::EndpointRequiredModel endpoint;
      endpoint.node = load;
      endpoint.clock_name = "clk";
      input.endpoint_required.push_back(std::move(endpoint));
      const auto payload = netPayload(source, input.nodes[load.value()],
                                      group.output_load);
      const std::size_t id = input.arcs.size();
      input.arcs.push_back(arcRecordFrom(payload, id));
      input.arc_ids_by_key.push_back({payload.key, csr::ArcId{id}});
    }
  }
  std::ranges::sort(input.port_constraints, {},
                    &csr::PortConstraintModel::map_key);
  std::ranges::sort(input.ordered_end_ids, [](csr::NodeId lhs,
                                             csr::NodeId rhs) {
    return lhs.value() < rhs.value();
  });
  input.clock_tags = {"clk"};
  input.clock_owner_by_node.assign(input.nodes.size(), std::nullopt);

  std::vector<std::vector<csr::ArcId>> outgoing(input.nodes.size());
  std::vector<std::vector<csr::ArcId>> incoming(input.nodes.size());
  for (const auto& arc : input.arcs) {
    outgoing[arc.source.value()].push_back(arc.id);
    incoming[arc.sink.value()].push_back(arc.id);
  }
  input.out_row_offsets.push_back(0);
  input.in_row_offsets.push_back(0);
  for (std::size_t node = 0; node < input.nodes.size(); ++node) {
    input.out_arc_ids.insert(input.out_arc_ids.end(),
                             outgoing[node].begin(), outgoing[node].end());
    input.in_arc_ids.insert(input.in_arc_ids.end(),
                            incoming[node].begin(), incoming[node].end());
    input.out_row_offsets.push_back(input.out_arc_ids.size());
    input.in_row_offsets.push_back(input.in_arc_ids.size());
  }
  csr::test::refreshIdentity(input, kSessionValue);

  auto attempt = DualCsrBaselineFactory::create(
      input, timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{input.nodes.size(), 0,
                                       input.arcs.size()});
  require(attempt.success(),
          "random baseline rejected at "
              + (attempt.violations.empty()
                     ? std::string{"unknown"}
                     : attempt.violations.front().field + ": "
                           + attempt.violations.front().message));
  PropertyOracle oracle;
  oracle.nodes = input.nodes;
  for (const auto& arc : input.arcs)
    oracle.arcs.push_back({netPayload(
                              input.nodes[arc.source.value()],
                              input.nodes[arc.sink.value()],
                              (*arc.output_load)[0][0],
                              arc.key.parallel_ordinal),
                          true});
  Generated result{std::move(*attempt.state), std::move(oracle),
                   std::move(groups)};
  requirePropertyMatches(result.state, result.oracle, "random baseline");
  return result;
}

std::size_t baseOutSlot(const DualCsrState& state, csr::NodeId source,
                        csr::ArcId arc)
{
  for (std::uint64_t slot = state.base_out.row_offsets.at(source.value());
       slot < state.base_out.row_offsets.at(source.value() + 1); ++slot)
    if (state.base_out.physical_slots.at(slot)
        == std::optional<csr::ArcId>{arc})
      return slot;
  throw std::runtime_error("BaseOut ArcId slot was not found");
}

std::size_t baseInSlot(const DualCsrState& state, csr::NodeId sink,
                       csr::ArcId arc)
{
  for (std::uint64_t slot = state.base_in.row_offsets.at(sink.value());
       slot < state.base_in.row_offsets.at(sink.value() + 1); ++slot)
    if (state.base_in.physical_slots.at(slot)
        == std::optional<csr::ArcId>{arc})
      return slot;
  throw std::runtime_error("BaseIn ArcId slot was not found");
}

csr::ArcId activeArc(const DualCsrState& state, csr::NodeId source,
                     csr::NodeId sink)
{
  for (std::size_t id = 0; id < state.arc_slots.size(); ++id)
    if (state.arc_slots[id].active
        && state.arc_slots[id].payload.source == source
        && state.arc_slots[id].payload.sink == sink)
      return csr::ArcId{id};
  throw std::runtime_error("active endpoint pair was not found");
}

std::vector<csr::ArcId> auxRow(const DualCsrState& state,
                               csr::NodeId source)
{
  const auto mapped = state.aux_out.source_to_aux_row.at(source.value());
  if (mapped == -1)
    return {};
  const auto row = static_cast<std::size_t>(mapped);
  return state.aux_out.row_arcs.at(row);
}


ArcAppendRecord appendNetArcWithKeys(
    csr::ArcId id, csr::NodeId source, csr::NodeId sink,
    const csr::NodeKey& source_key, const csr::NodeKey& sink_key,
    const std::string& net, double load, std::uint64_t ordinal = 0)
{
  auto result = appendArc(id.value(), source, sink, source_key, sink_key,
                          ordinal);
  result.payload.key.base.owner_name = net;
  result.payload.output_load = csr::test::outputLoad(load);
  result.key = result.payload.key;
  return result;
}


ArcAppendRecord appendNetArc(const DualCsrState& state, csr::ArcId id,
                             csr::NodeId source, csr::NodeId sink,
                             const std::string& net, double load,
                             std::uint64_t ordinal = 0)
{
  auto result = appendArc(id.value(), source, sink,
                          state.nodes.at(source.value()).key,
                          state.nodes.at(sink.value()).key, ordinal);
  result.payload.key.base.owner_name = net;
  result.payload.output_load = csr::test::outputLoad(load);
  result.key = result.payload.key;
  return result;
}

std::string descriptorSummary(const DualCsrBatchDescriptor& descriptor)
{
  return "nodes+=" + std::to_string(descriptor.node_appends.size())
         + ",arcs+=" + std::to_string(descriptor.arc_appends.size())
         + ",remove=" + std::to_string(descriptor.removes.size())
         + ",change=" + std::to_string(descriptor.changes.size())
         + ",reverse=" + std::to_string(descriptor.reverse_changes.size())
         + ",aux=" + std::to_string(descriptor.aux_rows.size())
         + ",deactivate="
         + std::to_string(descriptor.logical_deactivations.size())
         + ",payload=" + std::to_string(descriptor.payload_changes.size());
}

void requireAtomicReject(const DualCsrState& state,
                         const DualCsrBatchDescriptor& malformed,
                         const std::string& context)
{
  const DualCsrState before = state;
  const auto attempt = [&]() {
    try {
      return validateDualCsrDescriptor(state, malformed);
    } catch (const std::exception& error) {
      throw std::runtime_error(context + " updater threw: " + error.what());
    }
  }();
  require(!attempt.empty(),
          context + " malformed descriptor was accepted: "
              + descriptorSummary(malformed));
  require(state == before,
          context + " malformed descriptor mutated committed state");
}

void applyChecked(DualCsrState& state, PropertyOracle& oracle,
                  const DualCsrBatchDescriptor& descriptor,
                  const std::string& context)
{
  const auto violations = validateDualCsrDescriptor(state, descriptor);
  require(violations.empty(),
          context + " rejected " + descriptorSummary(descriptor)
              + (violations.empty() ? std::string{} : " at " + violations.front().field + ": " + violations.front().message));
  detail::PreparedDualCsrGraphPatch patch(state, descriptor);
  patch.apply();
  oracle.apply(descriptor);
  patch.commit();
  requirePropertyMatches(
      state, oracle,
      context + " [" + descriptorSummary(descriptor) + "]");
}

std::string uniqueName(std::uint64_t seed, std::size_t sample,
                       std::string_view role)
{
  return "z_" + std::to_string(seed) + "_" + std::to_string(sample)
         + "_" + std::string(role);
}

void runSample(std::uint64_t seed, std::size_t sample, Random& random)
{
  auto generated = generateBaseline(random);
  auto& state = generated.state;
  auto& oracle = generated.oracle;
  auto& primary = generated.groups.front();
  const auto primary_source = primary.source;
  const auto primary_load = primary.loads.front();
  const auto reconnect_sink
      = primary.loads[1 + random.pick(primary.loads.size() - 1)];
  double current_load = primary.output_load;
  const std::string prefix = "seed=" + std::to_string(seed)
                             + ",sample=" + std::to_string(sample) + ",";

  const csr::NodeId aux_sink{state.nodes.size()};
  const csr::ArcId aux_arc{state.arc_slots.size()};
  auto aux_descriptor = descriptorFor(state, 1, 1);
  auto aux_node = appendNode(aux_sink.value(),
                             uniqueName(seed, sample, "aux_sink"),
                             AppendedPortRole::kSink);
  aux_node.payload.net_name = primary.net;
  aux_descriptor.node_appends = {aux_node};
  aux_descriptor.arc_appends = {appendNetArcWithKeys(
      aux_arc, primary_source, aux_sink,
      state.nodes[primary_source.value()].key, aux_node.key, primary.net,
      current_load)};
  aux_descriptor.aux_rows = {{primary_source, auxRow(state, primary_source),
                              {aux_arc}}};
  completeReferencePayload(aux_descriptor, state);
  auto bad_aux = aux_descriptor;
  bad_aux.node_appends[0].id = csr::NodeId{aux_sink.value() + 1};
  requireAtomicReject(state, bad_aux, prefix + "malformed_aux_append");
  applyChecked(state, oracle, aux_descriptor, prefix + "aux_append");
  require(auxRow(state, primary_source)
              == std::vector<csr::ArcId>{aux_arc},
          prefix + "Aux append did not materialize the expected row");

  const auto parallel_old = activeArc(state, primary_source, primary_load);
  const csr::ArcId parallel_new{state.arc_slots.size()};
  auto parallel_descriptor = descriptorFor(state, 0, 1);
  const auto parallel_base
      = state.arc_slots[parallel_old.value()].payload.key.base;
  auto parallel_append = appendNetArc(
      state, parallel_new, primary_source, primary_load, primary.net,
      current_load, 0);
  std::uint64_t ordinal = 0;
  for (std::size_t id = 0; id < state.arc_slots.size(); ++id) {
    const auto& slot = state.arc_slots[id];
    if (!slot.active || id == parallel_old.value()
        || slot.payload.key.base != parallel_base)
      continue;
    if (slot.payload.key.parallel_ordinal != ordinal) {
      auto replacement = slot.payload;
      replacement.key.parallel_ordinal = ordinal;
      parallel_descriptor.payload_changes.push_back(
          {csr::ArcId{id}, slot.payload, std::move(replacement)});
    }
    ++ordinal;
  }
  parallel_append.key.parallel_ordinal = ordinal;
  parallel_append.payload.key.parallel_ordinal = ordinal;
  parallel_descriptor.arc_appends = {parallel_append};
  parallel_descriptor.changes = {{
      primary_source, baseOutSlot(state, primary_source, parallel_old),
      parallel_old, parallel_new}};
  parallel_descriptor.reverse_changes = {{
      primary_load, baseInSlot(state, primary_load, parallel_old),
      parallel_old, parallel_new}};
  parallel_descriptor.logical_deactivations = {deactivate(
      state, parallel_old, LogicalArcDeactivateReason::kReplaced)};
  completeReferencePayload(parallel_descriptor, state);
  auto bad_parallel = parallel_descriptor;
  bad_parallel.logical_deactivations.clear();
  requireAtomicReject(state, bad_parallel,
                      prefix + "malformed_parallel_replace");
  applyChecked(state, oracle, parallel_descriptor,
               prefix + "parallel_replace");

  double replacement_load
      = 0.25 * static_cast<double>(1 + random.pick(4));
  if (replacement_load == current_load)
    replacement_load = current_load == 1.0 ? 0.25 : current_load + 0.25;
  auto payload_descriptor = descriptorFor(state, 0, 0);
  for (std::size_t id = 0; id < state.arc_slots.size(); ++id) {
    const auto& slot = state.arc_slots[id];
    if (!slot.active || slot.payload.key.base.owner_name != primary.net)
      continue;
    auto replacement = slot.payload;
    replacement.output_load = csr::test::outputLoad(replacement_load);
    payload_descriptor.payload_changes.push_back(
        {csr::ArcId{id}, slot.payload, std::move(replacement)});
  }
  completeReferencePayload(payload_descriptor, state);
  auto constraints = state.reference_payload.port_constraints;
  const auto load_constraint = std::ranges::find_if(
      constraints, [&](const csr::PortConstraintModel& constraint) {
        return constraint.resolved_node
               == std::optional<csr::NodeId>{primary_load};
      });
  require(load_constraint != constraints.end(),
          prefix + "primary load constraint disappeared");
  load_constraint->output_load = replacement_load;
  replaceReference(payload_descriptor.reference_payload.port_constraints,
                   state.reference_payload.port_constraints,
                   std::move(constraints));
  auto bad_payload = payload_descriptor;
  ++bad_payload.payload_changes.front().replacement.key.parallel_ordinal;
  requireAtomicReject(state, bad_payload,
                      prefix + "malformed_payload_change");
  applyChecked(state, oracle, payload_descriptor,
               prefix + "payload_change");
  current_load = replacement_load;

  const auto reconnect_old
      = activeArc(state, primary_source, reconnect_sink);
  const auto tombstone_slot
      = baseOutSlot(state, primary_source, reconnect_old);
  const csr::NodeId reconnect_source{state.nodes.size()};
  const csr::ArcId reconnect_arc{state.arc_slots.size()};
  const std::string reconnect_net
      = uniqueName(seed, sample, "reconnect_net");
  auto reconnect_descriptor = descriptorFor(state, 1, 1);
  auto reconnect_node = appendNode(
      reconnect_source.value(),
      uniqueName(seed, sample, "reconnect_source"),
      AppendedPortRole::kSource);
  reconnect_node.payload.net_name = reconnect_net;
  reconnect_descriptor.node_appends = {reconnect_node};
  reconnect_descriptor.arc_appends = {appendNetArcWithKeys(
      reconnect_arc, reconnect_source, reconnect_sink, reconnect_node.key,
      state.nodes[reconnect_sink.value()].key, reconnect_net, 0.0)};
  reconnect_descriptor.removes = {{primary_source, tombstone_slot,
                                    reconnect_old}};
  reconnect_descriptor.reverse_changes = {{
      reconnect_sink, baseInSlot(state, reconnect_sink, reconnect_old),
      reconnect_old, reconnect_arc}};
  reconnect_descriptor.logical_deactivations = {deactivate(
      state, reconnect_old, LogicalArcDeactivateReason::kRemoved)};
  reconnect_descriptor.node_payload_changes = {{
      reconnect_sink, state.nodes[reconnect_sink.value()].key,
      state.nodes[reconnect_sink.value()].net_name, reconnect_net}};
  completeReferencePayload(reconnect_descriptor, state);
  auto bad_reconnect = reconnect_descriptor;
  bad_reconnect.reverse_changes[0].expected_arc
      = csr::ArcId{state.arc_slots.size() + 99};
  requireAtomicReject(state, bad_reconnect,
                      prefix + "malformed_reconnect");
  applyChecked(state, oracle, reconnect_descriptor,
               prefix + "reconnect_tombstone");
  require(!state.base_out.physical_slots[tombstone_slot],
          prefix + "Base tombstone was not created");

  const csr::NodeId reused_sink{state.nodes.size()};
  const csr::ArcId reused_arc{state.arc_slots.size()};
  auto reuse_descriptor = descriptorFor(state, 1, 1);
  auto reuse_node = appendNode(reused_sink.value(),
                               uniqueName(seed, sample, "reuse_sink"),
                               AppendedPortRole::kSink);
  reuse_node.payload.net_name = primary.net;
  reuse_descriptor.node_appends = {reuse_node};
  reuse_descriptor.arc_appends = {appendNetArcWithKeys(
      reused_arc, primary_source, reused_sink,
      state.nodes[primary_source.value()].key, reuse_node.key, primary.net,
      current_load)};
  reuse_descriptor.changes = {{primary_source, tombstone_slot,
                               std::nullopt, reused_arc}};
  completeReferencePayload(reuse_descriptor, state);
  auto bad_reuse = reuse_descriptor;
  bad_reuse.changes[0].expected_arc = reconnect_old;
  requireAtomicReject(state, bad_reuse,
                      prefix + "malformed_tombstone_reuse");
  applyChecked(state, oracle, reuse_descriptor,
               prefix + "tombstone_reuse");
  require(state.base_out.physical_slots[tombstone_slot]
              == std::optional<csr::ArcId>{reused_arc},
          prefix + "Base tombstone was not reused");

  const csr::NodeId aux_replacement_source{state.nodes.size()};
  const csr::ArcId aux_replacement_arc{state.arc_slots.size()};
  const std::string aux_net
      = uniqueName(seed, sample, "aux_reconnect_net");
  auto clear_descriptor = descriptorFor(state, 1, 1);
  auto clear_node = appendNode(
      aux_replacement_source.value(),
      uniqueName(seed, sample, "aux_reconnect_source"),
      AppendedPortRole::kSource);
  clear_node.payload.net_name = aux_net;
  clear_descriptor.node_appends = {clear_node};
  clear_descriptor.arc_appends = {appendNetArcWithKeys(
      aux_replacement_arc, aux_replacement_source, aux_sink, clear_node.key,
      state.nodes[aux_sink.value()].key, aux_net, 0.0)};
  clear_descriptor.reverse_changes = {{
      aux_sink, baseInSlot(state, aux_sink, aux_arc), aux_arc,
      aux_replacement_arc}};
  clear_descriptor.logical_deactivations = {deactivate(
      state, aux_arc, LogicalArcDeactivateReason::kAuxCleared)};
  clear_descriptor.aux_rows = {{primary_source,
                                auxRow(state, primary_source), {}}};
  clear_descriptor.node_payload_changes = {{
      aux_sink, state.nodes[aux_sink.value()].key,
      state.nodes[aux_sink.value()].net_name, aux_net}};
  completeReferencePayload(clear_descriptor, state);
  auto bad_clear = clear_descriptor;
  bad_clear.logical_deactivations.clear();
  requireAtomicReject(state, bad_clear, prefix + "malformed_aux_clear");
  applyChecked(state, oracle, clear_descriptor, prefix + "aux_clear");
  require(auxRow(state, primary_source).empty()
              && !state.arc_slots[aux_arc.value()].active,
          prefix + "Aux clear did not remove/deactivate the old arc");
}

void randomizedDagDescriptorProperty()
{
  for (const std::uint64_t seed : kXorShiftSeeds) {
    Random random{seed};
    for (std::size_t sample = 0; sample < kSampleCount; ++sample) {
      try {
        runSample(seed, sample, random);
      } catch (const std::exception& error) {
        throw std::runtime_error(
            "seed=" + std::to_string(seed)
            + ",sample=" + std::to_string(sample) + ": "
            + error.what());
      }
    }
  }
}

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main()
{
  try {
    ista::timing_dual_csr::test::randomizedDagDescriptorProperty();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
