// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "BufferReplayContext.hpp"
#include "BufferTraceReplayer.hpp"

namespace {

using namespace ista;
using namespace ista::buffer_trace;

static_assert(!std::is_default_constructible_v<BufferReplayContext>);
static_assert(!std::is_copy_constructible_v<BufferReplayContext>);
static_assert(!std::is_copy_assignable_v<BufferReplayContext>);
static_assert(!std::is_move_constructible_v<BufferReplayContext>);
static_assert(!std::is_move_assignable_v<BufferReplayContext>);
static_assert(!std::is_destructible_v<BufferReplayContext>);
static_assert(std::is_copy_constructible_v<ReplayReceipt>);
static_assert(std::is_move_constructible_v<ReplayReceipt>);

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

NativeDatabaseFunctionalReceipt databaseReceipt()
{
  return NativeDatabaseFunctionalReceipt{
      .design_name = "stage4",
      .instances = {NativeInstanceFunctionalRecord{
          .name = "u0",
          .cell = "OR2_X1",
          .ordered_pins = {"u0:A", "u0:B", "u0:Y"}}},
      .pins = {NativePinFunctionalRecord{
          .full_name = "u0:A",
          .pin_name = "A",
          .instance_name = "u0",
          .net_name = "fanout",
          .direction = 1}},
      .nets = {NativeNetFunctionalRecord{
          .name = "fanout",
          .ordered_drivers = {"data_in"},
          .ordered_loads = {"u0:A"},
          .ordered_pins = {"data_in", "u0:A"}}}};
}

FunctionalBatchReceipt batchReceipt()
{
  FunctionalBatchReceipt value;
  value.batch_ordinal = 0;
  value.expected_generation = timing_edit::TransactionGeneration{0};
  value.target_generation = timing_edit::TransactionGeneration{1};
  value.post_identity.generation = timing_edit::TransactionGeneration{1};
  value.csr_timing.level_offsets = {0, 1};
  value.csr_timing.level_nodes = {csr::NodeId{0}};
  value.native_database = databaseReceipt();
  return value;
}

ReplayReceipt replayReceipt()
{
  ReplayReceipt value;
  value.header = ReplayHeader{
      .workload_kind = SemanticWorkloadKind::kRealDesign,
      .profile_id = ProfileId::kEngineeringSmoke,
      .comparability_status = ComparabilityStatus::kNonComparable,
      .comparability_reason = ComparabilityReason::kEngineeringRcNoPhysical};
  value.prefix_batches = 1;
  value.batches = {batchReceipt()};
  value.committed_identity.generation = timing_edit::TransactionGeneration{1};
  value.final_database = databaseReceipt();
  value.final_csr_timing.level_offsets = {0, 1};
  value.final_csr_timing.level_nodes = {csr::NodeId{0}};
  return value;
}

void testFunctionalEqualityIsFieldLevelAndValueOnly()
{
  const ReplayReceipt baseline = replayReceipt();
  require(functionallyEqual(baseline, baseline),
          "receipt is not functionally equal to itself");

  auto changed = baseline;
  changed.final_database.nets[0].ordered_loads.push_back("u1:A");
  require(!functionallyEqual(baseline, changed),
          "raw native connectivity difference was hidden");

  changed = baseline;
  changed.final_csr_timing.level_offsets[1] = 2;
  require(!functionallyEqual(baseline, changed),
          "CSR timing structure difference was hidden");

  changed = baseline;
  changed.final_csr_input.source_identity.topology_fingerprint
      = "different-final-source";
  require(!functionallyEqual(baseline, changed),
          "final CSR source identity difference was hidden");

  changed = baseline;
  changed.batches[0].csr_timing.level_nodes[0] = csr::NodeId{1};
  require(!functionallyEqual(baseline, changed),
          "per-batch CSR result difference was hidden");

  changed = baseline;
  changed.batches[0].native_database.pins[0].net_name = "other";
  require(!functionallyEqual(baseline, changed),
          "per-batch native pin-to-net difference was hidden");

  changed = baseline;
  changed.prefix_batches = 0;
  require(!functionallyEqual(baseline, changed),
          "prefix identity difference was hidden");

  changed = baseline;
  changed.header.comparability_reason.reset();
  require(!functionallyEqual(baseline, changed),
          "replay-safe header difference was hidden");
}

void testBatchEqualityCoversSemanticAndTimingFields()
{
  const FunctionalBatchReceipt baseline = batchReceipt();
  require(functionallyEqual(baseline, baseline),
          "batch is not functionally equal to itself");

  auto changed = baseline;
  changed.target_generation = timing_edit::TransactionGeneration{2};
  require(!functionallyEqual(baseline, changed),
          "generation difference was hidden");

  changed = baseline;
  changed.csr_timing.level_offsets.push_back(2);
  require(!functionallyEqual(baseline, changed),
          "timing-result difference was hidden");

  changed = baseline;
  changed.csr_input.source_identity.topology_fingerprint
      = "different-batch-source";
  require(!functionallyEqual(baseline, changed),
          "batch CSR source identity difference was hidden");

  changed = baseline;
  changed.native_database.instances[0].cell = "BUF_X1";
  require(!functionallyEqual(baseline, changed),
          "native instance master difference was hidden");
}

}  // namespace

int main()
{
  testFunctionalEqualityIsFieldLevelAndValueOnly();
  testBatchEqualityCoversSemanticAndTimingFields();
  return 0;
}
