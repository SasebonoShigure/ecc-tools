// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include "IncreGpuStaBuildConfig.hpp"
#include "json.hpp"

namespace ista::timing_dual_csr::test {

inline nlohmann::json incregpustaBuildInfo()
{
  return {{"debug_checks_enabled", incregpusta_debug_checks_enabled},
          {"cuda_enabled", incregpusta_cuda_enabled},
          {"build_config", incregpusta_build_config},
          {"compiler_id", incregpusta_compiler_id},
          {"compiler_version", incregpusta_compiler_version}};
}

}  // namespace ista::timing_dual_csr::test
