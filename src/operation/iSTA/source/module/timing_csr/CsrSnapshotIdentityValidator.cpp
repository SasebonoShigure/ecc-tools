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
#include <string>
#include <string_view>
#include <utility>

#include "CsrFingerprint.hpp"
#include "CsrReferenceValidator.hpp"
#include "CsrTimingResultValidatorInternal.hpp"

namespace ista::csr {
namespace {

std::string identityField(std::string_view prefix, std::string_view field)
{
  while (!prefix.empty() && prefix.back() == '.')
    prefix.remove_suffix(1);
  if (prefix.empty())
    return std::string(field);
  std::string qualified;
  qualified.reserve(prefix.size() + 1 + field.size());
  qualified.append(prefix);
  qualified.push_back('.');
  qualified.append(field);
  return qualified;
}

std::vector<CsrContractViolation> projectViolations(detail::TimingResultViolations classified)
{
  std::vector<CsrContractViolation> errors;
  errors.reserve(classified.size());
  for (auto& error : classified)
    errors.push_back({std::move(error.field), std::move(error.message)});
  return errors;
}

}  // namespace

detail::TimingResultViolations detail::validateSnapshotIdentityClassified(const CsrSnapshotIdentity& identity, std::string_view prefix,
                                                                          bool require_stage2_generation)
{
  TimingResultViolations errors;
  if (identity.fingerprint_schema_version != kCsrFingerprintSchemaVersion)
    reject(errors, CsrTimingResultViolationClass::kSchema, identityField(prefix, "fingerprint_schema_version"), "unsupported schema version");
  if (identity.session_token == 0)
    reject(errors, CsrTimingResultViolationClass::kSchema, identityField(prefix, "session_token"), "must be nonzero");

  const std::array<std::pair<std::string_view, const std::string*>, 6> fingerprints
      = {{{"topology_fingerprint", &identity.topology_fingerprint},
          {"clock_constraint_fingerprint", &identity.clock_constraint_fingerprint},
          {"library_fingerprint", &identity.library_fingerprint},
          {"corner_fingerprint", &identity.corner_fingerprint},
          {"parasitic_fingerprint", &identity.parasitic_fingerprint},
          {"timing_input_fingerprint", &identity.timing_input_fingerprint}}};
  for (const auto& [name, value] : fingerprints)
    if (!isLowercaseSha256(*value))
      reject(errors, CsrTimingResultViolationClass::kSchema, identityField(prefix, name), "must be 64 lowercase hexadecimal characters");
  if (require_stage2_generation && identity.graph_generation != 0)
    reject(errors, CsrTimingResultViolationClass::kGeneration, identityField(prefix, "graph_generation"),
           "Stage 2 committed identity requires generation zero");
  return errors;
}

std::vector<CsrContractViolation> validateSnapshotIdentitySyntax(const CsrSnapshotIdentity& identity, std::string_view prefix)
{
  return projectViolations(detail::validateSnapshotIdentityClassified(identity, prefix, false));
}

std::vector<CsrContractViolation> validateStage2CommittedIdentity(const CsrSnapshotIdentity& identity, std::string_view prefix)
{
  return projectViolations(detail::validateSnapshotIdentityClassified(identity, prefix, true));
}

}  // namespace ista::csr
