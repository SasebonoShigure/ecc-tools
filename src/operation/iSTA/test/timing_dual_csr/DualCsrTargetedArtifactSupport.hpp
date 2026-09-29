// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <memory>
#include <vector>

#include "BufferTraceProductionFreshAdapter.hpp"
#include "ProductionFreshSessionLoader.hpp"

namespace ista::buffer_trace {
struct ReplayProgram;
}

namespace ista::timing_dual_csr::test {

class DualCsrTargetedArtifactSupport final
{
 public:
  explicit DualCsrTargetedArtifactSupport(
      const buffer_trace::ReplayProgram& program);
  ~DualCsrTargetedArtifactSupport();

  DualCsrTargetedArtifactSupport(
      const DualCsrTargetedArtifactSupport&) = delete;
  DualCsrTargetedArtifactSupport& operator=(
      const DualCsrTargetedArtifactSupport&) = delete;
  DualCsrTargetedArtifactSupport(
      DualCsrTargetedArtifactSupport&&) = delete;
  DualCsrTargetedArtifactSupport& operator=(
      DualCsrTargetedArtifactSupport&&) = delete;

  [[nodiscard]] const timing_edit::oracle::FreshBaselineArtifacts&
  artifacts() const noexcept;
  [[nodiscard]] std::vector<buffer_trace::SourceArtifactFunctionalRecord>
  capture() const;
  // Explicit success path reports cleanup errors; destruction remains no-throw.
  void close();

 private:
  class Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace ista::timing_dual_csr::test
