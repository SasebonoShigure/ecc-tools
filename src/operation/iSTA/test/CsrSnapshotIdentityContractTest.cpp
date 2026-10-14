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
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "CsrCoreCompiledContractTestSupport.hpp"
#include "CsrValidatorTestFixture.hpp"

namespace {

using namespace ista::csr;
using namespace ista::csr::test;

constexpr std::array<CsrCoreCompiledContract, 8> kCompiledContracts = {
    CsrCoreCompiledContract{"identity_syntax_generation_one", "identity.syntax.gen0", "replace_value", "identity.graph_generation", "1", "-", "valid", "-", "-",
                            "-"},
    CsrCoreCompiledContract{"identity_syntax_generation_max", "identity.syntax.gen0", "replace_value", "identity.graph_generation", "18446744073709551615", "-",
                            "valid", "-", "-", "-"},
    CsrCoreCompiledContract{"identity_syntax_session_max", "identity.syntax.gen0", "replace_value", "identity.session_token", "18446744073709551615", "-",
                            "valid", "-", "-", "-"},
    CsrCoreCompiledContract{"identity_syntax_zero_token", "identity.syntax.gen0", "replace_value", "identity.session_token", "0", "-", "invalid",
                            "identity.session_token", "-", "must be nonzero"},
    CsrCoreCompiledContract{"identity_syntax_bad_version", "identity.syntax.gen0", "replace_value", "identity.fingerprint_schema_version", "1.0.0", "-",
                            "invalid", "identity.fingerprint_schema_version", "-", "unsupported schema version"},
    CsrCoreCompiledContract{"identity_syntax_bad_topology_hash", "identity.syntax.gen0", "replace_value", "identity.topology_fingerprint", "uppercase", "-",
                            "invalid", "identity.topology_fingerprint", "-", "must be 64 lowercase hexadecimal characters"},
    CsrCoreCompiledContract{"identity_committed_generation_one", "identity.committed.gen0", "replace_value", "identity.graph_generation", "1", "-", "invalid",
                            "identity.graph_generation", "-", "Stage 2 committed identity requires generation zero"},
    CsrCoreCompiledContract{"identity_committed_generation_max", "identity.committed.gen0", "replace_value", "identity.graph_generation",
                            "18446744073709551615", "-", "invalid", "identity.graph_generation", "-", "Stage 2 committed identity requires generation zero"},
};

std::uint64_t parseU64(std::string_view text)
{
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    throw std::runtime_error("compiled identity operand is not a canonical u64");
  return value;
}

void applyIdentityRecipe(CsrSnapshotIdentity& value, const CsrCoreCompiledContract& contract)
{
  require(contract.recipe_op == "replace_value", "identity compiled recipe must be replace_value");
  if (contract.target == "identity.graph_generation") {
    value.graph_generation = parseU64(contract.operand);
  } else if (contract.target == "identity.session_token") {
    value.session_token = parseU64(contract.operand);
  } else if (contract.target == "identity.fingerprint_schema_version") {
    value.fingerprint_schema_version = contract.operand;
  } else if (contract.target == "identity.topology_fingerprint" && contract.operand == "uppercase") {
    value.topology_fingerprint.assign(64, 'A');
  } else {
    throw std::runtime_error("compiled identity recipe is not implemented");
  }
}

std::vector<CsrContractViolation> evaluateIdentityCase(const CsrCoreCompiledContract& contract)
{
  CsrSnapshotIdentity value = identity();
  applyIdentityRecipe(value, contract);
  if (contract.baseline.starts_with("identity.syntax."))
    return validateSnapshotIdentitySyntax(value, "identity");
  if (contract.baseline.starts_with("identity.committed."))
    return validateStage2CommittedIdentity(value, "identity");
  throw std::runtime_error("compiled identity baseline is not implemented");
}

void testCompiledContracts()
{
  for (const auto& contract : kCompiledContracts)
    requireCoreCompiledOutcome(contract, evaluateIdentityCase(contract));
}

void testEveryFingerprintField()
{
  using FingerprintMember = std::string CsrSnapshotIdentity::*;
  const std::array<std::pair<std::string_view, FingerprintMember>, 6> fields
      = {{{"topology_fingerprint", &CsrSnapshotIdentity::topology_fingerprint},
          {"clock_constraint_fingerprint", &CsrSnapshotIdentity::clock_constraint_fingerprint},
          {"library_fingerprint", &CsrSnapshotIdentity::library_fingerprint},
          {"corner_fingerprint", &CsrSnapshotIdentity::corner_fingerprint},
          {"parasitic_fingerprint", &CsrSnapshotIdentity::parasitic_fingerprint},
          {"timing_input_fingerprint", &CsrSnapshotIdentity::timing_input_fingerprint}}};
  for (const auto& [field, member] : fields) {
    CsrSnapshotIdentity value = identity();
    value.*member = std::string(64, 'g');
    const auto errors = validateSnapshotIdentitySyntax(value, "identity.");
    require(errors.size() == 1, std::string(field) + " must produce one syntax violation");
    require(errors.front().field == "identity." + std::string(field), std::string(field) + " field path mismatch");
    require(errors.front().message == "must be 64 lowercase hexadecimal characters", std::string(field) + " message mismatch");
  }
}

void testPrefixAndOrdering()
{
  CsrSnapshotIdentity zero_token = identity();
  zero_token.session_token = 0;
  const auto bare = validateSnapshotIdentitySyntax(zero_token, "");
  require(bare.size() == 1 && bare.front().field == "session_token", "empty prefix must emit a bare field");
  const auto trailing = validateSnapshotIdentitySyntax(zero_token, "identity...");
  require(trailing.size() == 1 && trailing.front().field == "identity.session_token", "trailing prefix dots must collapse to one separator");
  const auto only_dots = validateSnapshotIdentitySyntax(zero_token, "...");
  require(only_dots.size() == 1 && only_dots.front().field == "session_token", "dot-only prefix must emit a bare field");

  CsrSnapshotIdentity malformed = identity();
  malformed.fingerprint_schema_version = "1.0.0";
  malformed.session_token = 0;
  malformed.graph_generation = 1;
  malformed.topology_fingerprint.clear();
  malformed.clock_constraint_fingerprint.clear();
  malformed.library_fingerprint.clear();
  malformed.corner_fingerprint.clear();
  malformed.parasitic_fingerprint.clear();
  malformed.timing_input_fingerprint.clear();
  const auto errors = validateStage2CommittedIdentity(malformed, "identity");
  require(errors.size() == 9, "committed validation must report complete syntax plus generation");
  require(errors.front().field == "identity.fingerprint_schema_version", "syntax version must be reported first");
  require(errors.back().field == "identity.graph_generation", "committed generation must be appended after complete syntax");
}

void testCommittedValidatorConsumers()
{
  CsrReferenceInput input = referenceInput();
  input.source_identity.graph_generation = 1;
  requireViolation(validateReferenceInput(input), "source_identity.graph_generation", "requires generation zero", "reference input committed identity");

  input = referenceInput();
  CsrTimingResult result = timingResult(input);
  result.source_identity.graph_generation = 1;
  requireViolation(validateTimingResult(input, result), "result.source_identity.graph_generation", "requires generation zero",
                   "timing result committed identity");
}

void testU64Boundaries()
{
  CsrSnapshotIdentity value = identity();
  value.session_token = 1;
  value.graph_generation = 0;
  requireValid(validateSnapshotIdentitySyntax(value, "identity"), "syntax token one generation zero");
  requireValid(validateStage2CommittedIdentity(value, "identity"), "committed token one generation zero");

  value.session_token = std::numeric_limits<std::uint64_t>::max();
  value.graph_generation = std::numeric_limits<std::uint64_t>::max();
  requireValid(validateSnapshotIdentitySyntax(value, "identity"), "syntax u64 maxima");
  requireInvalid(validateStage2CommittedIdentity(value, "identity"), "committed generation maximum");
}

void testFirstInvalidationReason()
{
  const CsrSnapshotIdentity committed = identity();
  const CsrCurrentFingerprints current{.topology_fingerprint = committed.topology_fingerprint,
                                       .clock_constraint_fingerprint = committed.clock_constraint_fingerprint,
                                       .library_fingerprint = committed.library_fingerprint,
                                       .corner_fingerprint = committed.corner_fingerprint,
                                       .parasitic_fingerprint = committed.parasitic_fingerprint,
                                       .timing_input_fingerprint = committed.timing_input_fingerprint};
  require(!firstCsrInvalidationReason(committed, current), "matching fingerprints must not invalidate");

  using FingerprintMember = std::optional<std::string> CsrCurrentFingerprints::*;
  const std::array<std::pair<FingerprintMember, CsrInvalidationReason>, 6> cases
      = {{{&CsrCurrentFingerprints::topology_fingerprint, CsrInvalidationReason::kTopologyChanged},
          {&CsrCurrentFingerprints::clock_constraint_fingerprint, CsrInvalidationReason::kClockConstraintChanged},
          {&CsrCurrentFingerprints::library_fingerprint, CsrInvalidationReason::kLibraryChanged},
          {&CsrCurrentFingerprints::corner_fingerprint, CsrInvalidationReason::kCornerChanged},
          {&CsrCurrentFingerprints::parasitic_fingerprint, CsrInvalidationReason::kParasiticChanged},
          {&CsrCurrentFingerprints::timing_input_fingerprint, CsrInvalidationReason::kTimingInputChanged}}};
  for (const auto& [member, reason] : cases) {
    CsrCurrentFingerprints missing = current;
    missing.*member = std::nullopt;
    require(firstCsrInvalidationReason(committed, missing) == reason, "missing fingerprint invalidation reason mismatch");
    CsrCurrentFingerprints mismatched = current;
    mismatched.*member = "mismatch";
    require(firstCsrInvalidationReason(committed, mismatched) == reason, "changed fingerprint invalidation reason mismatch");
  }

  CsrCurrentFingerprints priority = current;
  priority.topology_fingerprint = std::nullopt;
  priority.timing_input_fingerprint = std::nullopt;
  require(firstCsrInvalidationReason(committed, priority) == CsrInvalidationReason::kTopologyChanged, "invalidation reason must follow frozen family priority");
}

}  // namespace

int main(int argc, char* argv[])
{
  if (handleCoreCompiledContractCli(argc, argv, kCompiledContracts))
    return 0;
  testCompiledContracts();
  testEveryFingerprintField();
  testPrefixAndOrdering();
  testCommittedValidatorConsumers();
  testU64Boundaries();
  testFirstInvalidationReason();
  return 0;
}
