#pragma once

#include <cstdint>
#include <string>

#include "CsrReferenceTypes.hpp"

namespace ista::csr {

struct CsrCudaTelemetry
{
  double encode_ms{0.0};
  double h2d_ms{0.0};
  double levelization_ms{0.0};
  double propagation_ms{0.0};
  // Device-event interval for post-propagation status and explicit queries only.
  double delivery_d2h_ms{0.0};
  // Runtime waits for this generation, queries and undo; close is lifecycle cost.
  // Host waits overlap kernel/transfer intervals; never add them to those times.
  double stream_wait_ms{0.0};
  std::uint64_t stream_waits{0};
  double levelization_decision_wait_ms{0.0};
  double level_metadata_wait_ms{0.0};
  double status_wait_ms{0.0};
  double candidate_wait_ms{0.0};
  double result_wait_ms{0.0};
  std::uint64_t level_metadata_d2h_bytes{0};
  std::uint64_t status_d2h_bytes{0};
  std::uint64_t candidate_d2h_bytes{0};
  std::uint64_t result_d2h_bytes{0};
  double end_to_end_ms{0.0};
  std::uint64_t h2d_bytes{0};
  std::uint64_t d2h_bytes{0};
  std::uint64_t d2d_bytes{0};
  std::uint64_t allocations{0};
  // Explicit launches only; library internals are measured by the GPU profiler.
  std::uint64_t kernel_launches{0};
  std::uint64_t device_algorithm_calls{0};
  std::uint64_t levelization_decision_bytes{0};
  std::string device_name;
  std::string device_uuid;
  int driver_version{0};
  int runtime_version{0};
};

struct CsrCudaComputation
{
  CsrComputationResult computation;
  CsrCudaTelemetry telemetry;
};

// Available only in builds explicitly enabling ISTA_ENABLE_CUDA. Each full
// invocation owns fresh device storage; CUDA failures throw with call context.
class CsrCudaBackend final
{
 public:
  static void requireDevice();
  [[nodiscard]] CsrCudaComputation computeWithTelemetry(const CsrReferenceInput& input) const;
};

}  // namespace ista::csr
