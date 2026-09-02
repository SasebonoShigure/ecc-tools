#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "AffectedCapturePlan.hpp"
#include "DualCsrArcGroupIndex.hpp"

namespace ista {
class Database;
}

namespace ista::timing_dual_csr::detail {

struct NativeAffectedArcAdjacency
{
  std::vector<std::size_t> indices_in_native_order;
  std::uint64_t index_group_lookups{0};
  std::uint64_t candidate_neighbor_upper_bound{0};
};

using NativeAffectedArcAdjacencyResult
    = std::variant<NativeAffectedArcAdjacency, NarrowCaptureError>;

[[nodiscard]] NativeAffectedArcAdjacencyResult
collectNativeAffectedArcAdjacency(
    const ista::Database&, const DualCsrArcGroupIndex&,
    const AffectedCapturePlan&);

}  // namespace ista::timing_dual_csr::detail
