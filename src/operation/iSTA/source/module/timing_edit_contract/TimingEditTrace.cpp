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
#include "TimingEditTrace.hpp"

#include <algorithm>
#include <set>
#include <limits>
#include <stdexcept>

#include "CsrFingerprint.hpp"
#include "CsrReferenceCanonicalEncoder.hpp"
#include "TimingEditByteOrder.hpp"
#include "TimingEditCanonicalEncoding.hpp"

namespace ista::timing_edit {
namespace {

using csr::CanonicalEncoder;
using Violations = TimingEditContractViolations;

void reject(Violations& errors, TimingEditContractCode code, std::string field, std::string message)
{
  errors.push_back({code, std::move(field), std::move(message)});
}

void append(Violations& target, Violations source)
{
  target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

bool validText(std::string_view value)
{
  return !value.empty() && value.find('\0') == std::string_view::npos;
}

std::vector<std::uint8_t> artifactBytes(const SourceArtifactIdentity& value)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("timing-edit-source-artifact");
  encoder.appendString(value.artifact_role);
  encoder.appendString(value.logical_name);
  encoder.appendString(value.content_sha256);
  return encoder.bytes();
}

void appendPolicy(CanonicalEncoder& encoder, const PolicyReceipt& value)
{
  encoder.appendString(token(value.disposition));
  encoder.appendU64(value.reasons.size());
  for (const auto& reason : value.reasons)
    encoder.appendString(reason);
}

bool receiptMatches(const TimingEditTraceBatch& batch)
{
  bool requires_full = false;
  for (const auto& edit : batch.edits)
    if (const auto* rc = std::get_if<ParasiticRcInputs>(&edit.rc_inputs))
      requires_full = requires_full || rc->parasitic_policy == ParasiticEditPolicy::kFullWithRecord;
  if (requires_full != (batch.policy_receipt.disposition == EditDisposition::kFullWithRecord))
    return false;
  if (requires_full)
    return !batch.policy_receipt.reasons.empty();
  return batch.policy_receipt.reasons.empty();
}

}  // namespace

std::string_view token(TraceWorkloadKind value)
{
  switch (value) {
    case TraceWorkloadKind::kSynthetic:
      return "synthetic";
    case TraceWorkloadKind::kRealDesign:
      return "real_design";
  }
  throw std::invalid_argument("unknown TraceWorkloadKind");
}

TimingEditContractViolations validateTimingEditTrace(const TimingEditTrace& value)
{
  Violations errors;
  if (value.schema_version != kTimingEditTraceSchema)
    reject(errors, TimingEditContractCode::kSchemaMismatch, "trace.schema_version", "expected timing-edit-trace/1.0.0");
  try {
    static_cast<void>(token(value.workload_kind));
  } catch (const std::invalid_argument& error) {
    reject(errors, TimingEditContractCode::kInvalidValue, "trace.workload_kind", error.what());
  }
  if (!validText(value.profile))
    reject(errors, TimingEditContractCode::kInvalidValue, "trace.profile", "must be nonempty and contain no NUL");
  append(errors, validateBaselineSemanticIdentity(value.baseline, "trace.baseline"));

  std::set<std::pair<std::string, std::string>> artifact_keys;
  for (std::size_t index = 0; index < value.source_artifacts.size(); ++index) {
    const auto& artifact = value.source_artifacts[index];
    const std::string field = "trace.source_artifacts[" + std::to_string(index) + "]";
    if (!validText(artifact.artifact_role) || !validText(artifact.logical_name))
      reject(errors, TimingEditContractCode::kInvalidValue, field, "role and logical name must be nonempty and contain no NUL");
    if (!csr::isLowercaseSha256(artifact.content_sha256))
      reject(errors, TimingEditContractCode::kInvalidValue, field + ".content_sha256", "expected lowercase SHA-256 integrity value");
    if (!artifact_keys.emplace(artifact.artifact_role, artifact.logical_name).second)
      reject(errors, TimingEditContractCode::kDuplicate, field, "duplicate source artifact role/name");
  }
  if (value.workload_kind == TraceWorkloadKind::kRealDesign && value.source_artifacts.empty())
    reject(errors, TimingEditContractCode::kInvalidValue, "trace.source_artifacts", "real design trace requires source artifacts");
  if (value.batches.empty())
    reject(errors, TimingEditContractCode::kInvalidValue, "trace.batches", "at least one batch is required");

  std::optional<CommittedEditIdentity> prior_post;
  for (std::size_t index = 0; index < value.batches.size(); ++index) {
    const auto& batch = value.batches[index];
    const std::string field = "trace.batches[" + std::to_string(index) + "]";
    if (batch.ordinal != index + 1)
      reject(errors, TimingEditContractCode::kNotCanonical, field + ".ordinal", "batch ordinals must be dense, one-based and ordered");
    append(errors, validateCommittedEditIdentity(batch.expected_pre_identity, field + ".expected_pre_identity"));
    append(errors, validateCommittedEditIdentity(batch.expected_post_identity, field + ".expected_post_identity"));
    if (batch.expected_pre_identity.generation.value() == std::numeric_limits<std::uint64_t>::max()
        || batch.expected_post_identity.generation.value() != batch.expected_pre_identity.generation.value() + 1)
      reject(errors, TimingEditContractCode::kGenerationMismatch, field + ".expected_post_identity.generation",
             "post generation must equal pre generation plus one");
    if (index == 0) {
      CommittedEditIdentity baseline{TransactionGeneration{0}, value.baseline.fingerprints, value.baseline.canonical_pre_state};
      if (batch.expected_pre_identity != baseline)
        reject(errors, TimingEditContractCode::kIdentityMismatch, field + ".expected_pre_identity", "first pre identity must equal generation-zero baseline");
    } else if (prior_post != batch.expected_pre_identity) {
      reject(errors, TimingEditContractCode::kIdentityMismatch, field + ".expected_pre_identity", "batch identity chain is discontinuous");
    }
    prior_post = batch.expected_post_identity;

    EditBatch plan{batch.expected_pre_identity.generation, batch.edits};
    auto normalized = normalizeEditBatch(plan);
    if (const auto* plan_errors = std::get_if<Violations>(&normalized)) {
      for (auto error : *plan_errors) {
        error.field = field + "." + error.field;
        errors.push_back(std::move(error));
      }
    } else if (std::get<EditBatch>(normalized) != plan) {
      reject(errors, TimingEditContractCode::kNotCanonical, field + ".edits", "edits and moved sinks must be canonical ordered");
    }
    for (std::size_t edit_index = 0; edit_index < batch.edits.size(); ++edit_index) {
      const auto& token_value = batch.edits[edit_index].creation_token;
      const std::string token_field = field + ".edits[" + std::to_string(edit_index) + "].creation_token";
      if (token_value.seed != value.seed || token_value.profile_id != value.profile || token_value.batch_ordinal != batch.ordinal)
        reject(errors, TimingEditContractCode::kIdentityMismatch, token_field, "token seed/profile/batch must match trace envelope");
    }
    if (!receiptMatches(batch))
      reject(errors, TimingEditContractCode::kConflict, field + ".policy_receipt", "receipt does not match batch RC fallback semantics");
    if (!std::is_sorted(batch.policy_receipt.reasons.begin(), batch.policy_receipt.reasons.end())
        || std::adjacent_find(batch.policy_receipt.reasons.begin(), batch.policy_receipt.reasons.end()) != batch.policy_receipt.reasons.end())
      reject(errors, TimingEditContractCode::kNotCanonical, field + ".policy_receipt.reasons", "reasons must be sorted and duplicate-free");
  }

  if (!validText(value.replay_command.executable))
    reject(errors, TimingEditContractCode::kInvalidValue, "trace.replay_command.executable", "must be nonempty and contain no NUL");
  bool trace_flag = false;
  bool prefix_flag = false;
  for (const auto& argument : value.replay_command.arguments) {
    if (!validText(argument))
      reject(errors, TimingEditContractCode::kInvalidValue, "trace.replay_command.arguments", "arguments must be nonempty and contain no NUL");
    trace_flag = trace_flag || argument == "--trace";
    prefix_flag = prefix_flag || argument == "--prefix";
  }
  if (!trace_flag || !prefix_flag)
    reject(errors, TimingEditContractCode::kInvalidValue, "trace.replay_command.arguments", "must contain --trace and --prefix for one-command prefix replay");
  return errors;
}

std::vector<std::uint8_t> canonicalTimingEditTraceBytes(const TimingEditTrace& value)
{
  CanonicalEncoder encoder;
  encoder.appendDomain("timing-edit-trace");
  encoder.appendString(value.schema_version);
  encoder.appendString(token(value.workload_kind));
  encoder.appendU64(value.seed);
  encoder.appendString(value.profile);
  encoder.appendBytes(canonicalBaselineSemanticIdentityBytes(value.baseline));
  std::vector<std::vector<std::uint8_t>> artifacts;
  for (const auto& artifact : value.source_artifacts)
    artifacts.push_back(artifactBytes(artifact));
  std::sort(artifacts.begin(), artifacts.end(), detail::CanonicalBytesLess{});
  encoder.appendU64(artifacts.size());
  for (const auto& artifact : artifacts)
    encoder.appendBytes(artifact);
  encoder.appendU64(value.batches.size());
  for (const auto& batch : value.batches) {
    encoder.appendU64(batch.ordinal);
    encoder.appendBytes(canonicalCommittedEditIdentityBytes(batch.expected_pre_identity));
    std::vector<std::vector<std::uint8_t>> edits;
    for (const auto& edit : batch.edits)
      edits.push_back(canonicalBufferInsertionMutationBytes(edit));
    std::sort(edits.begin(), edits.end(), detail::CanonicalBytesLess{});
    encoder.appendU64(edits.size());
    for (const auto& edit : edits)
      encoder.appendBytes(edit);
    appendPolicy(encoder, batch.policy_receipt);
    encoder.appendBytes(canonicalCommittedEditIdentityBytes(batch.expected_post_identity));
  }
  encoder.appendString(value.replay_command.executable);
  encoder.appendU64(value.replay_command.arguments.size());
  for (const auto& argument : value.replay_command.arguments)
    encoder.appendString(argument);
  return encoder.bytes();
}

}  // namespace ista::timing_edit
