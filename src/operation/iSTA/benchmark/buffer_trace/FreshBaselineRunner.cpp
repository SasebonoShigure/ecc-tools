// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#include "FreshBaselineRunner.hpp"

#include <utility>

#include "CsrReferenceCanonicalEncoder.hpp"

namespace ista::buffer_trace {

const char* token(FreshBaselineFailureCode code) noexcept
{
  switch (code) {
    case FreshBaselineFailureCode::kFreshProcessReuseRejected:
      return "fresh_process_reuse_rejected";
    case FreshBaselineFailureCode::kArtifactMaterializationRejected:
      return "artifact_materialization_rejected";
    case FreshBaselineFailureCode::kArtifactEvidenceMismatch:
      return "artifact_evidence_mismatch";
    case FreshBaselineFailureCode::kBaselineIdentityMismatch:
      return "baseline_identity_mismatch";
    case FreshBaselineFailureCode::kBaselineCaptureFailed:
      return "baseline_capture_failed";
    case FreshBaselineFailureCode::kGuardedWorkFailed:
      return "guarded_work_failed";
  }
  return "unknown_fresh_baseline_failure";
}

FreshBaselineError::FreshBaselineError(FreshBaselineFailureCode code,
                                       std::string message)
    : std::runtime_error(std::string(token(code)) + ": " + std::move(message)),
      _code(code)
{
}

bool functionallyEqual(const FunctionalBatchReceipt& lhs,
                       const FunctionalBatchReceipt& rhs)
{
  return lhs.batch_ordinal == rhs.batch_ordinal
         && lhs.expected_generation == rhs.expected_generation
         && lhs.target_generation == rhs.target_generation
         && lhs.normalized_edits == rhs.normalized_edits
         && lhs.forward == rhs.forward && lhs.reverse == rhs.reverse
         && lhs.materializations == rhs.materializations
         && lhs.post_identity == rhs.post_identity
         && lhs.csr_input.source_identity == rhs.csr_input.source_identity
         && csr::canonicalReferenceInputBytes(lhs.csr_input)
                == csr::canonicalReferenceInputBytes(rhs.csr_input)
         && lhs.csr_timing == rhs.csr_timing
         && lhs.native_database == rhs.native_database;
}

bool functionallyEqual(const ReplayReceipt& lhs, const ReplayReceipt& rhs)
{
  if (!(lhs.header == rhs.header)
      || lhs.prefix_batches != rhs.prefix_batches
      || lhs.batches.size() != rhs.batches.size()
      || lhs.committed_identity != rhs.committed_identity
      || lhs.resolver_state != rhs.resolver_state
      || lhs.final_database != rhs.final_database
      || lhs.final_csr_input.source_identity
             != rhs.final_csr_input.source_identity
      || csr::canonicalReferenceInputBytes(lhs.final_csr_input)
             != csr::canonicalReferenceInputBytes(rhs.final_csr_input)
      || lhs.final_csr_timing != rhs.final_csr_timing) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.batches.size(); ++index) {
    if (!functionallyEqual(lhs.batches[index], rhs.batches[index])) {
      return false;
    }
  }
  return true;
}

}  // namespace ista::buffer_trace
