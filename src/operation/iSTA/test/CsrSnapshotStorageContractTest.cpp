// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#include "CsrModelTestMutation.hpp"
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CsrFingerprint.hpp"
#include "CsrReferenceInputStorageOracle.hpp"
#include "CsrSnapshotTestFixture.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::snapshot_test;

CsrSnapshotStorageReport productionStorage(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata)
{
  auto result = snapshotStorage(input, metadata);
  if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&result)) {
    require(!errors->empty(), "storage error alternative must be nonempty");
    throw std::runtime_error(errors->front().field + ": " + errors->front().message);
  }
  return std::get<CsrSnapshotStorageReport>(result);
}

void requireExactOracle(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata, std::string_view name)
{
  require(productionStorage(input, metadata) == independentReferenceInputStorage(input, metadata),
          std::string(name) + " differs from independent same-ABI oracle");
}

std::uint64_t parseU64(std::string_view text, std::string_view name)
{
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  require(error == std::errc{} && end == text.data() + text.size(), std::string(name) + " is not a canonical u64 operand");
  return value;
}

constexpr std::array<CompiledContractDescriptor, 3> kCases{
    CompiledContractDescriptor{"storage_ready_nonempty", "storage.ready.nonempty", "replace_value", "metadata.level_nodes", "canonical", "-", "valid", "-", "-",
                               "-"},
    CompiledContractDescriptor{"storage_metadata_wrong_fifo_order", "storage.ready.nonempty", "replace_value", "metadata.level_nodes", "swapped", "-",
                               "invalid", "metadata.level_nodes", "-", "must exactly match FIFO replay"},
    CompiledContractDescriptor{"storage_modeled_add_overflow", "storage.overflow.add", "replace_value", "storage.accumulator_operand", "18446744073709551615",
                               "-", "invalid", "storage.host_bytes", "-", "modeled storage byte count overflow"},
};

void evaluateCompiledCase(const CompiledContractDescriptor& descriptor)
{
  require(descriptor.recipe_op == "replace_value", std::string(descriptor.case_id) + " has an unknown recipe operation");
  require(descriptor.action_token_or_dash == "-", std::string(descriptor.case_id) + " storage cases must not carry an action");
  if (descriptor.baseline == "storage.ready.nonempty") {
    auto input = sameLevelSnapshotInput();
    auto metadata = sameLevelSnapshotMetadata();
    requireValid(validateCaptureMetadata(input, metadata), std::string(descriptor.case_id) + " baseline");
    require(descriptor.target == "metadata.level_nodes", std::string(descriptor.case_id) + " has an unknown metadata target");
    if (descriptor.operand == "swapped")
      std::swap(metadata.level_nodes[0], metadata.level_nodes[1]);
    else
      require(descriptor.operand == "canonical", std::string(descriptor.case_id) + " has an unknown level-node operand");

    if (descriptor.expected_validity == "valid") {
      auto result = makeReadySnapshotObservation(input, metadata, 0.5);
      if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&result))
        requireCompiledOutcome(descriptor, *errors);
      else
        requireCompiledOutcome(descriptor, {});
    } else {
      requireCompiledOutcome(descriptor, validateCaptureMetadata(input, metadata));
    }
    return;
  }

  require(descriptor.baseline == "storage.overflow.add", std::string(descriptor.case_id) + " has an unknown storage baseline");
  require(descriptor.target == "storage.accumulator_operand", std::string(descriptor.case_id) + " has an unknown overflow target");
  const auto baseline = checkedStorageAdd(0, 1, "storage.host_bytes");
  require(std::holds_alternative<std::uint64_t>(baseline) && std::get<std::uint64_t>(baseline) == 1,
          std::string(descriptor.case_id) + " overflow baseline is not valid");
  auto result = checkedStorageAdd(parseU64(descriptor.operand, descriptor.case_id), 1, "storage.host_bytes");
  if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&result))
    requireCompiledOutcome(descriptor, *errors);
  else
    requireCompiledOutcome(descriptor, {});
}

enum class MetadataMutation : std::uint8_t
{
  kEmptyOffsets,
  kNonzeroOffsetStart,
  kNonmonotonicOffsets,
  kWrongFinalOffset,
  kShortLevelNodes,
  kShortNodeToLevel,
  kDifferentBuckets,
  kWrongNodeToLevel,
  kDuplicateNode,
  kOutOfBoundsNode,
  kCycle
};

struct MetadataCase
{
  std::string_view name;
  MetadataMutation mutation;
  std::string_view field;
  std::string_view message;
  std::size_t error_count;
};

