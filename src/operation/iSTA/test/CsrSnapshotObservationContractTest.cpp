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
#include <array>
#include <limits>
#include <utility>
#include <vector>

#include "CsrSnapshotTestFixture.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::snapshot_test;

void setShape(CsrSnapshotObservation& value, std::uint64_t nodes, std::uint64_t arcs, std::uint64_t levels)
{
  const auto exact = [](std::uint64_t size) { return CsrBufferExtent{size, size}; };
  const auto plus_one = [&](std::uint64_t size) { return size == std::numeric_limits<std::uint64_t>::max() ? CsrBufferExtent{} : exact(size + 1); };
  value.counts = {nodes, arcs, levels};
  value.buffers.node_keys = exact(nodes);
  value.buffers.arc_keys = exact(arcs);
  value.buffers.out_row_offsets = plus_one(nodes);
  value.buffers.out_arc_ids = exact(arcs);
  value.buffers.in_row_offsets = plus_one(nodes);
  value.buffers.in_arc_ids = exact(arcs);
  value.buffers.level_offsets = plus_one(levels);
  value.buffers.level_nodes = exact(nodes);
  value.buffers.node_to_level = exact(nodes);
}

CsrSnapshotObservation compiledBaseline(std::string_view baseline)
{
  if (baseline == "observation.ready.empty")
    return emptyReadyObservation();
  if (baseline == "observation.ready.nonempty")
    return readyObservation();
  if (baseline == "observation.ready.rejected")
    return rejectedObservation();
  require(baseline == "observation.invalidated.topology", std::string(baseline) + " is an unknown observation baseline");
  return invalidatedObservation();
}

constexpr std::array<CompiledContractDescriptor, 9> kCases{
    CompiledContractDescriptor{"observation_ready_valid", "observation.ready.empty", "replace_value", "observation.state", "ready", "-", "valid", "-", "-",
                               "-"},
    CompiledContractDescriptor{"observation_unknown_state", "observation.ready.empty", "unknown_enum", "observation.state", "255", "-", "invalid",
                               "observation.state", "-", "unknown snapshot state"},
    CompiledContractDescriptor{"observation_empty_graph_with_arc", "observation.ready.empty", "replace_value", "observation.counts.arcs", "1", "-", "invalid",
                               "observation.counts.arcs", "-", "empty graph cannot contain arcs"},
    CompiledContractDescriptor{"observation_buffer_capacity_small", "observation.ready.nonempty", "replace_value", "observation.buffers.node_keys.capacity",
                               "0", "-", "invalid", "observation.buffers.node_keys", "-", "capacity must be at least logical size"},
    CompiledContractDescriptor{"observation_support_mismatch", "observation.ready.empty", "replace_value", "observation.support.reference_supported", "false",
                               "-", "invalid", "observation.support", "-", "reference_supported must match unsupported reasons"},
    CompiledContractDescriptor{"observation_nonfinite_export", "observation.ready.empty", "positive_infinity", "observation.timing_ms.export", "inf", "-",
                               "invalid", "observation.timing_ms.export", "-", "must be finite and nonnegative"},
    CompiledContractDescriptor{"observation_rejection_source_priority", "observation.ready.rejected", "replace_value", "observation.last_rejection.reason",
                               "session_mismatch", "-", "invalid", "observation.last_rejection", "-", "rejection reason and source identity disagree"},
    CompiledContractDescriptor{"observation_invalidation_first_difference", "observation.invalidated.topology", "replace_value",
                               "observation.last_invalidation.reason", "library_changed", "-", "invalid", "observation.last_invalidation.reason", "-",
                               "must identify the first changed fingerprint"},
    CompiledContractDescriptor{"observation_invalidated_event_combo", "observation.invalidated.topology", "replace_value", "observation.last_rejection",
                               "present", "-", "invalid", "observation.state", "-", "invalidated state requires only an invalidation event"},
};

