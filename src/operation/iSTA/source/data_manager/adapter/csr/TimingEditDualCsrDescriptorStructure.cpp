// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "TimingEditDualCsrDescriptorStructureInternal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "NarrowTimingCaptureInternal.hpp"
#include "TimingEditDualCsrDescriptorBuilder.hpp"

namespace ista::timing_dual_csr::detail {
namespace {

[[noreturn]] void invalid(std::string message)
{
  throw std::invalid_argument("dual csr descriptor structure: "
                              + std::move(message));
}

const timing_edit::NativeSlotBinding& stagedNodeBinding(
    const timing_edit::TimingEditResolverState& resolver,
    const timing_edit::StableNodeRef& ref)
{
  const auto found = resolver.nodeBindings().find(ref);
  if (found == resolver.nodeBindings().end() || found->second.tombstone)
    invalid("reconnect sink has no live staged resolver binding");
  return found->second;
}

const timing_edit::NativeSlotBinding& stagedArcBinding(
    const timing_edit::TimingEditResolverState& resolver,
    const timing_edit::StableArcRef& ref, bool expected_tombstone)
{
  const auto found = resolver.arcBindings().find(ref);
  if (found == resolver.arcBindings().end()
      || found->second.tombstone != expected_tombstone)
    invalid("reconnect arc has the wrong staged resolver lifecycle");
  return found->second;
}

bool matchesReconnect(const timing_edit::StableArcRef& arc,
                      const timing_edit::StableNodeRef& sink,
                      const timing_edit::StableNetRef& net)
{
  return std::visit(
      [&](const auto& ref) {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref,
                                     timing_edit::ExistingArcRef>) {
          if (ref.baseline_key.base.kind != csr::ArcKind::kNet)
            return false;
          return timing_edit::StableNodeRef{timing_edit::ExistingNodeRef{
                     ref.baseline_key.base.sink}}
                     == sink
                 && timing_edit::StableNetRef{timing_edit::ExistingNetRef{
                        ref.baseline_key.base.owner_name}}
                        == net;
        } else if constexpr (std::is_same_v<
                                 Ref,
                                 timing_edit::CreatedOldNetArcRef>) {
          return ref.sink == sink && ref.net == net;
        } else if constexpr (std::is_same_v<
                                 Ref,
                                 timing_edit::CreatedNewNetArcRef>) {
          return ref.sink == sink
                 && timing_edit::StableNetRef{timing_edit::CreatedNetRef{
                        ref.creation_token,
                        timing_edit::CreatedNetRole::kOutputNet}}
                        == net;
        } else {
          return false;
        }
      },
      arc);
}

template <typename Operation>
std::vector<timing_edit::StableArcRef> matchingArcs(
    const timing_edit::TimingGraphDelta& delta,
    const timing_edit::StableNodeRef& sink,
    const timing_edit::StableNetRef& net)
{
  std::vector<timing_edit::StableArcRef> result;
  for (const auto& operation : delta.operations)
    if (const auto* item = std::get_if<Operation>(&operation);
        item && matchesReconnect(item->arc, sink, net))
      result.push_back(item->arc);
  return result;
}

const CapturedArc& capturedArc(
    const NarrowTimingRecords& records,
    const timing_edit::StableArcRef& ref)
{
  const CapturedArc* match = nullptr;
  for (const auto& captured : records.arcs) {
    if (captured.ref != ref)
      continue;
    if (match != nullptr)
      invalid("capture repeats a reconnect StableArcRef");
    match = &captured;
  }
  if (match == nullptr)
    invalid("capture omits a reconnect StableArcRef");
  return *match;
}

std::vector<csr::ArcId> baseOutRow(const DualCsrState& state,
                                   csr::NodeId source)
{
  if (source.value() + 1 >= state.base_out.row_offsets.size())
    invalid("BaseOut source is outside committed rows");
  const auto begin = state.base_out.row_offsets[source.value()];
  const auto end = state.base_out.row_offsets[source.value() + 1];
  if (begin > end || end > state.base_out.physical_slots.size())
    invalid("committed BaseOut row has invalid bounds");
  std::vector<csr::ArcId> result;
  for (auto slot = begin; slot < end; ++slot)
    if (state.base_out.physical_slots[slot])
      result.push_back(*state.base_out.physical_slots[slot]);
  return result;
}

std::vector<csr::ArcId> baseInRow(const DualCsrState& state,
                                  csr::NodeId sink)
{
  if (sink.value() + 1 >= state.base_in.row_offsets.size())
    invalid("BaseIn sink is outside committed rows");
  const auto begin = state.base_in.row_offsets[sink.value()];
  const auto end = state.base_in.row_offsets[sink.value() + 1];
  if (begin > end || end > state.base_in.physical_slots.size())
    invalid("committed BaseIn row has invalid bounds");
  std::vector<csr::ArcId> result;
  result.reserve(static_cast<std::size_t>(end - begin));
  for (auto slot = begin; slot < end; ++slot) {
    if (!state.base_in.physical_slots[slot])
      invalid("committed BaseIn row contains a tombstone");
    result.push_back(*state.base_in.physical_slots[slot]);
  }
  return result;
}

std::vector<csr::ArcId> auxRow(const DualCsrState& state,
                               csr::NodeId source)
{
  if (source.value() >= state.aux_out.source_to_aux_row.size())
    invalid("Aux source is outside committed mapping");
  const auto mapped = state.aux_out.source_to_aux_row[source.value()];
  if (mapped < 0)
    return {};
  const auto row = static_cast<std::size_t>(mapped);
  const auto begin = state.aux_out.row_offsets.at(row);
  const auto end = state.aux_out.row_offsets.at(row + 1);
  return {state.aux_out.arc_ids.begin()
              + static_cast<std::ptrdiff_t>(begin),
          state.aux_out.arc_ids.begin()
              + static_cast<std::ptrdiff_t>(end)};
}

std::vector<csr::ArcId> fullOutgoingRow(const DualCsrState& state,
                                        csr::NodeId source)
{
  auto result = baseOutRow(state, source);
  auto aux = auxRow(state, source);
  result.insert(result.end(), aux.begin(), aux.end());
  return result;
}

bool deactivated(const DualCsrBatchDescriptor& descriptor, csr::ArcId id)
{
  return std::ranges::any_of(
      descriptor.logical_deactivations,
      [&](const auto& item) { return item.id == id; });
}

void requireExactIds(const std::vector<csr::ArcId>& expected,
                     const std::vector<csr::ArcId>& actual,
                     const char* field)
{
  std::set<std::uint64_t> expected_ids;
  std::set<std::uint64_t> actual_ids;
  for (const auto id : expected)
    if (!expected_ids.insert(id.value()).second)
      invalid(std::string{field} + " expected row repeats ArcId");
  for (const auto id : actual)
    if (!actual_ids.insert(id.value()).second)
      invalid(std::string{field} + " candidate row repeats ArcId");
  if (expected_ids != actual_ids)
    invalid(std::string{field} + " candidate row has wrong ArcId set");
}

void buildOutgoingAndAux(const DualCsrState& state,
                         const NarrowTimingRecords& records,
                         DualCsrBatchDescriptor& descriptor)
{
  std::set<std::uint64_t> expected_sources;
  for (const auto& item : descriptor.node_appends)
    expected_sources.insert(item.id.value());
  for (const auto& item : descriptor.logical_deactivations)
    expected_sources.insert(item.source.value());
  for (const auto& item : descriptor.arc_appends)
    expected_sources.insert(item.source.value());

  std::set<std::uint64_t> captured_sources;
  for (const auto& captured : records.outgoing_rows) {
    const auto source = captured.source;
    if (!expected_sources.contains(source.value())
        || !captured_sources.insert(source.value()).second)
      invalid("candidate outgoing rows do not exactly cover affected sources");
    std::vector<csr::ArcId> expected_post;
    if (source.value() < descriptor.before.node_slots) {
      expected_post = fullOutgoingRow(state, source);
      expected_post.erase(
          std::remove_if(expected_post.begin(), expected_post.end(),
                         [&](csr::ArcId id) {
                           return deactivated(descriptor, id);
                         }),
          expected_post.end());
    }
    for (const auto& append : descriptor.arc_appends)
      if (append.source == source)
        expected_post.push_back(append.id);
    requireExactIds(expected_post, captured.active_arc_ids,
                    "outgoing row");

    if (source.value() >= descriptor.before.node_slots) {
      descriptor.appended_base_out_rows.push_back(
          {source, captured.active_arc_ids});
      continue;
    }

    const auto expected = fullOutgoingRow(state, source);
    descriptor.affected_base_out_rows.push_back(
        {source, expected, captured.active_arc_ids});
    const auto begin = state.base_out.row_offsets.at(source.value());
    const auto end = state.base_out.row_offsets.at(source.value() + 1);
    const auto capacity = static_cast<std::size_t>(end - begin);
    for (std::size_t offset = 0; offset < capacity; ++offset) {
      const auto slot = begin + offset;
      const auto before = state.base_out.physical_slots.at(slot);
      const std::optional<csr::ArcId> after
          = offset < captured.active_arc_ids.size()
                ? std::optional<csr::ArcId>{captured.active_arc_ids[offset]}
                : std::nullopt;
      if (before == after)
        continue;
      if (after)
        descriptor.changes.push_back({source, slot, before, *after});
      else if (before)
        descriptor.removes.push_back({source, slot, *before});
    }
    const auto split = std::min(capacity, captured.active_arc_ids.size());
    std::vector<csr::ArcId> replacement(
        captured.active_arc_ids.begin()
            + static_cast<std::ptrdiff_t>(split),
        captured.active_arc_ids.end());
    const auto expected_aux = auxRow(state, source);
    if (replacement != expected_aux)
      descriptor.aux_rows.push_back(
          {source, expected_aux, std::move(replacement)});
  }
  if (captured_sources != expected_sources)
    invalid("candidate outgoing rows omit an affected source");
  std::ranges::sort(descriptor.appended_base_out_rows,
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.source.value() < rhs.source.value();
                    });
  std::ranges::sort(descriptor.affected_base_out_rows,
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.source.value() < rhs.source.value();
                    });
}

