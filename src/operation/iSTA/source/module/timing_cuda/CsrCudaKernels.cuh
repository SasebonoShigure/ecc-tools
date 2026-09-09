#pragma once

#include <cuda_runtime_api.h>

#include "CsrCudaDeviceTypes.hpp"

namespace ista::csr::cuda_detail {

void launchLevelization(DeviceView view, cudaStream_t stream);
std::uint64_t launchPropagation(DeviceView view, cudaStream_t stream);

}  // namespace ista::csr::cuda_detail
