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
#include "CsrSnapshotTestFixture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "CsrValidatorTestFixture.hpp"

namespace ista::csr::snapshot_test {
namespace {

struct Bytes
{
  std::uint64_t logical{0};
  std::uint64_t capacity{0};
};

std::uint64_t checkedAdd(std::uint64_t lhs, std::uint64_t rhs)
{
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
    throw std::overflow_error("independent storage add overflow");
  return lhs + rhs;
}

std::uint64_t checkedMultiply(std::uint64_t lhs, std::uint64_t rhs)
{
  if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs)
    throw std::overflow_error("independent storage multiply overflow");
  return lhs * rhs;
}

std::uint64_t asU64(std::size_t value)
{
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t))
    if (value > std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("independent storage conversion overflow");
  return static_cast<std::uint64_t>(value);
}

Bytes plus(Bytes lhs, Bytes rhs)
{
  return {checkedAdd(lhs.logical, rhs.logical), checkedAdd(lhs.capacity, rhs.capacity)};
}

Bytes backing(std::size_t size, std::size_t capacity, std::size_t width)
{
  return {checkedMultiply(asU64(size), asU64(width)), checkedMultiply(asU64(capacity), asU64(width))};
}

Bytes dynamic(const std::string& value)
{
  return {asU64(value.size()), asU64(value.capacity())};
}

template <typename T, typename Dynamic>
Bytes vectorBytes(const std::vector<T>& values, Dynamic element_dynamic)
{
  Bytes result = backing(values.size(), values.capacity(), sizeof(T));
  for (const auto& value : values)
    result = plus(result, element_dynamic(value));
  return result;
}

template <typename T>
Bytes scalarVectorBytes(const std::vector<T>& values)
{
  return backing(values.size(), values.capacity(), sizeof(T));
}

Bytes dynamic(const NodeKey& value)
{
  return plus(plus(dynamic(value.full_name), dynamic(value.instance_name)), dynamic(value.pin_name));
}

Bytes dynamic(const LibraryArcSignature& value)
{
  return plus(dynamic(value.library_name), dynamic(value.sdf_condition));
}

Bytes dynamic(const ArcKeyBase& value)
{
  Bytes result = plus(dynamic(value.source), dynamic(value.sink));
  result = plus(result, dynamic(value.owner_name));
  result = plus(result, dynamic(value.library_source_port));
  result = plus(result, dynamic(value.library_sink_port));
  return plus(result, vectorBytes(value.library_arcs, [](const auto& item) { return dynamic(item); }));
}

Bytes dynamic(const ArcKey& value)
{
  return dynamic(value.base);
}

Bytes dynamic(const NodeIdLookupEntry& value)
{
  return dynamic(value.key);
}

Bytes dynamic(const ArcIdLookupEntry& value)
{
  return dynamic(value.key);
}

Bytes dynamic(const NodeRecord& value)
{
  return plus(plus(dynamic(value.key), dynamic(value.instance_cell)), dynamic(value.net_name));
}

Bytes dynamic(const InstanceRecord& value)
{
  return plus(plus(dynamic(value.instance_name), dynamic(value.cell_name)), scalarVectorBytes(value.ordered_pins));
}

Bytes dynamic(const ArcRecord& value)
{
  return dynamic(value.key);
}

Bytes dynamic(const ClockConstraintModel& value)
{
  return plus(dynamic(value.map_key), scalarVectorBytes(value.ordered_sources));
}

Bytes dynamic(const PortConstraintModel& value)
{
  return dynamic(value.clock_name);
}

Bytes dynamic(const StartSeedModel& value)
{
  return dynamic(value.clock_tag);
}

Bytes dynamic(const EndpointRequiredModel& value)
{
  return dynamic(value.clock_name);
}

template <typename T>
Bytes none(const T&)
{
  return {};
}

CsrBufferExtent extent(std::size_t size, std::size_t capacity)
{
  return {asU64(size), asU64(capacity)};
}

