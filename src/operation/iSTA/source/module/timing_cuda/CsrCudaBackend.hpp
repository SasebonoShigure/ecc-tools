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
  double d2h_ms{0.0};
  double end_to_end_ms{0.0};
  std::uint64_t h2d_bytes{0};
  std::uint64_t d2h_bytes{0};
  std::uint64_t d2d_bytes{0};
  std::uint64_t allocations{0};
  std::uint64_t kernel_launches{0};
  std::string device_name;
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
