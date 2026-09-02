// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include "AffectedCapturePlan.hpp"
#include "CsrReferenceInputAdapterInternal.hpp"
#include "NarrowTimingCaptureInternal.hpp"

namespace ista::timing_dual_csr::detail {

struct CaptureAbort
{
  NarrowCaptureError error;
};

[[noreturn]] void abortCapture(NarrowCaptureError error);

class CaptureRun final
{
 public:
  explicit CaptureRun(const AffectedCapturePlan& plan);

  [[nodiscard]] NarrowTimingRecords& records() noexcept
  {
    return _records;
  }

  [[nodiscard]] NativeCaptureResult fail(NarrowCaptureError error);
  [[nodiscard]] NativeCaptureResult succeed();

 private:
  void observeExports();

  NarrowTimingRecords _records;
  csr::detail::NativeFullExportCounters _before;
};

}  // namespace ista::timing_dual_csr::detail