Bytes rootStringBytes(const CsrReferenceInput& input)
{
  Bytes result = dynamic(input.schema_version);
  result = plus(result, dynamic(input.source_identity.fingerprint_schema_version));
  result = plus(result, dynamic(input.source_identity.topology_fingerprint));
  result = plus(result, dynamic(input.source_identity.clock_constraint_fingerprint));
  result = plus(result, dynamic(input.source_identity.library_fingerprint));
  result = plus(result, dynamic(input.source_identity.corner_fingerprint));
  result = plus(result, dynamic(input.source_identity.parasitic_fingerprint));
  result = plus(result, dynamic(input.source_identity.timing_input_fingerprint));
  result = plus(result, dynamic(input.analysis_context.liberty_corner));
  result = plus(result, dynamic(input.analysis_context.temperature));
  return plus(result, dynamic(input.analysis_context.rc_corner));
}

Bytes inputVectorBytes(const CsrReferenceInput& input)
{
  Bytes result = vectorBytes(input.nodes, [](const auto& item) { return dynamic(item); });
  result = plus(result, vectorBytes(input.node_ids_by_key, [](const auto& item) { return dynamic(item); }));
  result = plus(result, vectorBytes(input.instances, [](const auto& item) { return dynamic(item); }));
  result = plus(result, vectorBytes(input.arcs, [](const auto& item) { return dynamic(item); }));
  result = plus(result, vectorBytes(input.arc_ids_by_key, [](const auto& item) { return dynamic(item); }));
  result = plus(result, scalarVectorBytes(input.out_row_offsets));
  result = plus(result, scalarVectorBytes(input.out_arc_ids));
  result = plus(result, scalarVectorBytes(input.in_row_offsets));
  result = plus(result, scalarVectorBytes(input.in_arc_ids));
  result = plus(result, scalarVectorBytes(input.ordered_root_ids));
  result = plus(result, scalarVectorBytes(input.ordered_start_ids));
  result = plus(result, scalarVectorBytes(input.ordered_end_ids));
  result = plus(result, vectorBytes(input.clock_tags, [](const auto& item) { return dynamic(item); }));
  result = plus(result, vectorBytes(input.clocks, [](const auto& item) { return dynamic(item); }));
  result = plus(result, vectorBytes(input.port_constraints, [](const auto& item) { return dynamic(item); }));
  result = plus(result, vectorBytes(input.case_analysis_constraints, [](const auto& item) { return none(item); }));
  result = plus(result, vectorBytes(input.start_seeds, [](const auto& item) { return dynamic(item); }));
  result = plus(result, vectorBytes(input.endpoint_required, [](const auto& item) { return dynamic(item); }));
  result = plus(result, vectorBytes(input.cell_pin_capacitances, [](const auto& item) { return none(item); }));
  return plus(result, vectorBytes(input.unsupported_reasons, [](const auto& item) { return none(item); }));
}

CsrSnapshotObservation unwrapObservation(CsrObservationResult result)
{
  if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&result)) {
    const std::string message = errors->empty() ? "empty observation error" : errors->front().field + ": " + errors->front().message;
    throw std::runtime_error(message);
  }
  return std::get<CsrSnapshotObservation>(std::move(result));
}

CsrSnapshotObservation unwrapOptional(CsrOptionalObservationResult result)
{
  if (const auto* errors = std::get_if<std::vector<CsrContractViolation>>(&result)) {
    const std::string message = errors->empty() ? "empty reducer error" : errors->front().field + ": " + errors->front().message;
    throw std::runtime_error(message);
  }
  auto observation = std::get<CsrOptionalObservation>(std::move(result));
  if (!observation)
    throw std::runtime_error("reducer unexpectedly returned no observation");
  return std::move(*observation);
}

void validateDescriptor(const CompiledContractDescriptor& descriptor)
{
  const std::array columns{descriptor.case_id,
                           descriptor.baseline,
                           descriptor.recipe_op,
                           descriptor.target,
                           descriptor.operand,
                           descriptor.action_token_or_dash,
                           descriptor.expected_validity,
                           descriptor.expected_field,
                           descriptor.expected_category_or_dash,
                           descriptor.expected_message};
  for (std::string_view column : columns) {
    if (column.empty())
      throw std::runtime_error("compiled-contract columns must not be empty");
    for (unsigned char byte : column)
      if (byte < 0x20 || byte > 0x7e)
        throw std::runtime_error("compiled-contract columns must be printable ASCII without TAB/CR/LF");
  }
  if (descriptor.expected_validity != "valid" && descriptor.expected_validity != "invalid")
    throw std::runtime_error("compiled-contract validity must be valid or invalid");
}

}  // namespace

