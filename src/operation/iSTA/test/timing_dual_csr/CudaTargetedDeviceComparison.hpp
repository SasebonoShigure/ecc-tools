#pragma once
#include "CudaTargetedTraceReplayer.hpp"
namespace ista::csr::test {
// Reads the real ON device observation, not a host descriptor replay.
void requireCudaDeviceMatchesCanonical(const CudaTargetedTraceObservation& observation);
}  // namespace ista::csr::test
