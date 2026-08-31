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
#include <stdexcept>
#include <string>

#include "FreshBaselineRunner.hpp"

namespace ista::buffer_trace {

class BufferFixedTraceGenerator;
class BufferReplayContext;

enum class BufferReplayFailureCode : std::uint8_t
{
  kInvalidProgram,
  kPrefixOutsideTrace,
  kContextMismatch,
  kGenerationMismatch,
  kPrepareRejected,
  kTranslationRejected,
  kNativeExecutionFailed,
  kCsrSnapshotFailed,
  kCsrComputationFailed,
  kPublishRejected
};

[[nodiscard]] const char* token(BufferReplayFailureCode code) noexcept;

class BufferReplayError final : public std::runtime_error
{
 public:
  BufferReplayError(BufferReplayFailureCode code, std::string message);
  [[nodiscard]] BufferReplayFailureCode code() const noexcept { return _code; }

 private:
  BufferReplayFailureCode _code;
};

class BufferTraceReplayer final
{
 public:
  [[nodiscard]] static ReplayReceipt replayPrefix(
      BufferReplayContext& context, const ReplayProgram& program,
      std::size_t prefix_batches);

 private:
  [[nodiscard]] static FunctionalBatchReceipt applyNextBatch(
      BufferReplayContext& context, const timing_edit::DesignEditPlan& plan,
      std::size_t batch_ordinal);
  [[nodiscard]] static ReplayReceipt finishCurrent(
      BufferReplayContext& context, const ReplayProgram& program,
      std::size_t expected_prefix,
      std::vector<FunctionalBatchReceipt> receipts);

  friend class BufferFixedTraceGenerator;
};

// Public replay accepts only the replay-safe projection. Generation receipts,
// selector operands, gains, and expected timing answers are unrepresentable.
[[nodiscard]] ReplayReceipt replayTracePrefix(BufferReplayContext& context,
                                              const ReplayProgram& program,
                                              std::size_t prefix_batches);

}  // namespace ista::buffer_trace
