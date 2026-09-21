// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "CsrCpuReferenceBackend.hpp"
#include "DualCsrEdgeViewImpl.hpp"
#include "DualCsrTestSupport.hpp"
#include "DualCsrTimingPayloadViewImpl.hpp"
#include "DualSemanticReadView.hpp"

namespace ista::timing_dual_csr::test {
namespace {

DualCsrState twoCommittedParallelArcs()
{
  auto input = csr::test::cellInput();
  auto parallel = input.arcs.front();
  parallel.id = csr::ArcId{1};
  parallel.key.parallel_ordinal = 1;
  parallel.native_traversal_rank = 1;
  input.arcs.push_back(parallel);
  input.arc_ids_by_key.push_back({parallel.key, parallel.id});
  input.out_row_offsets = {0, 2, 2};
  input.out_arc_ids = {csr::ArcId{0}, csr::ArcId{1}};
  input.in_row_offsets = {0, 0, 2};
  input.in_arc_ids = {csr::ArcId{0}, csr::ArcId{1}};
  csr::test::refreshIdentity(input, kSessionValue);
  auto attempt = DualCsrBaselineFactory::create(
      input, timing_edit::TimingEditSessionToken{kSessionValue},
      timing_edit::AppendOnlyHighWater{
          input.nodes.size(), input.instances.size(), input.arcs.size()});
  require(attempt.success(),
          "parallel cell baseline factory rejected test fixture");
  DualCsrState state = std::move(*attempt.state);

  const auto violations = validateDualCsrState(state);
  require(violations.empty(),
          "two-parallel committed fixture was rejected"
              + (violations.empty()
                     ? std::string{}
                     : ": " + violations.front().field + " "
                           + violations.front().message));
  return state;
}

DualCsrBatchDescriptor partialParallelReplacement(
    const DualCsrState& before)
{
  auto descriptor = descriptorFor(before, 0, 1);
  auto survivor = before.arc_slots[1].payload;
  survivor.key.parallel_ordinal = 0;
  descriptor.payload_changes = {{
      csr::ArcId{1}, before.arc_slots[1].payload, std::move(survivor)}};
  auto appended = before.arc_slots[0].payload;
  appended.key.parallel_ordinal = 1;
  descriptor.arc_appends = {{
      csr::ArcId{2}, appended.key, appended.source, appended.sink,
      std::move(appended)}};
  descriptor.changes = {{
      csr::NodeId{0}, 0, csr::ArcId{0}, csr::ArcId{2}}};
  descriptor.reverse_changes = {{
      csr::NodeId{1}, 0, csr::ArcId{0}, csr::ArcId{2}}};
  descriptor.logical_deactivations = {deactivate(
      before, csr::ArcId{0},
      LogicalArcDeactivateReason::kReplaced)};
  completeReferencePayload(descriptor, before);
  return descriptor;
}

std::vector<csr::EdgeEntry> rowEntries(
    const DualCsrEdgeViewImpl& edges, csr::NodeId node, bool outgoing)
{
  std::vector<csr::EdgeEntry> result;
  const auto visit = [&](const csr::EdgeEntry& entry) {
    result.push_back(entry);
    return csr::EdgeVisitResult::continueVisit();
  };
  const auto status = outgoing ? edges.forEachOutgoing(node, visit)
                               : edges.forEachIncoming(node, visit);
  require(status.control == csr::EdgeVisitControl::kContinue,
          "physical row traversal failed");
  return result;
}

DualCsrTimingResult timingFor(const DualCsrState& state)
{
  csr::CsrCpuReferenceBackend backend;
  DualCsrEdgeViewImpl edges(state);
  DualCsrTimingPayloadViewImpl payloads(state);
  auto output = backend.computeWithTelemetry(edges, payloads);
  const auto* values
      = std::get_if<csr::CsrTimingResult>(&output.computation.outcome);
  require(values != nullptr,
          "fresh parallel state did not compute timing");
  DualCsrTimingResult timing{*values};
  const auto violations = validateDualCsrTimingResult(state, timing);
  require(violations.empty(),
          "fresh parallel timing was rejected"
              + (violations.empty()
                     ? std::string{}
                     : ": " + violations.front().field + " "
                           + violations.front().message));
  return timing;
}

FunctionalSemanticReceipt receiptFor(const DualCsrState& state)
{
  const auto attempt = projectDualCsrFunctionalSemanticReceipt(
      state, timingFor(state));
  require(attempt.success(),
          "fresh canonical functional receipt was rejected");
  return *attempt.receipt;
}

void sameMembershipRowsFollowCandidateOrder()
{
  const DualCsrState before = twoCommittedParallelArcs();
  auto descriptor = descriptorFor(before, 0, 0);
  descriptor.changes = {
      {csr::NodeId{0}, 0, csr::ArcId{0}, csr::ArcId{1}},
      {csr::NodeId{0}, 1, csr::ArcId{1}, csr::ArcId{0}}};
  descriptor.reverse_changes = {
      {csr::NodeId{1}, 0, csr::ArcId{0}, csr::ArcId{1}},
      {csr::NodeId{1}, 1, csr::ArcId{1}, csr::ArcId{0}}};
  completeReferencePayload(descriptor, before);
  descriptor.affected_base_in_rows.insert(
      descriptor.affected_base_in_rows.begin(),
      {csr::NodeId{0}, {}, {}});

  auto noncanonical = descriptor;
  std::swap(noncanonical.affected_base_in_rows[0],
            noncanonical.affected_base_in_rows[1]);
  const auto rejected = validateDualCsrDescriptor(before, noncanonical);
  require(!rejected.empty()
              && hasCode(rejected,
                         DualCsrViolationCode::kExpectedValue),
          "descriptor accepted noncanonical BaseIn row ordering");

  const DualCsrState after = apply(before, descriptor);
  const DualCsrEdgeViewImpl edges(after);
  require(rowEntries(edges, csr::NodeId{0}, true)
              == std::vector<csr::EdgeEntry>{
                  {csr::ArcId{1}, csr::EdgeStorageKind::kBaseOut},
                  {csr::ArcId{0}, csr::EdgeStorageKind::kBaseOut}}
              && rowEntries(edges, csr::NodeId{1}, false)
                     == std::vector<csr::EdgeEntry>{
                         {csr::ArcId{1}, csr::EdgeStorageKind::kBaseIn},
                         {csr::ArcId{0}, csr::EdgeStorageKind::kBaseIn}},
          "same-membership BaseOut/BaseIn rows lost candidate order");
}

void partialParallelDeactivationKeepsCanonicalActiveKeys()
{
  const DualCsrState before = twoCommittedParallelArcs();
  const DualCsrBatchDescriptor descriptor
      = partialParallelReplacement(before);
  const DualCsrState after = apply(before, descriptor);
  require(validateDualCsrState(after).empty(),
          "partial parallel replacement produced an invalid state");

  const auto& inactive = after.arc_slots[0];
  const auto& survivor = after.arc_slots[1];
  const auto& appended = after.arc_slots[2];
  require(!inactive.active && survivor.active && appended.active
              && survivor.payload.key.parallel_ordinal == 0
              && appended.payload.key.parallel_ordinal == 1,
          "partial replacement did not canonicalize active ordinals");
  require(inactive.payload.key == survivor.payload.key,
          "inactive historical key did not retain the intended collision");

  require(descriptor.affected_base_out_rows.size() == 1
              && descriptor.affected_base_out_rows[0].expected_arc_ids
                     == std::vector<csr::ArcId>{csr::ArcId{0},
                                                csr::ArcId{1}}
              && descriptor.affected_base_out_rows[0].replacement_arc_ids
                     == std::vector<csr::ArcId>{csr::ArcId{2},
                                                csr::ArcId{1}}
              && descriptor.affected_base_in_rows.size() == 1
              && descriptor.affected_base_in_rows[0].replacement_arc_ids
                     == std::vector<csr::ArcId>{csr::ArcId{2},
                                                csr::ArcId{1}},
          "partial replacement lost complete candidate row evidence");
  require(csr::arcKeyLess(survivor.payload.key, appended.payload.key),
          "partial replacement FIFO canary is not reverse ArcKey order");
  const DualCsrEdgeViewImpl edges(after);
  require(rowEntries(edges, csr::NodeId{0}, true)[0].arc_id
                  == csr::ArcId{2}
              && rowEntries(edges, csr::NodeId{1}, false)[0].arc_id
                     == csr::ArcId{2},
          "new arc was not placed before the surviving old arc");

  const DualSemanticReadView view(after);
  const auto keyed = view.arcByKey(survivor.payload.key);
  require(keyed.hasValue()
              && &keyed.value() == &survivor.payload
              && view.arcIdByKey(survivor.payload.key)
                     == csr::ArcId{1},
          "semantic keyed view did not select the active colliding ArcId");
  const auto by_inactive_id = view.arcById(csr::ArcId{0});
  require(!by_inactive_id.hasValue()
              && by_inactive_id.error()
                     == DualSemanticReadError::kInactiveArc,
          "inactive historical ArcId became semantically visible");

  const FunctionalSemanticReceipt receipt = receiptFor(after);
  require(receipt.graph.active_arcs.size() == 2
              && receipt.graph.active_arcs[0].key
                     == survivor.payload.key
              && receipt.graph.active_arcs[1].key
                     == appended.payload.key,
          "fresh receipt disagrees with canonical active ArcKeys");
}

}  // namespace
}  // namespace ista::timing_dual_csr::test

int main()
{
  try {
    ista::timing_dual_csr::test::
        sameMembershipRowsFollowCandidateOrder();
    ista::timing_dual_csr::test::
        partialParallelDeactivationKeepsCanonicalActiveKeys();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
