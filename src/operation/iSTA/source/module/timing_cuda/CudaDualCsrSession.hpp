#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "CsrCudaBackend.hpp"
#include "CsrBufferCandidateTiming.hpp"
#include "CsrReportValues.hpp"
#include "CudaDualCsrPatch.hpp"
#include "MixedLevelizationTelemetry.hpp"
#include "MixedLevelizationTypes.hpp"

namespace ista {
class Database;
class StaCsrSessionContext;
class StaCudaSessionState;
class PhysicalStaSessionState;
}
namespace ista::csr {
class CudaDualCsrSession;


enum class CudaLevelizationMode
{
  kFull,
  kMixed
};

struct CudaDualCsrTelemetry
{
  CsrCudaTelemetry cuda;
  double update_ms{0.0};
  double materialize_ms{0.0};
  double mixed_prepare_ms{0.0};
  timing_incremental_levelization::MixedLevelizationTelemetry mixed;
  std::optional<timing_incremental_levelization::MixedStageFailureCode> mixed_failure;
  // Device buffers whose capacity grew, including descriptor and undo scratch.
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
  [[nodiscard]] bool canPublish(const CudaDualCsrSession& candidate) const noexcept;
  [[nodiscard]] bool publish(CudaDualCsrSession&& candidate) noexcept;
  [[nodiscard]] bool close() noexcept;
  void requireCommitted() const;
  [[nodiscard]] const CsrTimingValues& timingValues() const;
  [[nodiscard]] const std::vector<value_detail::NodeTimingScalars>& timingScalars() const;
  [[nodiscard]] std::optional<CsrComputationCode> computationCode() const noexcept;
  [[nodiscard]] const CudaDualCsrTelemetry& telemetry() const;
  [[nodiscard]] timing_edit::TransactionGeneration generation() const noexcept;
  [[nodiscard]] bool poisoned() const noexcept;
  // Full device readback is available only with development checks enabled.
  [[nodiscard]] CudaDualCsrDebugState debugState() const;

 private:
  // The implementation of this reader is linked only into the acceptance test.
  friend class CudaDualCsrTestReader;
  friend class CsrCudaBackend;
  friend class ista::StaCudaSessionState;
  friend class ista::PhysicalStaSessionState;
  [[nodiscard]] const std::vector<BufferCandidateTiming>& candidateTiming(std::span<const NodeId> nodes) const;
  [[nodiscard]] const CsrReportValues& reportValues(std::span<const NodeId> nodes, std::span<const CsrQueryPath> paths) const;
  [[nodiscard]] std::vector<CsrQueryPath> prepareReport(const CsrReportRequest& request) const;
  [[nodiscard]] std::span<const std::string> reportClockNames() const;
  // The native export already validated the initialization input.
  [[nodiscard]] static CudaDualCsrSession fromNativeExport(
      const CsrReferenceInput& input, timing_edit::AppendOnlyHighWater high_water);
  struct Impl;
  explicit CudaDualCsrSession(std::shared_ptr<Impl> impl) noexcept;
  std::shared_ptr<Impl> _impl;
};

}  // namespace ista::csr