bool listCompiledContractsIfRequested(int argc, char** argv, std::span<const CompiledContractDescriptor> descriptors)
{
  if (argc == 1)
    return false;
  if (argc != 2 || std::string_view(argv[1]) != "--list-case-contracts")
    throw std::invalid_argument("expected no argument or --list-case-contracts");
  writeCompiledContracts(std::cout, descriptors);
  return true;
}

void writeCompiledContracts(std::ostream& output, std::span<const CompiledContractDescriptor> descriptors)
{
  for (const auto& descriptor : descriptors) {
    validateDescriptor(descriptor);
    const std::array columns{descriptor.case_id,
                             descriptor.baseline,
                             descriptor.recipe_op,
                             descriptor.target,
                             descriptor.operand,
                             descriptor.action_token_or_dash,
                             descriptor.expected_validity,
                             descriptor.expected_field,
                             descriptor.expected_category_or_dash,
                             descriptor.expected_message};
    for (std::size_t index = 0; index < columns.size(); ++index)
      output << (index == 0 ? "" : "\t") << columns[index];
    output << '\n';
  }
}

void require(bool condition, std::string_view message)
{
  if (!condition)
    throw std::runtime_error(std::string(message));
}

void requireValid(const std::vector<CsrContractViolation>& errors, std::string_view name)
{
  if (!errors.empty())
    throw std::runtime_error(std::string(name) + ": " + errors.front().field + ": " + errors.front().message);
}

void requireViolation(const std::vector<CsrContractViolation>& errors, std::string_view field, std::string_view message, std::string_view name)
{
  const bool found = std::ranges::any_of(errors, [&](const auto& error) { return error.field == field && error.message == message; });
  require(found, std::string(name) + " did not report the exact compiled-contract violation");
}

void requireExactViolation(const std::vector<CsrContractViolation>& errors, std::string_view field, std::string_view message, std::string_view name)
{
  require(errors.size() == 1, std::string(name) + " must report exactly one violation");
  require(errors.front().field == field && errors.front().message == message, std::string(name) + " did not report the exact sole violation");
}

void requirePrimaryViolation(const std::vector<CsrContractViolation>& errors, std::string_view field, std::string_view message, std::size_t expected_count,
                             std::string_view name)
{
  require(errors.size() == expected_count, std::string(name) + " reported an unstable violation count");
  require(!errors.empty() && errors.front().field == field && errors.front().message == message,
          std::string(name) + " did not report the expected primary violation");
}

void requireCompiledOutcome(const CompiledContractDescriptor& descriptor, const std::vector<CsrContractViolation>& errors)
{
  validateDescriptor(descriptor);
  require(descriptor.expected_category_or_dash == "-", std::string(descriptor.case_id) + " has an unsupported C++ violation category");
  if (descriptor.expected_validity == "valid") {
    require(descriptor.expected_field == "-" && descriptor.expected_message == "-",
            std::string(descriptor.case_id) + " valid outcome must use dash expectations");
    requireValid(errors, descriptor.case_id);
    return;
  }
  require(descriptor.expected_field != "-" && descriptor.expected_message != "-",
          std::string(descriptor.case_id) + " invalid outcome needs an exact field and message");
  requireExactViolation(errors, descriptor.expected_field, descriptor.expected_message, descriptor.case_id);
}

CsrReferenceInput snapshotInput()
{
  return test::referenceInput();
}

CsrReferenceInput emptySnapshotInput()
{
  CsrReferenceInput input;
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  input.out_row_offsets = {0};
  input.in_row_offsets = {0};
  test::refreshIdentity(input);
  return input;
}

CsrReferenceInput tableSnapshotInput()
{
  return test::tableInput();
}

