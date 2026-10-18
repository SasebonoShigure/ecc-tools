#pragma once
#include "CudaDualCsrSession.hpp"
#include "IncreGpuStaBuildConfig.hpp"
#include "json.hpp"
namespace ista::csr {
void runCudaClockContracts();
void runCudaSequentialContracts();
void runCudaRcPropagationContracts();
class CudaDualCsrTestReader final
{
 public:
  static void clockWorkReuseContracts();
  static void reportQueryContracts(CudaLevelizationMode mode);
  static void reportChainContracts();
  // Component-only RC inputs while the public unsupported gate remains closed.
  static CudaDualCsrSession computeRc(const CsrReferenceInput& input, unsigned threads,
      std::optional<timing_edit::AppendOnlyHighWater> high_water = std::nullopt);
  static void setKernelThreads(CudaDualCsrSession& session, unsigned threads);
  static nlohmann::json capture(const CudaDualCsrSession& session);
  static const std::vector<BufferCandidateTiming>& readCandidateTiming(const CudaDualCsrSession& session, std::span<const NodeId> nodes);
  static void omitAuxAndRecompute(CudaDualCsrSession& candidate);
#if ECC_INCREGPUSTA_DEBUG_CHECKS
  static void corruptContext();
#endif
};
}  // namespace ista::csr