constexpr std::array kMetadataCases{
    MetadataCase{"empty level offsets", MetadataMutation::kEmptyOffsets, "metadata.level_offsets", "offsets must be nonempty", 2},
    MetadataCase{"nonzero first level offset", MetadataMutation::kNonzeroOffsetStart, "metadata.level_offsets", "offsets must start at zero", 2},
    MetadataCase{"nonmonotonic level offsets", MetadataMutation::kNonmonotonicOffsets, "metadata.level_offsets", "offsets must be monotonic", 2},
    MetadataCase{"wrong final level offset", MetadataMutation::kWrongFinalOffset, "metadata.level_offsets", "final offset must equal node count", 2},
    MetadataCase{"short level nodes", MetadataMutation::kShortLevelNodes, "metadata.level_nodes", "level nodes must contain exactly N entries", 2},
    MetadataCase{"short node-to-level", MetadataMutation::kShortNodeToLevel, "metadata.node_to_level", "node-to-level must contain exactly N entries", 2},
    MetadataCase{"different level buckets", MetadataMutation::kDifferentBuckets, "metadata.level_offsets", "must exactly match FIFO replay", 2},
    MetadataCase{"wrong node-to-level inverse", MetadataMutation::kWrongNodeToLevel, "metadata.node_to_level",
                 "node-to-level must be the inverse of level buckets", 2},
    MetadataCase{"duplicate level node", MetadataMutation::kDuplicateNode, "metadata.level_nodes", "NodeId is duplicated", 4},
    MetadataCase{"out-of-bounds level node", MetadataMutation::kOutOfBoundsNode, "metadata.level_nodes", "NodeId is out of range", 3},
    MetadataCase{"cyclic FIFO replay", MetadataMutation::kCycle, "metadata", "FIFO replay must be complete", 4},
};

void checkMetadataFailures()
{
  for (const auto& test_case : kMetadataCases) {
    auto input = test_case.mutation == MetadataMutation::kCycle ? cycleSnapshotInput() : snapshotInput();
    auto metadata = test_case.mutation == MetadataMutation::kCycle ? sameLevelSnapshotMetadata() : snapshotMetadata();
    if (test_case.mutation == MetadataMutation::kCycle)
      requireValid(validateReferenceInput(input), std::string(test_case.name) + " reference baseline");
    else
      requireValid(validateCaptureMetadata(input, metadata), std::string(test_case.name) + " baseline");
    switch (test_case.mutation) {
      case MetadataMutation::kEmptyOffsets:
        metadata.level_offsets.clear();
        break;
      case MetadataMutation::kNonzeroOffsetStart:
        metadata.level_offsets.front() = 1;
        break;
      case MetadataMutation::kNonmonotonicOffsets:
        metadata.level_offsets = {0, 2, 1, 2};
        break;
      case MetadataMutation::kWrongFinalOffset:
        metadata.level_offsets.back() = 1;
        break;
      case MetadataMutation::kShortLevelNodes:
        metadata.level_nodes.pop_back();
        break;
      case MetadataMutation::kShortNodeToLevel:
        metadata.node_to_level.pop_back();
        break;
      case MetadataMutation::kDifferentBuckets:
        metadata = sameLevelSnapshotMetadata();
        break;
      case MetadataMutation::kWrongNodeToLevel:
        metadata.node_to_level[0] = 2;
        break;
      case MetadataMutation::kDuplicateNode:
        metadata.level_nodes[1] = NodeId{0};
        break;
      case MetadataMutation::kOutOfBoundsNode:
        metadata.level_nodes[1] = NodeId{2};
        break;
      case MetadataMutation::kCycle:
        break;
    }
    requirePrimaryViolation(validateCaptureMetadata(input, metadata), test_case.field, test_case.message, test_case.error_count, test_case.name);
  }
}

void checkRootStrings()
{
  using Mutation = void (*)(CsrReferenceInput&);
  constexpr std::array<Mutation, 11> mutations{
      +[](CsrReferenceInput& input) { input.schema_version.append(40, 's'); },
      +[](CsrReferenceInput& input) { input.source_identity.fingerprint_schema_version.append(40, 'v'); },
      +[](CsrReferenceInput& input) { input.source_identity.topology_fingerprint.append(40, 'a'); },
      +[](CsrReferenceInput& input) { input.source_identity.clock_constraint_fingerprint.append(40, 'b'); },
      +[](CsrReferenceInput& input) { input.source_identity.library_fingerprint.append(40, 'c'); },
      +[](CsrReferenceInput& input) { input.source_identity.corner_fingerprint.append(40, 'd'); },
      +[](CsrReferenceInput& input) { input.source_identity.parasitic_fingerprint.append(40, 'e'); },
      +[](CsrReferenceInput& input) { input.source_identity.timing_input_fingerprint.append(40, 'f'); },
      +[](CsrReferenceInput& input) { input.analysis_context.liberty_corner.append(40, 'l'); },
      +[](CsrReferenceInput& input) { input.analysis_context.temperature.append(40, 't'); },
      +[](CsrReferenceInput& input) { input.analysis_context.rc_corner.append(40, 'r'); },
  };
  for (std::size_t index = 0; index < mutations.size(); ++index) {
    auto input = snapshotInput();
    mutations[index](input);
    requireExactOracle(input, snapshotMetadata(), "root string family " + std::to_string(index));
  }
}