CsrReferenceInput sameLevelSnapshotInput()
{
  auto input = snapshotInput();
  input.arcs.clear();
  input.arc_ids_by_key.clear();
  input.out_row_offsets = {0, 0, 0};
  input.out_arc_ids.clear();
  input.in_row_offsets = {0, 0, 0};
  input.in_arc_ids.clear();
  input.ordered_root_ids = {NodeId{0}, NodeId{1}};
  input.nodes[0].is_endpoint = true;
  input.nodes[1].is_startpoint = true;
  input.ordered_start_ids = {NodeId{0}, NodeId{1}};
  input.ordered_end_ids = {NodeId{0}, NodeId{1}};
  StartSeedModel second_seed;
  second_seed.seed_node = NodeId{1};
  second_seed.path_start_node = NodeId{1};
  second_seed.clock_tag = "clk";
  for (auto& analysis : second_seed.slew)
    analysis.fill(0.0);
  input.start_seeds.push_back(std::move(second_seed));
  test::refreshIdentity(input);
  return input;
}

CsrReferenceInput cycleSnapshotInput()
{
  return test::cycleInput();
}

CsrCaptureMetadata sameLevelSnapshotMetadata()
{
  return {{0, 2}, {NodeId{0}, NodeId{1}}, {1, 1}};
}

CsrCaptureMetadata snapshotMetadata()
{
  return {{0, 1, 2}, {NodeId{0}, NodeId{1}}, {1, 2}};
}

CsrCaptureMetadata emptySnapshotMetadata()
{
  return {{0}, {}, {}};
}

CsrSnapshotObservation readyObservation()
{
  return unwrapObservation(makeReadySnapshotObservation(snapshotInput(), snapshotMetadata(), 0.5));
}

CsrSnapshotObservation emptyReadyObservation()
{
  return unwrapObservation(makeReadySnapshotObservation(emptySnapshotInput(), emptySnapshotMetadata(), 0.5));
}

CsrCurrentFingerprints matchingFingerprints(const CsrSnapshotIdentity& identity)
{
  return {identity.topology_fingerprint, identity.clock_constraint_fingerprint, identity.library_fingerprint,
          identity.corner_fingerprint,   identity.parasitic_fingerprint,        identity.timing_input_fingerprint};
}

CsrCurrentFingerprints topologyDriftFingerprints(const CsrSnapshotIdentity& identity)
{
  auto current = matchingFingerprints(identity);
  current.topology_fingerprint.reset();
  return current;
}

CsrSnapshotObservation rejectedObservation()
{
  auto before = readyObservation();
  return unwrapOptional(reduceSnapshotObservation(before, ScatterRejectionAction{CsrScatterCode::kSchemaMismatch, std::nullopt, 1.0}));
}

CsrSnapshotObservation invalidatedObservation()
{
  auto before = readyObservation();
  return unwrapOptional(reduceSnapshotObservation(before, InvalidationAction{topologyDriftFingerprints(before.identity), 1.0}));
}

CsrSnapshotObservation releasedObservation()
{
  return unwrapOptional(reduceSnapshotObservation(readyObservation(), ReleaseAction{}));
}

CsrSnapshotStorageReport independentSnapshotStorage(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata)
{
  CsrSnapshotStorageReport report;
  report.buffers = {extent(input.nodes.size(), input.nodes.capacity()),
                    extent(input.arcs.size(), input.arcs.capacity()),
                    extent(input.out_row_offsets.size(), input.out_row_offsets.capacity()),
                    extent(input.out_arc_ids.size(), input.out_arc_ids.capacity()),
                    extent(input.in_row_offsets.size(), input.in_row_offsets.capacity()),
                    extent(input.in_arc_ids.size(), input.in_arc_ids.capacity()),
                    extent(metadata.level_offsets.size(), metadata.level_offsets.capacity()),
                    extent(metadata.level_nodes.size(), metadata.level_nodes.capacity()),
                    extent(metadata.node_to_level.size(), metadata.node_to_level.capacity())};
  const std::uint64_t root_bytes = checkedAdd(asU64(sizeof(CsrReferenceInput)), asU64(sizeof(CsrCaptureMetadata)));
  Bytes bytes{root_bytes, root_bytes};
  bytes = plus(bytes, rootStringBytes(input));
  bytes = plus(bytes, inputVectorBytes(input));
  bytes = plus(bytes, scalarVectorBytes(metadata.level_offsets));
  bytes = plus(bytes, scalarVectorBytes(metadata.level_nodes));
  bytes = plus(bytes, scalarVectorBytes(metadata.node_to_level));
  report.host_bytes = {bytes.logical, bytes.capacity};
  return report;
}

}  // namespace ista::csr::snapshot_test
