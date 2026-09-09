#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "CsrCudaBackend.hpp"
#include "CudaDualCsrPatch.hpp"
#include "MixedLevelizationTelemetry.hpp"
#include "MixedLevelizationTypes.hpp"

namespace ista::csr {

enum class CudaLevelizationMode
{
  kFull,
  kMixed
};

struct CudaDualCsrTelemetry
{
  CsrCudaTelemetry cuda;
  double update_ms{0.0};
  double mixed_prepare_ms{0.0};
  timing_incremental_levelization::MixedLevelizationTelemetry mixed;
  std::optional<timing_incremental_levelization::MixedStageFailureCode> mixed_failure;
  // Persistent buffers whose allocated element capacity grew from the origin.
  // Per-attempt descriptor scratch is counted in cuda.allocations only.
  std::uint64_t grown_buffers{0};
};

struct CudaDualCsrDebugState;

class CudaDualCsrSession final
{
 public:
  CudaDualCsrSession(const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater high_water);
  ~CudaDualCsrSession();
  CudaDualCsrSession(CudaDualCsrSession&&) noexcept;
  CudaDualCsrSession& operator=(CudaDualCsrSession&&) noexcept;
  CudaDualCsrSession(const CudaDualCsrSession&) = delete;
  CudaDualCsrSession& operator=(const CudaDualCsrSession&) = delete;

  [[nodiscard]] CudaDualCsrSession stage(const CudaDualCsrPatch& patch, CudaLevelizationMode mode = CudaLevelizationMode::kFull);
  [[nodiscard]] bool publish(CudaDualCsrSession&& candidate) noexcept;
  [[nodiscard]] const CsrTimingValues& timingValues() const;
  [[nodiscard]] std::optional<CsrComputationCode> computationCode() const noexcept;
  [[nodiscard]] const CudaDualCsrTelemetry& telemetry() const;
  [[nodiscard]] timing_edit::TransactionGeneration generation() const noexcept;
  [[nodiscard]] bool poisoned() const noexcept;
  // Full device readback is available only with development checks enabled.
  [[nodiscard]] CudaDualCsrDebugState debugState() const;

 private:
  // The implementation of this reader is linked only into the acceptance test.
  friend class CudaDualCsrTestReader;
  struct Impl;
  explicit CudaDualCsrSession(std::shared_ptr<Impl> impl) noexcept;
  std::shared_ptr<Impl> _impl;
};

}  // namespace ista::csr
