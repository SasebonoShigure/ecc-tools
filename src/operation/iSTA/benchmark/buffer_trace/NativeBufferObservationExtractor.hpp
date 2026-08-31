// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "BufferTraceTypes.hpp"

namespace ista {
class Database;
}

namespace ista::timing_edit {
class TimingEditResolverState;
}

namespace ista::buffer_trace {

enum class NativeObservationFailureCode : std::uint8_t
{
  kInvalidProfile,
  kInvalidNativeDatabase,
  kInvalidResolver,
  kInvalidLibraryTable
};

[[nodiscard]] const char* token(NativeObservationFailureCode code) noexcept;

class NativeObservationError final : public std::runtime_error
{
 public:
  NativeObservationError(NativeObservationFailureCode code, std::string message);
  [[nodiscard]] NativeObservationFailureCode code() const noexcept { return _code; }

 private:
  NativeObservationFailureCode _code;
};

// Reads a completed native CPU/full Database. It creates one candidate per
// legal (single-driver data net, instance-input sink, legal buffer cell)
// tuple and never writes native or derived state.
class NativeBufferObservationExtractor final
{
 public:
  [[nodiscard]] static std::vector<BufferCandidateObservation> extract(
      const Database& database,
      const timing_edit::TimingEditResolverState& resolver,
      const SelectionProfile& profile);
};

}  // namespace ista::buffer_trace