void buildIncoming(const PendingStructuredBatch& pending,
                   const NarrowTimingRecords& records,
                   DualCsrBatchDescriptor& descriptor)
{
  const auto& staged = pending.stagedResolver();
  std::set<std::uint64_t> used_old;
  std::set<std::uint64_t> used_new;
  for (const auto& operation : pending.preparedBatch()
                                   .forwardDelta().operations) {
    const auto* reconnect = std::get_if<timing_edit::ReconnectSink>(
        &operation);
    if (reconnect == nullptr)
      continue;
    const auto removed = matchingArcs<timing_edit::RemoveNetArc>(
        pending.preparedBatch().forwardDelta(), reconnect->sink,
        reconnect->old_net);
    const auto added = matchingArcs<timing_edit::AddNetArc>(
        pending.preparedBatch().forwardDelta(), reconnect->sink,
        reconnect->new_net);
    if (removed.size() != 1 || added.size() != 1)
      invalid("reconnect lacks one exact remove-old/add-new ref pair");

    const auto& old_capture = capturedArc(records, removed.front());
    const auto& new_capture = capturedArc(records, added.front());
    const auto old_id
        = stagedArcBinding(staged, removed.front(), true).slot.value();
    const auto new_id
        = stagedArcBinding(staged, added.front(), false).slot.value();
    const auto sink_id
        = stagedNodeBinding(staged, reconnect->sink).slot.value();
    if (old_capture.active || !new_capture.active
        || old_capture.id.value() != old_id
        || new_capture.id.value() != new_id
        || old_capture.payload.sink.value() != sink_id
        || new_capture.payload.sink.value() != sink_id)
      invalid("captured reconnect payload does not match its stable ref pair");

    auto deactivation = std::ranges::find(
        descriptor.logical_deactivations, csr::ArcId{old_id},
        &LogicalArcDeactivate::id);
    const auto append = std::ranges::find(
        descriptor.arc_appends, csr::ArcId{new_id},
        &ArcAppendRecord::id);
    if (deactivation == descriptor.logical_deactivations.end()
        || append == descriptor.arc_appends.end()
        || deactivation->sink.value() != sink_id
        || append->sink.value() != sink_id
        || !used_old.insert(old_id).second
        || !used_new.insert(new_id).second)
      invalid("descriptor reconnect is not a one-to-one stable-ref pair");

    deactivation->reason = LogicalArcDeactivateReason::kReplaced;
  }

  if (used_old.size() != descriptor.logical_deactivations.size())
    invalid("logical deactivation has no semantic reconnect identity");
  for (const auto& append : descriptor.arc_appends)
    if (append.sink.value() < descriptor.before.node_slots
        && !used_new.contains(append.id.value()))
      invalid("append to an old sink has no semantic reconnect identity");

  const auto& state = pending.expectedBefore().graph();
  std::set<std::uint64_t> expected_sinks;
  for (const auto& item : descriptor.node_appends)
    expected_sinks.insert(item.id.value());
  for (const auto& item : descriptor.logical_deactivations)
    expected_sinks.insert(item.sink.value());
  for (const auto& item : descriptor.arc_appends)
    expected_sinks.insert(item.sink.value());
  std::set<std::uint64_t> captured_sinks;
  for (const auto& captured : records.incoming_rows) {
    const auto sink = captured.sink;
    if (!expected_sinks.contains(sink.value())
        || !captured_sinks.insert(sink.value()).second)
      invalid("candidate incoming rows do not exactly cover affected sinks");
    std::vector<csr::ArcId> expected;
    if (sink.value() < descriptor.before.node_slots)
      expected = baseInRow(state, sink);
    auto expected_post = expected;
    expected_post.erase(
        std::remove_if(expected_post.begin(), expected_post.end(),
                       [&](csr::ArcId id) {
                         return deactivated(descriptor, id);
                       }),
        expected_post.end());
    for (const auto& append : descriptor.arc_appends)
      if (append.sink == sink)
        expected_post.push_back(append.id);
    requireExactIds(expected_post, captured.active_arc_ids,
                    "incoming row");
    descriptor.affected_base_in_rows.push_back(
        {sink, expected, captured.active_arc_ids});
    if (sink.value() >= descriptor.before.node_slots)
      continue;
    const auto begin = state.base_in.row_offsets.at(sink.value());
    const auto end = state.base_in.row_offsets.at(sink.value() + 1);
    if (captured.active_arc_ids.size()
        != static_cast<std::size_t>(end - begin))
      invalid("existing BaseIn row cannot change physical capacity");
    for (std::size_t offset = 0;
         offset < captured.active_arc_ids.size(); ++offset) {
      if (expected[offset] == captured.active_arc_ids[offset])
        continue;
      descriptor.reverse_changes.push_back(
          {sink, begin + offset, expected[offset],
           captured.active_arc_ids[offset]});
    }
  }
  if (captured_sinks != expected_sinks)
    invalid("candidate incoming rows omit an affected sink");
  std::ranges::sort(descriptor.affected_base_in_rows,
                    [](const auto& lhs, const auto& rhs) {
                      return lhs.sink.value() < rhs.sink.value();
                    });
  std::ranges::sort(
      descriptor.reverse_changes,
      [](const auto& lhs, const auto& rhs) {
        if (lhs.sink != rhs.sink)
          return lhs.sink.value() < rhs.sink.value();
        return lhs.physical_slot < rhs.physical_slot;
      });
}

}  // namespace

void buildStructuredPhysicalDelta(
    const PendingStructuredBatch& pending,
    const NarrowTimingRecords& records,
    DualCsrBatchDescriptor& descriptor)
{
  buildOutgoingAndAux(pending.expectedBefore().graph(), records,
                      descriptor);
  buildIncoming(pending, records, descriptor);
}

}  // namespace ista::timing_dual_csr::detail
