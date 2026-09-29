#pragma once

#include <cuda_runtime_api.h>
#include <algorithm>

#include "CsrCudaDeviceTypes.hpp"

namespace ista::csr::cuda_detail {

inline unsigned kernelBlocks(Index count, unsigned threads = 256)
{
  return static_cast<unsigned>(std::max<Index>(1, std::min<Index>(65535, count / threads + (count % threads != 0))));
}

#ifdef __CUDACC__
// A shared status is never read non-atomically while another thread can fail.
__device__ inline Status readStatus(Status* status)
{
  return static_cast<Status>(atomicAdd(reinterpret_cast<int*>(status), 0));
}
__device__ inline void recordFailure(Status* status, Status failure)
{
  if (failure != Status::success)
    atomicCAS(reinterpret_cast<int*>(status), static_cast<int>(Status::success), static_cast<int>(failure));
}
#endif

std::uint64_t launchSeedInitialization(DeviceView view, const NodeSeed* nodes, Index node_count,
                                       const PathSeed* paths, Index path_count, cudaStream_t stream, unsigned threads);

}  // namespace ista::csr::cuda_detail
