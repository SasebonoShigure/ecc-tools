// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <string>

#include "FreshBaselineRunner.hpp"

namespace ista::buffer_trace {

struct BufferTraceGenerationRequest
{
  ReplayProgram replay_template;
  SelectionProfile profile;
  std::string case_id;
  std::uint64_t seed{0};
};

struct BufferTraceGenerationResult
{
  BufferTraceArtifact artifact;
  ReplayReceipt functional_receipt;
};

enum class BufferGenerationFailureCode : std::uint8_t
{
  kInvalidRequest,
  kObservationFailed,
  kSelectionFailed,
  kReplayFailed,
  kInvalidArtifact
};

[[nodiscard]] const char* token(BufferGenerationFailureCode code) noexcept;

class BufferGenerationError final : public std::runtime_error
{
 public:
  BufferGenerationError(BufferGenerationFailureCode code, std::string message);
  [[nodiscard]] BufferGenerationFailureCode code() const noexcept { return _code; }

 private:
  BufferGenerationFailureCode _code;
};

class BufferFixedTraceGenerator final
{
 public:
  [[nodiscard]] static BufferTraceGenerationResult generate(
      FreshBaselineRunner& runner, BufferTraceGenerationRequest request);
};

}  // namespace ista::buffer_trace
