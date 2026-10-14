// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CsrValidatorTestFixture.hpp"
#include "DualCsrBaselineFactory.hpp"
#include "DualCsrFunctionalProjection.hpp"
#include "DualCsrDescriptorValidation.hpp"
#include "DualCsrUpdaterInternal.hpp"

namespace ista::timing_dual_csr::test {

inline constexpr std::uint64_t kSessionValue = 47;

inline void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

inline csr::NodeKey nodeKey(std::string_view name)
{
  csr::NodeKey key;
  key.kind = csr::NodeKind::kPort;
  key.full_name = std::string{name};
  return key;
}

inline csr::NodeRecord node(std::uint64_t id, std::string_view name)
{
  csr::NodeRecord result;
  result.id = csr::NodeId{id};
  result.key = nodeKey(name);
  result.direction = csr::PinDirection::kInput;
  result.net_name = "n1";
  result.is_port = true;
  return result;
}

inline csr::ArcSemanticPayload arcPayload(
    const csr::NodeKey& source_key, const csr::NodeKey& sink_key,
    csr::NodeId source, csr::NodeId sink, std::uint64_t ordinal)
{
  csr::ArcSemanticPayload result;
  result.key.base.source = source_key;
  result.key.base.sink = sink_key;
  result.key.base.kind = csr::ArcKind::kNet;
  result.key.base.owner_name = "n1";
  result.key.parallel_ordinal = ordinal;
  result.source = source;
  result.sink = sink;
  result.kind = csr::ArcKind::kNet;
  result.eligibility = {true, true, true, false};
  result.transfer.delay = 0.0;
  result.transfer.delay_min = 0.0;
  result.transfer.delay_max = 0.0;
  return result;
}

inline csr::ArcRecord arcRecord(std::uint64_t id,
                                const csr::NodeRecord& source,
                                const csr::NodeRecord& sink,
                                std::uint64_t ordinal)
{
  const auto payload = arcPayload(source.key, sink.key, source.id, sink.id,
                                  ordinal);
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
  result.native_cell_arc_ordinal = payload.native_cell_arc_ordinal;
  return result;
}

inline csr::CsrReferenceInput baselineInput()
{
  csr::CsrReferenceInput input = csr::test::referenceInput();
  csr::test::refreshIdentity(input, kSessionValue);
  return input;
}

inline DualCsrState baselineState()
{
  const auto input = baselineInput();
  auto attempt = DualCsrBaselineFactory::create(
      input, timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{input.nodes.size(), 0,
                                       input.arcs.size()});
  require(attempt.success(), "Dual baseline factory rejected test fixture");
  return std::move(*attempt.state);
}

inline DualCsrBatchDescriptor descriptorFor(
    const DualCsrState& state, std::size_t appended_nodes,
    std::size_t appended_arcs)
{
  DualCsrBatchDescriptor result;
  result.session_token = state.session_token;
  result.expected_generation = state.generation;
  result.target_generation
      = timing_edit::TransactionGeneration{state.generation.value() + 1};
  result.before = state.high_water;
  result.after = state.high_water;
  result.after.node_slots += appended_nodes;
  result.after.arc_slots += appended_arcs;
  auto replacement = state.reference_payload.source_identity;
  replacement.session_token = state.session_token.value();
  replacement.graph_generation = result.target_generation.value();
  result.reference_payload.source_identity
      = ValueReplacement<csr::CsrSnapshotIdentity>{
          state.reference_payload.source_identity,
          std::move(replacement)};
  return result;
}

enum class AppendedPortRole : std::uint8_t
{
  kSource,
  kSink,
  kIsolated
};

inline NodeAppendRecord appendNode(std::uint64_t id,
                                   std::string_view name,
                                   AppendedPortRole role)
{
  NodeAppendRecord result;
  result.id = csr::NodeId{id};
  result.key = nodeKey(name);
  result.payload = node(id, name);
  if (role == AppendedPortRole::kSource) {
    result.payload.is_startpoint = true;
  } else if (role == AppendedPortRole::kSink) {
    result.payload.direction = csr::PinDirection::kOutput;
    result.payload.is_endpoint = true;
  } else {
    result.payload.direction = csr::PinDirection::kInout;
    result.payload.is_startpoint = true;
    result.payload.is_endpoint = true;
  }
  return result;
}

template <typename T>
inline void replaceReference(
    std::optional<ValueReplacement<T>>& target, const T& current,
    T replacement)
{
  if (current == replacement) {
    target.reset();
  } else {
    target = ValueReplacement<T>{current, std::move(replacement)};
  }
}

inline std::vector<csr::ArcId> evidenceBaseRow(
    const BaseCsrState& base, csr::NodeId node)
{
  std::vector<csr::ArcId> result;
  const auto begin = base.row_offsets[node.value()];
  const auto end = base.row_offsets[node.value() + 1];
  for (auto slot = begin; slot < end; ++slot)
    if (base.physical_slots[slot])
      result.push_back(*base.physical_slots[slot]);
  return result;
}

// Legacy DB-free unit fixtures declare their intended physical result here.
// Production Stage 5 never calls this: its row order comes only from the
// reader-minted native candidate evidence.
inline void completePhysicalRowEvidence(
    DualCsrBatchDescriptor& descriptor, const DualCsrState& state)
{
  descriptor.appended_base_out_rows.clear();
  for (const auto& node : descriptor.node_appends) {
    std::vector<csr::ArcId> row;
    for (const auto& arc : descriptor.arc_appends)
      if (arc.source == node.id)
        row.push_back(arc.id);
    descriptor.appended_base_out_rows.push_back(
        {node.id, std::move(row)});
  }

  auto in_slots = state.base_in.physical_slots;
  for (const auto& change : descriptor.reverse_changes)
    in_slots[change.physical_slot] = change.replacement_arc;
  std::set<std::uint64_t> affected_sinks;
  for (const auto& node : descriptor.node_appends)
    affected_sinks.insert(node.id.value());
  for (const auto& change : descriptor.reverse_changes)
    affected_sinks.insert(change.sink.value());
  descriptor.affected_base_in_rows.clear();
  for (const auto sink_value : affected_sinks) {
    const csr::NodeId sink{sink_value};
    std::vector<csr::ArcId> expected;
    std::vector<csr::ArcId> replacement;
    if (sink_value < descriptor.before.node_slots) {
      expected = evidenceBaseRow(state.base_in, sink);
      const auto begin = state.base_in.row_offsets[sink_value];
      const auto end = state.base_in.row_offsets[sink_value + 1];
      for (auto slot = begin; slot < end; ++slot)
        if (in_slots[slot])
          replacement.push_back(*in_slots[slot]);
    } else {
      for (const auto& arc : descriptor.arc_appends)
        if (arc.sink == sink)
          replacement.push_back(arc.id);
    }
    descriptor.affected_base_in_rows.push_back(
        {sink, std::move(expected), std::move(replacement)});
  }
}

inline void completeReferencePayload(
    DualCsrBatchDescriptor& descriptor, const DualCsrState& state)
{
  completePhysicalRowEvidence(descriptor, state);
  std::vector<csr::NodeRecord> nodes = state.nodes;
  for (const auto& append : descriptor.node_appends)
    nodes.push_back(append.payload);

  std::vector<csr::ArcSemanticPayload> active_arcs;
  for (std::size_t id = 0; id < state.arc_slots.size(); ++id) {
    if (!state.arc_slots[id].active)
      continue;
    const bool deactivated = std::ranges::any_of(
        descriptor.logical_deactivations,
        [id](const LogicalArcDeactivate& item) {
          return item.id.value() == id;
        });
    if (deactivated)
      continue;
    const auto changed = std::ranges::find_if(
        descriptor.payload_changes,
        [id](const ArcPayloadChangeDescriptor& item) {
          return item.id.value() == id;
        });
    active_arcs.push_back(changed == descriptor.payload_changes.end()
                              ? state.arc_slots[id].payload
                              : changed->replacement);
  }
  for (const auto& append : descriptor.arc_appends)
    active_arcs.push_back(append.payload);

  std::vector<std::uint64_t> eligible_indegree(nodes.size(), 0);
  for (const auto& arc : active_arcs)
    if (arc.eligibility.levelization
        && arc.sink.value() < eligible_indegree.size())
      ++eligible_indegree[arc.sink.value()];

  std::vector<csr::NodeId> full_name_order;
  full_name_order.reserve(nodes.size());
  for (std::size_t id = 0; id < nodes.size(); ++id)
    full_name_order.emplace_back(id);
  std::ranges::sort(full_name_order, [&](csr::NodeId lhs, csr::NodeId rhs) {
    return nodes[lhs.value()].key.full_name
           < nodes[rhs.value()].key.full_name;
  });

  std::vector<csr::NodeId> roots;
  std::vector<csr::NodeId> starts;
  std::vector<csr::NodeId> ends;
  for (const auto id : full_name_order) {
    if (eligible_indegree[id.value()] == 0)
      roots.push_back(id);
    if (nodes[id.value()].is_startpoint)
      starts.push_back(id);
    if (nodes[id.value()].is_endpoint)
      ends.push_back(id);
  }
  replaceReference(descriptor.reference_payload.ordered_root_ids,
                   state.reference_payload.ordered_root_ids,
                   std::move(roots));
  replaceReference(descriptor.reference_payload.ordered_start_ids,
                   state.reference_payload.ordered_start_ids, starts);
  replaceReference(descriptor.reference_payload.ordered_end_ids,
                   state.reference_payload.ordered_end_ids, ends);

  const auto owner_count = state.reference_payload.clock_owner_by_node.size();
  if (nodes.size() > owner_count)
    descriptor.reference_payload.clock_owner_by_node = RecordAppend<std::optional<std::string>>{
        owner_count, std::vector<std::optional<std::string>>(nodes.size() - owner_count)};

  std::vector<csr::StartSeedModel> seeds;
  for (const auto id : starts) {
    const auto found = std::ranges::find(
        state.reference_payload.start_seeds, id,
        &csr::StartSeedModel::seed_node);
    if (found != state.reference_payload.start_seeds.end()) {
      seeds.push_back(*found);
      continue;
    }
    csr::StartSeedModel seed;
    seed.seed_node = id;
    seed.path_start_node = id;
    seed.clock_tag = state.reference_payload.clock_tags.empty()
                         ? std::string{"clk"}
                         : state.reference_payload.clock_tags.front();
    for (auto& analysis : seed.slew)
      for (auto& transition : analysis)
        transition = 0.0;
    seeds.push_back(std::move(seed));
  }
  const auto& old_seeds = state.reference_payload.start_seeds;
  require(seeds.size() >= old_seeds.size(), "test descriptor cannot remove startpoints");
  StartSeedDelta seed_delta{old_seeds.size(), {}, {}};
  for (std::size_t slot = 0; slot < old_seeds.size(); ++slot)
    if (old_seeds[slot] != seeds[slot])
      seed_delta.changes.push_back({slot, old_seeds[slot], std::move(seeds[slot])});
  for (std::size_t slot = old_seeds.size(); slot < seeds.size(); ++slot)
    seed_delta.appends.push_back(std::move(seeds[slot]));
  if (!seed_delta.changes.empty() || !seed_delta.appends.empty())
    descriptor.reference_payload.start_seeds = std::move(seed_delta);

  std::vector<csr::EndpointRequiredModel> required;
  for (const auto id : ends) {
    const auto& record = nodes[id.value()];
    if (!record.is_port
        || (record.direction != csr::PinDirection::kOutput
            && record.direction != csr::PinDirection::kInout))
      continue;
    const auto found = std::ranges::find(
        state.reference_payload.endpoint_required, id,
        &csr::EndpointRequiredModel::node);
    if (found != state.reference_payload.endpoint_required.end()) {
      required.push_back(*found);
      continue;
    }
    csr::EndpointRequiredModel endpoint;
    endpoint.node = id;
    endpoint.clock_name = state.reference_payload.clock_tags.empty()
                              ? std::string{"clk"}
                              : state.reference_payload.clock_tags.front();
    required.push_back(std::move(endpoint));
  }
  replaceReference(descriptor.reference_payload.endpoint_required,
                   state.reference_payload.endpoint_required,
                   std::move(required));
}

inline ArcAppendRecord appendArc(
    std::uint64_t id, csr::NodeId source, csr::NodeId sink,
    const csr::NodeKey& source_key, const csr::NodeKey& sink_key,
    std::uint64_t ordinal)
{
  ArcAppendRecord result;
  result.id = csr::ArcId{id};
  result.source = source;
  result.sink = sink;
  result.payload = arcPayload(source_key, sink_key, source, sink,
                              ordinal);
  result.key = result.payload.key;
  return result;
}

inline LogicalArcDeactivate deactivate(
    const DualCsrState& state, csr::ArcId id,
    LogicalArcDeactivateReason reason)
{
  const auto& payload = state.arc_slots[id.value()].payload;
  return {id, payload.key, payload.source, payload.sink, reason, true};
}

inline void replaceRoots(DualCsrBatchDescriptor& descriptor,
                         const DualCsrState& state,
                         std::vector<csr::NodeId> replacement)
{
  descriptor.reference_payload.ordered_root_ids
      = ValueReplacement<std::vector<csr::NodeId>>{
          state.reference_payload.ordered_root_ids,
          std::move(replacement)};
}

inline DualCsrState apply(DualCsrState state, const DualCsrBatchDescriptor& descriptor)
{
  const auto violations = validateDualCsrDescriptor(state, descriptor);
  if (!violations.empty())
    throw std::runtime_error(violations.front().field + ": " + violations.front().message);
  detail::PreparedDualCsrGraphPatch patch(state, descriptor);
  patch.apply();
  require(validateDualCsrState(state).empty(), "local update produced an invalid graph");
  patch.commit();
  return state;
}

inline bool hasCode(const DualCsrViolations& violations,
                    DualCsrViolationCode code)
{
  for (const auto& violation : violations)
    if (violation.code == code)
      return true;
  return false;
}

inline void requireOffsetPrefix(const std::vector<std::uint64_t>& before,
                                const std::vector<std::uint64_t>& after,
                                const std::string& field)
{
  require(after.size() >= before.size(), field + " shrank");
  for (std::size_t index = 0; index < before.size(); ++index)
    require(after[index] == before[index],
            field + " changed an old-row interval");
}

}  // namespace ista::timing_dual_csr::test
