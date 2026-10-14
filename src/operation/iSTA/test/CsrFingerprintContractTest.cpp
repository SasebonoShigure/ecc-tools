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
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include "CsrFingerprint.hpp"
#include "TimingSemanticKey.hpp"

namespace {

using namespace ista::csr;

template <typename T>
concept HasPersistedStorage = requires(T value) { value.storage; };

static_assert(!HasPersistedStorage<CsrReferenceInput>);

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

template <typename Callable>
void requireInvalid(Callable callable, const std::string& message)
{
  try {
    callable();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(message);
}

std::string bytesToHex(const std::vector<std::uint8_t>& bytes)
{
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (std::uint8_t byte : bytes) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

NodeKey port(std::string name)
{
  return NodeKey{NodeKind::kPort, std::move(name), {}, {}};
}

NodeKey instancePin(std::string full_name, std::string instance_name, std::string pin_name)
{
  return NodeKey{NodeKind::kInstancePin, std::move(full_name), std::move(instance_name), std::move(pin_name)};
}

LibraryArcSignature librarySignature()
{
  LibraryArcSignature signature;
  signature.library_name = "slow.lib";
  signature.arc_index = -1;
  signature.sense = TimingSense::kNoneUsesPositiveFallback;
  signature.trigger_transition = Transition::kRise;
  signature.check_transition = Transition::kNone;
  signature.delay_tables[0] = {true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance};
  signature.slew_tables[1] = {true, TableVariable::kConstrainedTransition, TableVariable::kNone};
  return signature;
}

CsrSnapshotIdentity identity()
{
  FingerprintPayloads payloads;
  payloads.topology = {1};
  payloads.clock_constraint = {2};
  payloads.library = {3};
  payloads.corner = {4};
  payloads.parasitic = {5};
  payloads.timing_input = {6};
  return makeSnapshotIdentity(7, 0, payloads);
}

CsrReferenceInput validInput()
{
  CsrReferenceInput input;
  input.source_identity = identity();
  input.analysis_context = {"slow", "25", "__no_rc_corner__"};
  input.nodes = {
      NodeRecord{
          .id = NodeId{0}, .key = port("in"), .direction = PinDirection::kInput, .instance_cell = {}, .net_name = {}, .is_port = true, .is_startpoint = true},
      NodeRecord{
          .id = NodeId{1}, .key = port("out"), .direction = PinDirection::kOutput, .instance_cell = {}, .net_name = {}, .is_port = true, .is_endpoint = true}};
  input.node_ids_by_key = {{input.nodes[0].key, NodeId{0}}, {input.nodes[1].key, NodeId{1}}};
  ArcKeyBase base{.source = input.nodes[0].key,
                  .sink = input.nodes[1].key,
                  .kind = ArcKind::kNet,
                  .owner_name = "n1",
                  .library_source_port = {},
                  .library_sink_port = {},
                  .library_arcs = {}};
  input.arcs = {ArcRecord{.id = ArcId{0},
                          .key = ArcKey{base, 0},
                          .source = NodeId{0},
                          .sink = NodeId{1},
                          .kind = ArcKind::kNet,
                          .eligibility = {true, true, true, false},
                          .transfer = {}, .output_load = {}}};
  input.arc_ids_by_key = {{input.arcs[0].key, ArcId{0}}};
  input.out_row_offsets = {0, 1, 1};
  input.out_arc_ids = {ArcId{0}};
  input.in_row_offsets = {0, 0, 1};
  input.in_arc_ids = {ArcId{0}};
  input.ordered_root_ids = {NodeId{0}};
  input.ordered_start_ids = {NodeId{0}};
  input.ordered_end_ids = {NodeId{1}};
  return input;
}

void testSha256AndPrimitiveEncoding()
{
  require(sha256Hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA-256");
  const std::vector<std::uint8_t> abc = {'a', 'b', 'c'};
  require(sha256Hex(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc SHA-256");
  const std::vector<std::uint8_t> million_a(1'000'000, 'a');
  require(sha256Hex(million_a) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "million-a SHA-256");
  const std::vector<std::uint8_t> fifty_five_a(55, 'a');
  require(sha256Hex(fifty_five_a) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", "55-byte SHA-256");
  const std::vector<std::uint8_t> fifty_six_a(56, 'a');
  require(sha256Hex(fifty_six_a) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", "56-byte SHA-256");

  for (const std::size_t size : {0U, 3U, 55U, 56U, 63U, 64U, 65535U, 65536U, 65537U, 1000000U}) {
    std::string payload(size, '\0');
    for (std::size_t index = 0; index < size; ++index)
      payload[index] = static_cast<char>(index % 251);
    std::istringstream stream(payload);
    require(sha256Hex(stream, size)
                == sha256Hex(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size())),
            "stream SHA-256 disagrees at block/padding boundary");
  }
  for (const auto size : {2U, 4U}) {
    std::istringstream stream("abc");
    bool rejected = false;
    try {
      static_cast<void>(sha256Hex(stream, size));
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    require(rejected, "stream SHA-256 accepted an incorrect declared size");
  }
  std::istringstream failed_stream;
  failed_stream.setstate(std::ios::badbit);
  bool read_failed = false;
  try {
    static_cast<void>(sha256Hex(failed_stream, 0));
  } catch (const std::runtime_error&) {
    read_failed = true;
  }
  require(read_failed, "stream SHA-256 accepted failed input");

  CanonicalEncoder signed_values;
  signed_values.appendI32(0);
  signed_values.appendI32(-1);
  require(bytesToHex(signed_values.bytes()) == "00000000ffffffff", "appendI32 big-endian/two's-complement");

  CanonicalEncoder encoder;
  encoder.appendDomain("known");
  encoder.appendU32(0x01020304U);
  encoder.appendU64(0x0102030405060708ULL);
  encoder.appendString("A");
  encoder.appendDouble(-0.0);
  encoder.appendDouble(std::numeric_limits<double>::infinity());
  encoder.appendDouble(-std::numeric_limits<double>::infinity());
  const std::string expected
      = "0000000000000008697374612d63737200000000000000056b6e6f776e0000000000000005312e302e30"
        "0102030401020304050607080000000000000001410000000000000000000102";
  require(bytesToHex(encoder.bytes()) == expected, "primitive canonical known answer");
  require(sha256Hex(encoder.bytes()) == "4b2cc69064c22df6a211dc1d719849bab90ce6899e1b0375989622b81072ebe4", "primitive canonical hash");

  CanonicalEncoder positive_zero;
  positive_zero.appendDouble(0.0);
  CanonicalEncoder negative_zero;
  negative_zero.appendDouble(-0.0);
  require(positive_zero.bytes() == negative_zero.bytes(), "negative zero canonicalization");
  requireInvalid(
      [] {
        CanonicalEncoder invalid;
        invalid.appendDouble(std::numeric_limits<double>::quiet_NaN());
      },
      "NaN must be rejected");
}

void testFingerprintPayloadBoundaries()
{
  for (std::size_t domain_size = 0; domain_size < 128; ++domain_size) {
    const std::string domain(domain_size, 'd');
    for (const std::size_t size : {0U, 1U, 55U, 56U, 63U, 64U, 65U, 127U, 128U, 129U, 1024U}) {
      std::vector<std::uint8_t> payload(size);
      for (std::size_t index = 0; index < size; ++index)
        payload[index] = static_cast<std::uint8_t>(index % 251);
      CanonicalEncoder legacy;
      legacy.appendDomain(domain);
      legacy.appendBytes(payload);
      require(componentFingerprint(domain, payload) == sha256Hex(legacy.bytes()), "component fingerprint changed at a split boundary");

      if (domain_size == 0) {
        FingerprintPayloads payloads;
        payloads.timing_input = payload;
        const auto identity = makeSnapshotIdentity(1, 0, payloads);
        CanonicalEncoder composite;
        composite.appendDomain("timing-input-composite");
        composite.appendString(identity.topology_fingerprint);
        composite.appendString(identity.clock_constraint_fingerprint);
        composite.appendString(identity.library_fingerprint);
        composite.appendString(identity.corner_fingerprint);
        composite.appendString(identity.parasitic_fingerprint);
        composite.appendBytes(payload);
        require(identity.timing_input_fingerprint == sha256Hex(composite.bytes()), "timing composite encoding changed");
      }
    }
  }
}


void testDirectKeyEncoding()
{
  CanonicalEncoder direct;
  CanonicalEncoder legacy;
  direct.appendDomain("key-sequence");
  legacy.appendDomain("key-sequence");
  for (const auto size : {0U, 1U, 55U, 64U, 257U, 4096U}) {
    std::string name(size, 'x');
    if (size != 0)
      name[size / 2] = '\0';
    for (const auto kind : {NodeKind::kPort, NodeKind::kInstancePin}) {
      const NodeKey node{kind, name, "\xe6\x85\xa2", "Y"};
      direct.appendNodeKeyBytes(node);
      legacy.appendBytes(canonicalNodeKeyBytes(node));
      require(direct.bytes() == legacy.bytes(), "direct node-key encoding changed bytes");
      for (const auto arc_kind : {ArcKind::kNet, ArcKind::kCell}) {
        ArcKey arc;
        arc.base.source = node;
        arc.base.sink = instancePin("sink:A", "sink", "A");
        arc.base.kind = arc_kind;
        arc.base.owner_name = name;
        arc.base.library_source_port = "Y";
        arc.base.library_sink_port = "A";
        arc.parallel_ordinal = std::numeric_limits<std::uint64_t>::max();
        if (arc_kind == ArcKind::kCell)
          arc.base.library_arcs = {librarySignature(), librarySignature()};
        direct.appendArcKeyBytes(arc);
        legacy.appendBytes(canonicalArcKeyBytes(arc));
        require(direct.bytes() == legacy.bytes(), "direct arc-key encoding changed bytes");
      }
    }
  }
  NodeKey bad_node = port("bad");
  bad_node.kind = static_cast<NodeKind>(0xff);
  requireInvalid([&] { direct.appendNodeKeyBytes(bad_node); }, "direct invalid node kind");
  ArcKey bad_arc;
  bad_arc.base.source = port("in");
  bad_arc.base.sink = port("out");
  bad_arc.base.kind = ArcKind::kCell;
  bad_arc.base.library_arcs = {librarySignature()};
  bad_arc.base.library_arcs[0].sense = static_cast<TimingSense>(0xff);
  requireInvalid([&] { direct.appendArcKeyBytes(bad_arc); }, "direct invalid nested arc sense");
}

void testAnalysisContextKnownAnswers()
{
  const StaAnalysisContext ascii_context{"slow", "25", "__no_rc_corner__"};
  const auto ascii_payload = canonicalAnalysisContextBytes(ascii_context);
  require(sha256Hex(ascii_payload) == "9d72757619bf1225c9888aa02b1f9917c8ea54a475754a8aae5939a7a96a1b83", "ASCII analysis-context payload SHA-256");
  FingerprintPayloads ascii_payloads;
  ascii_payloads.corner = ascii_payload;
  require(makeSnapshotIdentity(7, 0, ascii_payloads).corner_fingerprint == "095aeaf315c1c903fb0d166788cf05aa90af0055e41a25ae86e3458ce5f62a2a",
          "ASCII corner component fingerprint");

  const std::string utf8_corner{"\xe6\x85\xa2"};
  require(utf8_corner.size() == 3, "UTF-8 corner must contain exactly three bytes");
  const StaAnalysisContext utf8_context{utf8_corner, "25", "__no_rc_corner__"};
  const auto utf8_payload = canonicalAnalysisContextBytes(utf8_context);
  require(bytesToHex(utf8_payload).find("0000000000000003e685a2") != std::string::npos, "UTF-8 analysis-context string needs a three-byte length prefix");
  require(sha256Hex(utf8_payload) == "06402d67b192a27d7caef43a34054c58938df1e20fd5b9aefaffc5bc8486b8dd", "UTF-8 analysis-context payload SHA-256");
  FingerprintPayloads utf8_payloads;
  utf8_payloads.corner = utf8_payload;
  require(makeSnapshotIdentity(7, 0, utf8_payloads).corner_fingerprint == "e3bafb5e252ccc546ba306fd33f30122773840f1bc37a616752ebb96da26eec2",
          "UTF-8 corner component fingerprint");
}

std::array<std::string, 5> componentFingerprints(const CsrSnapshotIdentity& identity)
{
  return {identity.topology_fingerprint, identity.clock_constraint_fingerprint, identity.library_fingerprint, identity.corner_fingerprint,
          identity.parasitic_fingerprint};
}

void testSnapshotIdentityDomains()
{
  const FingerprintPayloads empty_payloads;
  const CsrSnapshotIdentity empty_identity = makeSnapshotIdentity(1, 0, empty_payloads);
  const auto empty_components = componentFingerprints(empty_identity);
  for (std::size_t lhs = 0; lhs < empty_components.size(); ++lhs)
    for (std::size_t rhs = lhs + 1; rhs < empty_components.size(); ++rhs)
      require(empty_components[lhs] != empty_components[rhs], "empty component payloads need distinct domains");

  using PayloadMember = std::vector<std::uint8_t> FingerprintPayloads::*;
  const std::array<PayloadMember, 5> component_payloads = {&FingerprintPayloads::topology, &FingerprintPayloads::clock_constraint,
                                                           &FingerprintPayloads::library, &FingerprintPayloads::corner, &FingerprintPayloads::parasitic};
  for (std::size_t changed_index = 0; changed_index < component_payloads.size(); ++changed_index) {
    FingerprintPayloads changed_payloads;
    changed_payloads.*component_payloads[changed_index] = {0x5a};
    const CsrSnapshotIdentity changed = makeSnapshotIdentity(1, 0, changed_payloads);
    const auto changed_components = componentFingerprints(changed);
    for (std::size_t component = 0; component < changed_components.size(); ++component)
      require((changed_components[component] != empty_components[component]) == (component == changed_index),
              "component payload changed an unrelated fingerprint");
    require(changed.timing_input_fingerprint != empty_identity.timing_input_fingerprint, "component change did not reach timing-input composite");
  }

  FingerprintPayloads timing_payload;
  timing_payload.timing_input = {0x5a};
  const CsrSnapshotIdentity changed_timing = makeSnapshotIdentity(1, 0, timing_payload);
  require(componentFingerprints(changed_timing) == empty_components, "timing payload changed a component fingerprint");
  require(changed_timing.timing_input_fingerprint != empty_identity.timing_input_fingerprint, "timing payload did not change timing-input composite");
}

void testNestedBadEnums()
{
  ArcKey bad_sense;
  bad_sense.base = {.source = instancePin("u1:A", "u1", "A"),
                    .sink = instancePin("u1:Y", "u1", "Y"),
                    .kind = ArcKind::kCell,
                    .owner_name = "u1",
                    .library_source_port = "A",
                    .library_sink_port = "Y",
                    .library_arcs = {librarySignature()}};
  bad_sense.base.library_arcs[0].sense = static_cast<TimingSense>(0xff);
  require(validateArcKeyBase(bad_sense.base).has_value(), "nested timing sense validation");
  requireInvalid([&] { static_cast<void>(canonicalArcKeyBytes(bad_sense)); }, "nested timing sense encoding");

  ArcKey bad_table = bad_sense;
  bad_table.base.library_arcs[0].sense = TimingSense::kNoneUsesPositiveFallback;
  bad_table.base.library_arcs[0].delay_tables[0].variable2 = static_cast<TableVariable>(0xff);
  require(validateArcKeyBase(bad_table.base).has_value(), "nested table variable validation");
  requireInvalid([&] { static_cast<void>(canonicalArcKeyBytes(bad_table)); }, "nested table variable encoding");

  CsrReferenceInput bad_input = validInput();
  ChildTimingArcModel child;
  child.signature = librarySignature();
  child.signature.check_transition = static_cast<Transition>(0xff);
  bad_input.arcs[0].transfer.setChildren({child});
  requireInvalid([&] { static_cast<void>(canonicalReferenceInputBytes(bad_input)); }, "nested reference-input enum");

  CsrReferenceInput bad_seed_input = validInput();
  StartSeedModel bad_seed;
  bad_seed.seed_node = NodeId{0};
  bad_seed.path_start_node = NodeId{0};
  bad_seed.path_source = static_cast<PathSource>(0xff);
  bad_seed_input.start_seeds = {bad_seed};
  requireInvalid([&] { static_cast<void>(canonicalReferenceInputBytes(bad_seed_input)); }, "optional path-source enum");

  bad_seed_input = validInput();
  bad_seed.path_source = PathSource::kInput;
  bad_seed.availability = static_cast<StartSeedAvailability>(0xff);
  bad_seed_input.start_seeds = {bad_seed};
  requireInvalid([&] { static_cast<void>(canonicalReferenceInputBytes(bad_seed_input)); }, "start-seed availability enum");

  CsrReferenceInput bad_check_input = validInput();
  TimingCheckEndpointModel bad_check;
  bad_check.check_kind = static_cast<TimingCheckKind>(0xff);
  bad_check_input.timing_check_endpoints = {bad_check};
  requireInvalid([&] { static_cast<void>(canonicalReferenceInputBytes(bad_check_input)); }, "timing-check kind enum");
}
}  // namespace

int main()
{
  testSha256AndPrimitiveEncoding();
  testFingerprintPayloadBoundaries();
  testDirectKeyEncoding();
  testAnalysisContextKnownAnswers();
  testSnapshotIdentityDomains();
  testNestedBadEnums();
  return 0;
}
