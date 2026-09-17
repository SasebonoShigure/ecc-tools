// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "DualCsrExactReceipt.hpp"
#include "DualCsrResourceObservation.hpp"

namespace ista {
class Database;
namespace csr {
struct CsrReferenceInput;
}
namespace timing_edit {
class NativeExecutionAuthority;
}
}  // namespace ista

namespace ista::timing_dual_csr {

struct DualCsrTraceContext
{
  Database& baseline_database;
  const csr::CsrReferenceInput& baseline_reference_input;
  const timing_edit::NativeExecutionAuthority& authority;
  std::uint64_t session_token{0};
};

struct DualCsrBatchPhaseTimings
{
  std::uint64_t generation_before{0};
  std::uint64_t generation_after{0};
  double pre_candidate_prepare_ms{0.0};
  double candidate_apply_execute_ms{0.0};
  double post_candidate_capture_ms{0.0};
  double dual_csr_update_ms{0.0};
  double full_levelization_ms{0.0};
  double mixed_build_ms{0.0};
  double mixed_kahn_ms{0.0};
  double reconstruct_ms{0.0};
  double full_propagation_ms{0.0};
  double invariant_ms{0.0};
  double verify_ms{0.0};
  double publish_ms{0.0};

  friend bool operator==(const DualCsrBatchPhaseTimings&,
                         const DualCsrBatchPhaseTimings&) = default;
};

struct DualCsrTraceRunEnvelope
{
  std::uint64_t baseline_factory_calls_outside_targeted{0};
  std::uint64_t baseline_factory_calls_inside_targeted{0};
  std::vector<DualCsrBatchPhaseTimings> batch_phase_timings;
  std::vector<DualCsrResourceObservation> batch_resource_observations;

  friend bool operator==(const DualCsrTraceRunEnvelope&,
                         const DualCsrTraceRunEnvelope&) = default;
};

struct DualCsrWorkloadResult
{
  std::uint64_t input_node_slots{0};
  std::uint64_t input_arc_slots{0};
  LevelizationExecutionMode requested_mode{LevelizationExecutionMode::kFullStage5};
  std::uint64_t generation{0};
  std::uint64_t node_slots{0};
  std::uint64_t arc_slots{0};
  std::uint64_t fallback_count{0};
  LevelizationExecutionMode executed_mode{LevelizationExecutionMode::kFullStage5};
  bool native_final_rollback_completed{false};
};

struct DualCsrTraceRunResult
{
  std::optional<DualCsrTransactionReceipt> transaction_receipt;
  DualCsrWorkloadResult result;
  std::vector<DualCsrPublishedObservation> published_observations;
  DualCsrTraceRunEnvelope envelope;
};

enum class DualCsrTraceFailureCode : std::uint8_t
{
  kInvalidProgram,
  kInvalidBaseline,
  kPrepareRejected,
  kCandidateRejected,
  kCaptureRejected,
  kStageRejected,
  kPublishRejected,
  kRollbackRejected,
  kForbiddenOperationObserved,
  kReceiptRejected
};

class DualCsrTraceRuntimeError final : public std::runtime_error
{
 public:
  DualCsrTraceRuntimeError(DualCsrTraceFailureCode code,
                           std::string message)
      : std::runtime_error(std::move(message)), _code(code)
  {
  }

  [[nodiscard]] DualCsrTraceFailureCode code() const noexcept
  {
    return _code;
  }

 private:
  DualCsrTraceFailureCode _code;
};

}  // namespace ista::timing_dual_csr
