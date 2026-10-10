// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <optional>

#include "DualCsrStateValidation.hpp"

namespace ista { class CudaCsrEditState; }

namespace ista::timing_dual_csr {

struct DualCsrBaselineAttempt
{
  std::optional<DualCsrState> state;
  DualCsrViolations violations;

  [[nodiscard]] bool success() const noexcept
  {
    return state.has_value() && violations.empty();
  }
};

class DualCsrBaselineFactory final
{
 public:
  [[nodiscard]] static DualCsrBaselineAttempt create(
      const csr::CsrReferenceInput& input,
      timing_edit::TimingEditSessionToken session_token,
      timing_edit::AppendOnlyHighWater high_water);

 private:
  friend class ista::CudaCsrEditState;
  // Session-owned exports already enforce their input contract while constructing the graph.
  [[nodiscard]] static DualCsrBaselineAttempt fromNativeExport(
      const csr::CsrReferenceInput& input,
      timing_edit::TimingEditSessionToken session_token,
      timing_edit::AppendOnlyHighWater high_water);
};

}  // namespace ista::timing_dual_csr