void evaluateCompiledCase(const CompiledContractDescriptor& descriptor)
{
  auto value = compiledBaseline(descriptor.baseline);
  requireValid(validateSnapshotObservation(value), std::string(descriptor.case_id) + " baseline");
  require(descriptor.action_token_or_dash == "-", std::string(descriptor.case_id) + " observation cases must not carry an action");
  if (descriptor.recipe_op == "unknown_enum") {
    require(descriptor.target == "observation.state" && descriptor.operand == "255", std::string(descriptor.case_id) + " has an unknown enum recipe");
    value.state = static_cast<CsrSnapshotState>(255);
  } else if (descriptor.recipe_op == "positive_infinity") {
    require(descriptor.target == "observation.timing_ms.export" && descriptor.operand == "inf",
            std::string(descriptor.case_id) + " has an unknown infinity recipe");
    value.timing_ms.export_ms = std::numeric_limits<double>::infinity();
  } else {
    require(descriptor.recipe_op == "replace_value", std::string(descriptor.case_id) + " has an unknown recipe operation");
    if (descriptor.target == "observation.state") {
      require(descriptor.operand == "ready", std::string(descriptor.case_id) + " has an unknown state operand");
      value.state = CsrSnapshotState::kReady;
    } else if (descriptor.target == "observation.counts.arcs") {
      require(descriptor.operand == "1", std::string(descriptor.case_id) + " has an unknown arc-count operand");
      setShape(value, 0, 1, 0);
    } else if (descriptor.target == "observation.buffers.node_keys.capacity") {
      require(descriptor.operand == "0", std::string(descriptor.case_id) + " has an unknown capacity operand");
      value.buffers.node_keys.capacity = 0;
    } else if (descriptor.target == "observation.support.reference_supported") {
      require(descriptor.operand == "false", std::string(descriptor.case_id) + " has an unknown support operand");
      value.support.reference_supported = false;
    } else if (descriptor.target == "observation.last_rejection.reason") {
      require(descriptor.operand == "session_mismatch" && value.last_rejection, std::string(descriptor.case_id) + " has an unknown rejection recipe");
      value.last_rejection->reason = CsrScatterCode::kSessionMismatch;
    } else if (descriptor.target == "observation.last_invalidation.reason") {
      require(descriptor.operand == "library_changed" && value.last_invalidation, std::string(descriptor.case_id) + " has an unknown invalidation recipe");
      value.last_invalidation->reason = CsrInvalidationReason::kLibraryChanged;
    } else {
      require(descriptor.target == "observation.last_rejection" && descriptor.operand == "present",
              std::string(descriptor.case_id) + " has an unknown event recipe");
      value.last_rejection = CsrScatterRejectionEvent{CsrScatterCode::kSchemaMismatch, 0, std::nullopt, value.identity};
    }
  }
  requireCompiledOutcome(descriptor, validateSnapshotObservation(value));
}

struct ExtentCase
{
  std::string_view name;
  CsrBufferExtent CsrSnapshotBufferMetrics::* member;
  std::string_view field;
};

constexpr std::array kExtentCases{
    ExtentCase{"node keys", &CsrSnapshotBufferMetrics::node_keys, "observation.buffers.node_keys.logical_size"},
    ExtentCase{"arc keys", &CsrSnapshotBufferMetrics::arc_keys, "observation.buffers.arc_keys.logical_size"},
    ExtentCase{"out row offsets", &CsrSnapshotBufferMetrics::out_row_offsets, "observation.buffers.out_row_offsets.logical_size"},
    ExtentCase{"out arc ids", &CsrSnapshotBufferMetrics::out_arc_ids, "observation.buffers.out_arc_ids.logical_size"},
    ExtentCase{"in row offsets", &CsrSnapshotBufferMetrics::in_row_offsets, "observation.buffers.in_row_offsets.logical_size"},
    ExtentCase{"in arc ids", &CsrSnapshotBufferMetrics::in_arc_ids, "observation.buffers.in_arc_ids.logical_size"},
    ExtentCase{"level offsets", &CsrSnapshotBufferMetrics::level_offsets, "observation.buffers.level_offsets.logical_size"},
    ExtentCase{"level nodes", &CsrSnapshotBufferMetrics::level_nodes, "observation.buffers.level_nodes.logical_size"},
    ExtentCase{"node to level", &CsrSnapshotBufferMetrics::node_to_level, "observation.buffers.node_to_level.logical_size"},
};

void checkExtentMatrix()
{
  for (const auto& test_case : kExtentCases) {
    auto value = readyObservation();
    requireValid(validateSnapshotObservation(value), std::string(test_case.name) + " baseline");
    auto& extent = value.buffers.*test_case.member;
    ++extent.logical_size;
    extent.capacity = extent.logical_size;
    requireExactViolation(validateSnapshotObservation(value), test_case.field, "logical size does not match the frozen count", test_case.name);
  }
}

struct ShapeCase
{
  std::string_view name;
  std::uint64_t nodes;
  std::uint64_t arcs;
  std::uint64_t levels;
  std::string_view field;
  std::string_view message;
  std::size_t error_count;
};