template <typename Mutation>
void checkInputFamilyDelta(Mutation mutate, std::string_view name)
{
  auto input = tableSnapshotInput();
  const auto metadata = snapshotMetadata();
  const auto production_before = productionStorage(input, metadata);
  const auto oracle_before = independentReferenceInputStorage(input, metadata);
  require(production_before == oracle_before, std::string(name) + " baseline differs from independent oracle");
  mutate(input);
  const auto production_after = productionStorage(input, metadata);
  const auto oracle_after = independentReferenceInputStorage(input, metadata);
  require(production_after == oracle_after, std::string(name) + " mutation differs from independent oracle");
  require(production_after.host_bytes.logical >= production_before.host_bytes.logical && oracle_after.host_bytes.logical >= oracle_before.host_bytes.logical,
          std::string(name) + " logical delta unexpectedly decreased");
  require(
      production_after.host_bytes.capacity >= production_before.host_bytes.capacity && oracle_after.host_bytes.capacity >= oracle_before.host_bytes.capacity,
      std::string(name) + " capacity delta unexpectedly decreased");
  require(production_after.host_bytes.logical - production_before.host_bytes.logical == oracle_after.host_bytes.logical - oracle_before.host_bytes.logical,
          std::string(name) + " logical delta differs from independent oracle");
  require(production_after.host_bytes.capacity - production_before.host_bytes.capacity == oracle_after.host_bytes.capacity - oracle_before.host_bytes.capacity,
          std::string(name) + " capacity delta differs from independent oracle");
}

