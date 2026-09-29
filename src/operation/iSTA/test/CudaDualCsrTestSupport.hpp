#pragma once

#include "CudaDualCsrSession.hpp"

namespace ista::csr::test {
inline CudaDualCsrPatch patchFor(const CsrReferenceInput& input, std::uint64_t generation, timing_edit::AppendOnlyHighWater before,
                          timing_edit::AppendOnlyHighWater after)
{
  CudaDualCsrPatch patch;
  patch.session_token = timing_edit::TimingEditSessionToken{input.source_identity.session_token};
  patch.expected_generation = timing_edit::TransactionGeneration{generation};
  patch.target_generation = timing_edit::TransactionGeneration{generation + 1};
  patch.before = before;
  patch.after = after;
  return patch;
}

void runCudaParallelPatchContracts(CudaLevelizationMode mode);
void runCudaLazySequence(CudaLevelizationMode mode);
void runCudaDualCsrStorageContracts(CudaLevelizationMode mode);
}  // namespace ista::csr::test