constexpr std::array kShapeCases{
    ShapeCase{"zero nodes with levels", 0, 0, 1, "observation.counts.levels", "N is zero if and only if L is zero", 1},
    ShapeCase{"nonempty zero levels", 1, 0, 0, "observation.counts.levels", "N is zero if and only if L is zero", 2},
    ShapeCase{"levels exceed nodes", 1, 0, 2, "observation.counts.levels", "nonempty observations require 1 <= L <= N", 1},
    ShapeCase{"node plus-one overflow", std::numeric_limits<std::uint64_t>::max(), 0, 1, "observation.counts.nodes", "node count plus one overflows u64", 1},
    ShapeCase{"level plus-one overflow", std::numeric_limits<std::uint64_t>::max() - 1, 0, std::numeric_limits<std::uint64_t>::max(),
              "observation.counts.levels", "level count plus one overflows u64", 2},
};

void checkShapeDomains()
{
  for (const auto& test_case : kShapeCases) {
    auto value = emptyReadyObservation();
    requireValid(validateSnapshotObservation(value), std::string(test_case.name) + " baseline");
    setShape(value, test_case.nodes, test_case.arcs, test_case.levels);
    requirePrimaryViolation(validateSnapshotObservation(value), test_case.field, test_case.message, test_case.error_count, test_case.name);
  }
}

void checkValidStateShapes()
{
  requireValid(validateSnapshotObservation(readyObservation()), "ready state shape");
  auto rejected = rejectedObservation();
  requireValid(validateSnapshotObservation(rejected), "ready rejection shape");
  rejected.state = CsrSnapshotState::kReleased;
  requireValid(validateSnapshotObservation(rejected), "released rejection shape");
  auto invalidated = invalidatedObservation();
  requireValid(validateSnapshotObservation(invalidated), "invalidated shape");
  invalidated.state = CsrSnapshotState::kReleased;
  requireValid(validateSnapshotObservation(invalidated), "released invalidation shape");
  requireValid(validateSnapshotObservation(releasedObservation()), "released ready shape");
}

void checkRejectionMatrix()
{
  const std::array reasons{CsrScatterCode::kSchemaMismatch,         CsrScatterCode::kSessionMismatch,    CsrScatterCode::kGenerationMismatch,
                           CsrScatterCode::kSourceIdentityMismatch, CsrScatterCode::kShapeMismatch,      CsrScatterCode::kResultNumericInvalid,
                           CsrScatterCode::kIdMappingMismatch,      CsrScatterCode::kPredecessorMismatch};
  for (CsrScatterCode reason : reasons) {
    auto observation = readyObservation();
    observation.timing_ms.scatter_ms = 0.0;
    std::optional<CsrSnapshotIdentity> source;
    if (reason != CsrScatterCode::kSchemaMismatch) {
      source = observation.identity;
      if (reason == CsrScatterCode::kSessionMismatch)
        ++source->session_token;
      else if (reason == CsrScatterCode::kGenerationMismatch)
        source->graph_generation = 1;
      else if (reason == CsrScatterCode::kSourceIdentityMismatch)
        source->topology_fingerprint.assign(64, '0');
    }
    observation.last_rejection = CsrScatterRejectionEvent{reason, 0, source, observation.identity};
    requireValid(validateSnapshotObservation(observation), "valid rejection reason matrix");
  }

  auto session_priority = readyObservation();
  auto session_source = session_priority.identity;
  ++session_source.session_token;
  ++session_source.graph_generation;
  session_source.topology_fingerprint.assign(64, '0');
  session_priority.timing_ms.scatter_ms = 0.0;
  session_priority.last_rejection = CsrScatterRejectionEvent{CsrScatterCode::kSessionMismatch, 0, session_source, session_priority.identity};
  requireValid(validateSnapshotObservation(session_priority), "session mismatch priority over generation and fingerprint differences");

  auto generation_priority = readyObservation();
  auto generation_source = generation_priority.identity;
  ++generation_source.graph_generation;
  generation_source.topology_fingerprint.assign(64, '0');
  generation_priority.timing_ms.scatter_ms = 0.0;
  generation_priority.last_rejection = CsrScatterRejectionEvent{CsrScatterCode::kGenerationMismatch, 0, generation_source, generation_priority.identity};
  requireValid(validateSnapshotObservation(generation_priority), "generation mismatch priority over fingerprint differences");

  auto unknown = readyObservation();
  requireValid(validateSnapshotObservation(unknown), "unknown rejection baseline");
  unknown.timing_ms.scatter_ms = 0.0;
  unknown.last_rejection = CsrScatterRejectionEvent{static_cast<CsrScatterCode>(255), 0, std::nullopt, unknown.identity};
  requireExactViolation(validateSnapshotObservation(unknown), "observation.last_rejection.reason", "reason must be one of the eight rejection codes",
                        "unknown rejection reason");
}

