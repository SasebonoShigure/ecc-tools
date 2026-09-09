#pragma once
#include "CudaDualCsrSession.hpp"
#include "json.hpp"
namespace ista::csr {
class CudaDualCsrTestReader final
{
 public:
  static nlohmann::json capture(const CudaDualCsrSession& session);
  static void omitAuxAndRecompute(CudaDualCsrSession& candidate);
};
}  // namespace ista::csr