void checkDynamicFamilies()
{
  checkInputFamilyDelta([](auto& input) { input.nodes[0].key.full_name += ":node"; }, "node record");
  checkInputFamilyDelta([](auto& input) { input.node_ids_by_key[0].key.full_name += ":lookup"; }, "node lookup");
  checkInputFamilyDelta([](auto& input) { input.instances.push_back({"u2", "BUF_X2", {}, false, false, false, false}); }, "instance record");
  checkInputFamilyDelta([](auto& input) { input.arcs[0].key.base.owner_name += ":arc"; }, "arc key");
  checkInputFamilyDelta(
      [](auto& input) {
        ista::csr::test::mutateChildren(input.arcs[0].transfer, [&](auto& children) { children[0].delay_tables[0].axes.push_back({0.3, 0.4});
        children[0].delay_tables[0].values.push_back(0.5); });
      },
      "nested array table");
  checkInputFamilyDelta(
      [](auto& input) {
        ista::csr::test::mutateChildren(input.arcs[0].transfer, [&](auto& children) {
      auto& table = children[0].check_tables[0];
        table.present = true;
        table.variable1 = TableVariable::kInputTransition;
        table.axes = {{0.6, 0.7}};
        table.values = {0.8, 0.9};
    });
      },
      "nested check table");
  checkInputFamilyDelta([](auto& input) { input.arc_ids_by_key[0].key.base.owner_name += ":lookup"; }, "arc lookup");
  checkInputFamilyDelta(
      [](auto& input) {
        input.out_row_offsets.push_back(2);
        input.out_arc_ids.push_back(ArcId{0});
        input.in_row_offsets.push_back(2);
        input.in_arc_ids.push_back(ArcId{0});
      },
      "CSR vectors");
  checkInputFamilyDelta(
      [](auto& input) {
        input.ordered_root_ids.push_back(NodeId{1});
        input.ordered_start_ids.push_back(NodeId{1});
        input.ordered_end_ids.push_back(NodeId{0});
      },
      "ordered node vectors");
  checkInputFamilyDelta([](auto& input) { input.clock_tags.push_back("other_clock"); }, "clock tags");
  checkInputFamilyDelta([](auto& input) { input.clocks.push_back({"clk2", "embedded", {RawNodeReference{"raw", std::nullopt}}, 1.0, {}, false}); },
                        "clock constraints");
  checkInputFamilyDelta([](auto& input) { input.clock_owner_by_node.push_back(std::string{"owner"}); }, "clock owners");
  checkInputFamilyDelta(
      [](auto& input) {
        PortConstraintModel constraint;
        constraint.map_key = "port";
        constraint.embedded_port_name = "embedded";
        constraint.clock_name = "constraint_clock";
        input.port_constraints.push_back(std::move(constraint));
      },
      "port constraints");
  checkInputFamilyDelta([](auto& input) { input.case_analysis_constraints.push_back({"raw_case", std::nullopt, true}); }, "case constraints");
  checkInputFamilyDelta(
      [](auto& input) { input.timing_check_endpoints.push_back({"u1", 0, TimingCheckKind::kSetup, "CK", "D", "u1:CK", "u1:D", std::nullopt, std::nullopt}); },
      "timing check endpoints");
  checkInputFamilyDelta(
      [](auto& input) {
        ReferencedLibraryCellModel cell;
        cell.cell_name = "ZZ_CELL";
        cell.present = true;
        cell.library_name = "extra.lib";
        cell.ports.push_back({.name = "A", .direction = PinDirection::kInput, .transition_capacitance = {}});
        input.referenced_library_cells.push_back(std::move(cell));
      },
      "referenced library closure");
  checkInputFamilyDelta(
      [](auto& input) {
        ChildTimingArcModel child;
        child.signature.library_name = "extra.lib";
        child.signature.sdf_condition = "check";
        child.check_tables[1].present = true;
        child.check_tables[1].variable1 = TableVariable::kConstrainedTransition;
        child.check_tables[1].axes = {{1.1, 1.2}};
        child.check_tables[1].values = {1.3, 1.4};
        LibraryTimingCheckArcModel check;
        check.clock_port = "CK";
        check.data_port = "D";
        check.kind = TimingCheckKind::kSetup;
        check.setChildren({std::move(child)});
        input.referenced_library_cells[0].timing_checks.push_back(std::move(check));
      },
      "referenced library timing checks");
  checkInputFamilyDelta(
      [](auto& input) {
        input.parasitics.capacitance_unit = "PF";
        input.parasitics.resistance_unit = "OHM";
        input.parasitics.nets.push_back({"n", "embedded_n", 0.2, {{"1", "embedded_1", 0.1, 1.0, 2.0}}, {{"1", "2", 3.0}}});
      },
      "typed parasitics");
  checkInputFamilyDelta([](auto& input) { input.start_seeds[0].clock_tag += ":seed"; }, "start seeds");
  checkInputFamilyDelta([](auto& input) { input.endpoint_required.push_back({NodeId{1}, "endpoint_clock", 1.0, {}}); }, "endpoint required");
  checkInputFamilyDelta([](auto& input) { input.cell_pin_capacitances.push_back({NodeId{0}, 0.1, {}}); }, "cell pin capacitance");
  checkInputFamilyDelta([](auto& input) { input.unsupported_reasons.push_back(CsrReferenceUnsupportedReason::kParasiticNet); }, "unsupported reasons");

  auto input = tableSnapshotInput();
  auto metadata = snapshotMetadata();
  const auto production_before = productionStorage(input, metadata);
  const auto oracle_before = independentReferenceInputStorage(input, metadata);
  metadata.level_offsets.reserve(15);
  metadata.level_nodes.reserve(16);
  metadata.node_to_level.reserve(17);
  const auto production_after = productionStorage(input, metadata);
  const auto oracle_after = independentReferenceInputStorage(input, metadata);
  require(production_after == oracle_after, "metadata vector family differs from independent oracle");
  require(production_after.host_bytes.logical - production_before.host_bytes.logical == oracle_after.host_bytes.logical - oracle_before.host_bytes.logical,
          "metadata logical delta differs from independent oracle");
  require(production_after.host_bytes.capacity - production_before.host_bytes.capacity == oracle_after.host_bytes.capacity - oracle_before.host_bytes.capacity,
          "metadata capacity delta differs from independent oracle");
}

