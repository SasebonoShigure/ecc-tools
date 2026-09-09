#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "CsrCudaBackend.hpp"
#include "DualCsrDescriptors.hpp"

namespace ista::csr {

struct CudaAppendedBaseInRow
{
  NodeId sink;
  std::vector<ArcId> arc_ids;
};

// Only changed data crosses this boundary; no full CPU graph or timing shadow.
struct CudaDualCsrPatch
{
  timing_edit::TimingEditSessionToken session_token;
  timing_edit::TransactionGeneration expected_generation, target_generation;
  timing_edit::AppendOnlyHighWater before, after;
  std::vector<timing_dual_csr::NodeAppendRecord> node_appends;
  std::vector<timing_dual_csr::ArcAppendRecord> arc_appends;
  std::vector<timing_dual_csr::BaseOutRemoveDescriptor> removes;
  std::vector<timing_dual_csr::BaseOutChangeDescriptor> changes;
  std::vector<timing_dual_csr::BaseInChangeDescriptor> reverse_changes;
  std::vector<timing_dual_csr::ArcPayloadChangeDescriptor> payload_changes;
  std::vector<timing_dual_csr::LogicalArcDeactivate> logical_deactivations;
  std::vector<timing_dual_csr::AuxOutRowDescriptor> aux_rows;
  std::vector<timing_dual_csr::AppendedBaseOutRowDescriptor> appended_base_out_rows;
  std::vector<CudaAppendedBaseInRow> appended_base_in_rows;
  // Production buffer insertion leaves these absent. Algorithm tests may append
  // source/end nodes, using existing clock tags and explicit initial seeds.
  std::optional<timing_dual_csr::ValueReplacement<std::vector<NodeId>>> roots, ends;
  std::vector<StartSeedModel> appended_start_seeds;
  std::vector<EndpointRequiredModel> appended_endpoints;
};

struct CudaDualCsrTelemetry
{
  CsrCudaTelemetry cuda;
  double update_ms{0.0};
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

  [[nodiscard]] CudaDualCsrSession stage(const CudaDualCsrPatch& patch);
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
