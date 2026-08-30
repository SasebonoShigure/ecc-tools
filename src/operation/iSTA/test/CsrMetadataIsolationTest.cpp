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
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "CsrCaptureMetadata.hpp"
#include "CsrFingerprint.hpp"
#include "CsrNativeGraphOracle.hpp"

namespace {

using namespace ista::csr;

template <typename T>
concept HasCaptureMetadata = requires(T value) { value.capture_metadata; };

template <typename T>
concept HasNativeOracle = requires(T value) { value.native_oracle; };

template <typename T>
concept HasReferenceInput = requires(T value) { value.input; };

template <typename T>
concept HasGraphSnapshot = requires(T value) { value.graph_snapshot; };

static_assert(!std::is_same_v<CsrReferenceInput, CsrCaptureMetadata>);
static_assert(!std::is_same_v<CsrReferenceInput, CsrNativeGraphOracle>);
static_assert(!std::is_same_v<CsrCaptureMetadata, CsrNativeGraphOracle>);
static_assert(!HasCaptureMetadata<CsrReferenceInput>);
static_assert(!HasNativeOracle<CsrReferenceInput>);
static_assert(!HasReferenceInput<CsrCaptureMetadata>);
static_assert(!HasReferenceInput<CsrNativeGraphOracle>);
static_assert(!HasGraphSnapshot<CsrCaptureMetadata>);
static_assert(!HasGraphSnapshot<CsrNativeGraphOracle>);

void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

CsrReferenceInput inputFixture()
{
  FingerprintPayloads payloads;
  payloads.topology = {1, 2};
  payloads.clock_constraint = {3};
  payloads.library = {4};
  payloads.corner = {5};
  payloads.parasitic = {6};
  payloads.timing_input = {7};
  CsrReferenceInput input;
  input.source_identity = makeSnapshotIdentity(1, 0, payloads);
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  NodeRecord node;
  node.id = NodeId{0};
  node.key = {NodeKind::kPort, "p", {}, {}};
  node.is_port = true;
  input.nodes = {node};
  input.node_ids_by_key = {{input.nodes[0].key, NodeId{0}}};
  input.out_row_offsets = {0, 0};
  input.in_row_offsets = {0, 0};
  input.ordered_root_ids = {NodeId{0}};
  return input;
}

void requireInputUnchanged(const CsrReferenceInput& input, const std::vector<std::uint8_t>& bytes, const CsrSnapshotIdentity& identity, const char* message)
{
  require(canonicalReferenceInputBytes(input) == bytes, message);
  require(input.source_identity == identity, "isolated state changed source identity");
}

}  // namespace

int main()
{
  CsrReferenceInput input = inputFixture();
  CsrCaptureMetadata capture_metadata;
  CsrNativeGraphOracle oracle;
  const auto input_bytes = canonicalReferenceInputBytes(input);
  const auto identity = input.source_identity;

  capture_metadata.level_offsets = {0, 0, 1};
  capture_metadata.level_nodes = {NodeId{0}, NodeId{0}};
  capture_metadata.node_to_level = {99};
  capture_metadata.level_offsets.reserve(64);
  capture_metadata.level_nodes.reserve(64);
  capture_metadata.node_to_level.reserve(64);
  requireInputUnchanged(input, input_bytes, identity, "capture metadata changed reference-input bytes");
  const auto metadata_offsets = capture_metadata.level_offsets;
  const auto metadata_nodes = capture_metadata.level_nodes;
  const auto metadata_levels = capture_metadata.node_to_level;

  oracle.schema = "oracle-only";
  oracle.native_timing_order = {input.nodes[0].key};
  oracle.native_node_levels = {{input.nodes[0].key, 123}};
  oracle.native_timing_order.reserve(64);
  oracle.native_node_levels.reserve(64);
  requireInputUnchanged(input, input_bytes, identity, "native oracle changed reference-input bytes");
  require(
      capture_metadata.level_offsets == metadata_offsets && capture_metadata.level_nodes == metadata_nodes && capture_metadata.node_to_level == metadata_levels,
      "native oracle changed capture metadata");

  CsrReferenceInput changed_input = input;
  changed_input.nodes[0].key.full_name = "q";
  require(canonicalReferenceInputBytes(changed_input) != input_bytes, "input isolation check was vacuous");
  require(capture_metadata.level_offsets == metadata_offsets && capture_metadata.level_nodes == metadata_nodes
              && capture_metadata.node_to_level == metadata_levels && oracle.native_timing_order.front() == input.nodes[0].key,
          "reference-input mutation changed metadata or native oracle");
  return 0;
}