void checkReserveAndTerminatorRules()
{
  auto input = snapshotInput();
  const auto canonical = canonicalReferenceInputBytes(input);
  const auto before = productionStorage(input, snapshotMetadata());
  const std::size_t old_capacity = input.arcs.capacity();
  input.arcs.reserve(old_capacity + 12);
  const auto after = productionStorage(input, snapshotMetadata());
  require(after.host_bytes.logical == before.host_bytes.logical, "reserve changed logical modeled bytes");
  require(after.host_bytes.capacity - before.host_bytes.capacity == (input.arcs.capacity() - old_capacity) * sizeof(ArcRecord),
          "reserved slots were recursively visited or vector slots were not counted exactly once");
  require(canonicalReferenceInputBytes(input) == canonical, "reserve changed canonical input bytes");
  requireExactOracle(input, snapshotMetadata(), "reserved slots");

  input.schema_version.reserve(input.schema_version.capacity() + 100);
  const auto string_before = productionStorage(input, snapshotMetadata());
  input.schema_version.push_back('x');
  const auto string_after = productionStorage(input, snapshotMetadata());
  require(string_after.host_bytes.logical == string_before.host_bytes.logical + 1, "string terminator was counted as a logical byte");
  require(string_after.host_bytes.capacity == string_before.host_bytes.capacity, "pre-reserved string unexpectedly changed capacity");
  requireExactOracle(input, snapshotMetadata(), "string terminator");

  auto slot_input = snapshotInput();
  slot_input.unsupported_reasons.reserve(1);
  const auto slot_before = productionStorage(slot_input, snapshotMetadata());
  slot_input.unsupported_reasons.push_back(CsrReferenceUnsupportedReason::kParasiticNet);
  const auto slot_after = productionStorage(slot_input, snapshotMetadata());
  require(slot_after.host_bytes.logical - slot_before.host_bytes.logical == sizeof(CsrReferenceUnsupportedReason),
          "constructed vector slot was not counted exactly once");
  require(slot_after.host_bytes.capacity == slot_before.host_bytes.capacity, "constructing a pre-reserved scalar slot unexpectedly changed capacity bytes");
  requireExactOracle(slot_input, snapshotMetadata(), "constructed vector slot");
}

void checkFixedOptionalStorage()
{
  auto input = snapshotInput();
  input.arcs[0].output_load = ArcOutputLoadMatrix{};
  const auto engaged = productionStorage(input, snapshotMetadata());
  const auto engaged_oracle = independentReferenceInputStorage(input, snapshotMetadata());
  input.arcs[0].output_load.reset();
  const auto absent = productionStorage(input, snapshotMetadata());
  const auto absent_oracle = independentReferenceInputStorage(input, snapshotMetadata());
  require(engaged == engaged_oracle && absent == absent_oracle, "optional load storage differs from independent oracle");
  require(engaged.host_bytes == absent.host_bytes, "optional fixed 2x2 output-load matrix must not own recursively counted backing storage");
}

void checkCopyMoveAndArithmetic()
{
  auto input = tableSnapshotInput();
  input.schema_version = "a";
  requireExactOracle(input, snapshotMetadata(), "SSO short string");
  input.schema_version.assign(200, 'z');
  requireExactOracle(input, snapshotMetadata(), "long string");
  CsrReferenceInput copied = input;
  requireExactOracle(copied, snapshotMetadata(), "copied capacities");
  CsrReferenceInput moved = std::move(copied);
  requireExactOracle(moved, snapshotMetadata(), "moved capacities");

  const auto multiply = checkedStorageMultiply(std::numeric_limits<std::uint64_t>::max(), 2, "storage.host_bytes");
  const auto* multiply_errors = std::get_if<std::vector<CsrContractViolation>>(&multiply);
  require(multiply_errors != nullptr, "checked multiply failed to detect overflow");
  requireExactViolation(*multiply_errors, "storage.host_bytes", "modeled storage byte count overflow", "checked multiply overflow");
  const std::array values{std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1}};
  const auto sum = checkedStorageSum(values, "storage.host_bytes");
  const auto* sum_errors = std::get_if<std::vector<CsrContractViolation>>(&sum);
  require(sum_errors != nullptr, "checked sum failed to detect overflow");
  requireExactViolation(*sum_errors, "storage.host_bytes", "modeled storage byte count overflow", "checked sum overflow");
}

}  // namespace

int main(int argc, char** argv)
{
  if (listCompiledContractsIfRequested(argc, argv, kCases))
    return 0;
  for (const auto& test_case : kCases)
    evaluateCompiledCase(test_case);
  checkMetadataFailures();
  requireExactOracle(emptySnapshotInput(), emptySnapshotMetadata(), "empty storage");
  requireExactOracle(snapshotInput(), snapshotMetadata(), "nonempty storage");
  requireExactOracle(tableSnapshotInput(), snapshotMetadata(), "nested table storage");
  checkRootStrings();
  checkDynamicFamilies();
  checkReserveAndTerminatorRules();
  checkFixedOptionalStorage();
  checkCopyMoveAndArithmetic();
  return 0;
}