void checkInvalidationMatrix()
{
  const std::array reasons{CsrInvalidationReason::kTopologyChanged, CsrInvalidationReason::kClockConstraintChanged, CsrInvalidationReason::kLibraryChanged,
                           CsrInvalidationReason::kCornerChanged,   CsrInvalidationReason::kParasiticChanged,       CsrInvalidationReason::kTimingInputChanged};
  for (std::size_t index = 0; index < reasons.size(); ++index) {
    auto observation = readyObservation();
    auto current = matchingFingerprints(observation.identity);
    switch (index) {
      case 0:
        current.topology_fingerprint.reset();
        break;
      case 1:
        current.clock_constraint_fingerprint.reset();
        break;
      case 2:
        current.library_fingerprint.reset();
        break;
      case 3:
        current.corner_fingerprint.reset();
        break;
      case 4:
        current.parasitic_fingerprint.reset();
        break;
      default:
        current.timing_input_fingerprint.reset();
        break;
    }
    observation.state = CsrSnapshotState::kInvalidated;
    observation.timing_ms.scatter_ms = 0.0;
    observation.last_invalidation = CsrInvalidationEvent{reasons[index], 0, observation.identity, current};
    requireValid(validateSnapshotObservation(observation), "valid invalidation reason matrix");
  }

  auto priority = readyObservation();
  auto current = matchingFingerprints(priority.identity);
  current.topology_fingerprint.reset();
  current.library_fingerprint.reset();
  priority.state = CsrSnapshotState::kInvalidated;
  priority.timing_ms.scatter_ms = 0.0;
  priority.last_invalidation = CsrInvalidationEvent{CsrInvalidationReason::kTopologyChanged, 0, priority.identity, current};
  requireValid(validateSnapshotObservation(priority), "topology invalidation priority over later fingerprint differences");
  requireValid(validateSnapshotObservation(readyObservation()), "multiple-difference invalidation negative baseline");
  priority.last_invalidation->reason = CsrInvalidationReason::kLibraryChanged;
  requireExactViolation(validateSnapshotObservation(priority), "observation.last_invalidation.reason", "must identify the first changed fingerprint",
                        "multiple-difference invalidation priority");
}

void checkStaticEdgesAndAtomicBuilder()
{
  auto observation = readyObservation();
  observation.timing_ms.reference_ms = 0.0;
  observation.timing_ms.scatter_ms = 0.0;
  requireValid(validateSnapshotObservation(observation), "real zero durations");
  observation.support = {false, {CsrReferenceUnsupportedReason::kParasiticNet}};
  requireValid(validateSnapshotObservation(observation), "valid unsupported observation");

  observation = readyObservation();
  requireValid(validateSnapshotObservation(observation), "host root lower-bound baseline");
  observation.host_bytes.logical = sizeof(CsrReferenceInput) + sizeof(CsrCaptureMetadata) - 1;
  requireExactViolation(validateSnapshotObservation(observation), "observation.host_bytes.logical", "modeled host bytes must include both root objects",
                        "host root lower bound");
  observation = rejectedObservation();
  requireValid(validateSnapshotObservation(observation), "event scatter timing baseline");
  observation.timing_ms.scatter_ms.reset();
  requireExactViolation(validateSnapshotObservation(observation), "observation.timing_ms.scatter", "an event requires a scatter duration",
                        "event scatter timing");

  auto input = sameLevelSnapshotInput();
  auto metadata = sameLevelSnapshotMetadata();
  requireValid(validateCaptureMetadata(input, metadata), "atomic builder metadata baseline");
  std::swap(metadata.level_nodes[0], metadata.level_nodes[1]);
  auto result = makeReadySnapshotObservation(input, metadata, 0.5);
  const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&result);
  require(errors != nullptr, "invalid metadata returned a partial ready observation");
  requireExactViolation(*errors, "metadata.level_nodes", "must exactly match FIFO replay", "atomic builder metadata failure");
}

}  // namespace

int main(int argc, char** argv)
{
  if (listCompiledContractsIfRequested(argc, argv, kCases))
    return 0;
  for (const auto& test_case : kCases)
    evaluateCompiledCase(test_case);
  checkExtentMatrix();
  checkShapeDomains();
  checkValidStateShapes();
  checkRejectionMatrix();
  checkInvalidationMatrix();
  checkStaticEdgesAndAtomicBuilder();
  return 0;
}
