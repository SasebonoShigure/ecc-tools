// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "BufferTraceTypes.hpp"
#include "CsrReferenceTypes.hpp"
#include "TimingEditCoordinator.hpp"
#include "TimingEditResolver.hpp"

namespace ista::buffer_trace {

class BufferFixedTraceGenerator;
class BufferReplayContext;

enum class FreshBaselineFailureCode : std::uint8_t
{
  kFreshProcessReuseRejected,
  kArtifactMaterializationRejected,
  kArtifactEvidenceMismatch,
  kBaselineIdentityMismatch,
  kBaselineCaptureFailed,
  kGuardedWorkFailed
};

[[nodiscard]] const char* token(FreshBaselineFailureCode code) noexcept;

class FreshBaselineError final : public std::runtime_error
{
 public:
  FreshBaselineError(FreshBaselineFailureCode code, std::string message);
  [[nodiscard]] FreshBaselineFailureCode code() const noexcept { return _code; }

 private:
  FreshBaselineFailureCode _code;
};

struct NativeInstanceFunctionalRecord
{
  std::string name;
  std::string cell;
  std::vector<std::string> ordered_pins;
  bool sequential{false};
  bool clock_gating{false};
  bool io_cell{false};

  friend bool operator==(const NativeInstanceFunctionalRecord&,
                         const NativeInstanceFunctionalRecord&) = default;
};

struct NativePinFunctionalRecord
{
  std::string full_name;
  std::string pin_name;
  std::string instance_name;
  std::string net_name;
  std::int32_t direction{0};
  bool port{false};

  friend bool operator==(const NativePinFunctionalRecord&,
                         const NativePinFunctionalRecord&) = default;
};

struct NativeNetFunctionalRecord
{
  std::string name;
  std::vector<std::string> ordered_drivers;
  std::vector<std::string> ordered_loads;
  std::vector<std::string> ordered_pins;

  friend bool operator==(const NativeNetFunctionalRecord&,
                         const NativeNetFunctionalRecord&) = default;
};

struct NativeDatabaseFunctionalReceipt
{
  std::string design_name;
  std::vector<NativeInstanceFunctionalRecord> instances;
  std::vector<NativePinFunctionalRecord> pins;
  std::vector<NativeNetFunctionalRecord> nets;

  friend bool operator==(const NativeDatabaseFunctionalReceipt&,
                         const NativeDatabaseFunctionalReceipt&) = default;
};

struct FunctionalBatchReceipt
{
  std::size_t batch_ordinal{0};
  timing_edit::TransactionGeneration expected_generation;
  timing_edit::TransactionGeneration target_generation;
  std::vector<timing_edit::BufferInsertionMutation> normalized_edits;
  timing_edit::TimingGraphDelta forward;
  timing_edit::TimingGraphDelta reverse;
  std::vector<timing_edit::TokenMaterialization> materializations;
  timing_edit::CommittedEditIdentity post_identity;
  csr::CsrReferenceInput csr_input;
  csr::CsrTimingResult csr_timing;
  NativeDatabaseFunctionalReceipt native_database;
};

// This receipt owns only values. In particular it never owns or refers to an
// operation guard, native authority, Database, coordinator, or CSR proof.
struct ReplayReceipt
{
  ReplayHeader header;
  std::size_t prefix_batches{0};
  std::vector<FunctionalBatchReceipt> batches;
  timing_edit::CommittedEditIdentity committed_identity;
  timing_edit::CumulativeEditTrace cumulative_trace;
  timing_edit::TimingEditResolverState resolver_state;
  NativeDatabaseFunctionalReceipt final_database;
  csr::CsrReferenceInput final_csr_input;
  csr::CsrTimingResult final_csr_timing;
};

[[nodiscard]] bool functionallyEqual(const FunctionalBatchReceipt& lhs,
                                     const FunctionalBatchReceipt& rhs);
[[nodiscard]] bool functionallyEqual(const ReplayReceipt& lhs,
                                     const ReplayReceipt& rhs);

using BufferReplayWork = std::function<ReplayReceipt(BufferReplayContext&)>;

// The owning loader remains outside the runtime target. This callback-only
// interface prevents a guarded session/context from being returned or cached.
class FreshBaselineRunner
{
 public:
  FreshBaselineRunner() = default;
  virtual ~FreshBaselineRunner() = default;

  FreshBaselineRunner(const FreshBaselineRunner&) = delete;
  FreshBaselineRunner& operator=(const FreshBaselineRunner&) = delete;
  FreshBaselineRunner(FreshBaselineRunner&&) = delete;
  FreshBaselineRunner& operator=(FreshBaselineRunner&&) = delete;

  [[nodiscard]] virtual ReplayReceipt withFreshBaseline(
      const ReplayProgram& program, BufferReplayWork work) = 0;

 private:
  [[nodiscard]] virtual ReplayReceipt withFreshGenerationBaseline(
      const ReplayProgram& empty_template, BufferReplayWork work) = 0;
  friend class BufferFixedTraceGenerator;
};

}  // namespace ista::buffer_trace
