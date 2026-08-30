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
#pragma once

#include <iosfwd>
#include <span>
#include <string_view>
#include <vector>

#include "CsrSnapshotObservationTransition.hpp"
#include "CsrSnapshotStorage.hpp"

namespace ista::csr::snapshot_test {

struct CompiledContractDescriptor
{
  std::string_view case_id;
  std::string_view baseline;
  std::string_view recipe_op;
  std::string_view target;
  std::string_view operand;
  std::string_view action_token_or_dash;
  std::string_view expected_validity;
  std::string_view expected_field;
  std::string_view expected_category_or_dash;
  std::string_view expected_message;
};

[[nodiscard]] bool listCompiledContractsIfRequested(int argc, char** argv, std::span<const CompiledContractDescriptor> descriptors);
void writeCompiledContracts(std::ostream& output, std::span<const CompiledContractDescriptor> descriptors);

void require(bool condition, std::string_view message);
void requireValid(const std::vector<CsrContractViolation>& errors, std::string_view name);
void requireViolation(const std::vector<CsrContractViolation>& errors, std::string_view field, std::string_view message, std::string_view name);
void requireExactViolation(const std::vector<CsrContractViolation>& errors, std::string_view field, std::string_view message, std::string_view name);
void requirePrimaryViolation(const std::vector<CsrContractViolation>& errors, std::string_view field, std::string_view message, std::size_t expected_count,
                             std::string_view name);
void requireCompiledOutcome(const CompiledContractDescriptor& descriptor, const std::vector<CsrContractViolation>& errors);

[[nodiscard]] CsrReferenceInput snapshotInput();
[[nodiscard]] CsrReferenceInput emptySnapshotInput();
[[nodiscard]] CsrReferenceInput tableSnapshotInput();
[[nodiscard]] CsrReferenceInput sameLevelSnapshotInput();
[[nodiscard]] CsrReferenceInput cycleSnapshotInput();
[[nodiscard]] CsrCaptureMetadata snapshotMetadata();
[[nodiscard]] CsrCaptureMetadata emptySnapshotMetadata();
[[nodiscard]] CsrCaptureMetadata sameLevelSnapshotMetadata();
[[nodiscard]] CsrSnapshotObservation readyObservation();
[[nodiscard]] CsrSnapshotObservation emptyReadyObservation();
[[nodiscard]] CsrSnapshotObservation rejectedObservation();
[[nodiscard]] CsrSnapshotObservation invalidatedObservation();
[[nodiscard]] CsrSnapshotObservation releasedObservation();
[[nodiscard]] CsrCurrentFingerprints matchingFingerprints(const CsrSnapshotIdentity& identity);
[[nodiscard]] CsrCurrentFingerprints topologyDriftFingerprints(const CsrSnapshotIdentity& identity);

// This test-only same-ABI oracle lives in a different translation unit and
// never calls snapshotStorage() or production checked-storage helpers.
[[nodiscard]] CsrSnapshotStorageReport independentSnapshotStorage(const CsrReferenceInput& input, const CsrCaptureMetadata& metadata);

}  // namespace ista::csr::snapshot_test
