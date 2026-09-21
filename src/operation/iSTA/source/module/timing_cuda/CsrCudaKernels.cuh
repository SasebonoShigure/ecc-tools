#pragma once

#include <cuda_runtime_api.h>

#include "CsrCudaDeviceTypes.hpp"

namespace ista::csr::cuda_detail {

std::uint64_t launchSeedInitialization(DeviceView view, const NodeSeed* nodes, Index node_count,
                                       const PathSeed* paths, Index path_count, cudaStream_t stream);
void launchLevelization(DeviceView view, cudaStream_t stream);
std::uint64_t launchPropagation(DeviceView view, cudaStream_t stream);

}  // namespace ista::csr::cuda_detail
