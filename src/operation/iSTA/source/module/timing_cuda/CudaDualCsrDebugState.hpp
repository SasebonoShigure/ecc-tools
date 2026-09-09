#pragma once
#include <array>
#include <vector>

#include "CsrCudaDeviceTypes.hpp"

namespace ista::csr {
struct CudaDualCsrDebugState
{
  std::vector<cuda_detail::Index> out_offsets, out_arcs, in_offsets, in_arcs;
  std::vector<cuda_detail::Index> aux_offsets, aux_arcs, source_to_aux_row;
  std::vector<cuda_detail::Arc> arcs;
  std::vector<cuda_detail::Child> children;
  std::vector<double> table_data;
  std::array<cuda_detail::Index, 8> storage_visits{};
};
}  // namespace ista::csr
