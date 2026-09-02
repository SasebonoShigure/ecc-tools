// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <exception>
#include <iostream>
#include <limits>

#include "IncrementalLevelizationTestSupport.hpp"

namespace ista::timing_incremental_levelization::test {
namespace {

void mappingBoundariesAndRoundTrips()
{
  const auto attempt = makeMixedIdDomain(4, 7, 3);
  const auto* domain = std::get_if<MixedIdDomain>(&attempt);
  require(domain != nullptr && domain->n_new_nodes == 3
              && domain->mixed_vertex_count == 6,
          "valid mixed domain was not constructed");
  for (std::uint64_t actual = 4; actual < 7; ++actual) {
    const auto encoded =
        mixedVertexForNewNode(NewActualNode{csr::NodeId{actual}}, *domain);
    const auto* mixed = std::get_if<MixedVertexId>(&encoded);
    require(mixed != nullptr && mixed->value() == 3 + actual - 4,
            "new actual node mapped to the wrong mixed ID");
    const auto decoded = newNodeForMixedVertex(*mixed, *domain);
    const auto* node = std::get_if<NewActualNode>(&decoded);
    require(node != nullptr && node->value() == csr::NodeId{actual},
            "new-node mixed mapping is not round-trip exact");
  }
  for (std::uint64_t level = 0; level < 3; ++level) {
    const auto encoded = mixedVertexForVirtualLevel(
        VirtualOldLevel{level}, *domain);
    const auto* mixed = std::get_if<MixedVertexId>(&encoded);
    require(mixed != nullptr && mixed->value() == level,
            "virtual old level mapped to the wrong mixed ID");
    const auto decoded = virtualLevelForMixedVertex(*mixed, *domain);
    require(std::get<VirtualOldLevel>(decoded).value() == level,
            "virtual level mapping is not round-trip exact");
  }
}

void typedBoundaryFailures()
{
  require(std::get<MixedMappingFailure>(makeMixedIdDomain(5, 4, 2)).code
              == MixedMappingFailureCode::kMalformedDomain,
          "shrinking current domain was not rejected");
  require(std::get<MixedMappingFailure>(makeMixedIdDomain(2, 3, 3)).code
              == MixedMappingFailureCode::kMalformedDomain,
          "too many old levels were not rejected");
  const MixedIdDomain domain = std::get<MixedIdDomain>(
      makeMixedIdDomain(4, 7, 3));
  require(std::get<MixedMappingFailure>(mixedVertexForNewNode(
              NewActualNode{csr::NodeId{3}}, domain)).code
              == MixedMappingFailureCode::kNewNodeOutOfRange,
          "old node accepted a new-node tag");
  require(std::get<MixedMappingFailure>(newNodeForMixedVertex(
              MixedVertexId{2}, domain)).code
              == MixedMappingFailureCode::kWrongMixedVertexKind,
          "virtual marker decoded as a new node");
  require(std::get<MixedMappingFailure>(virtualLevelForMixedVertex(
              MixedVertexId{3}, domain)).code
              == MixedMappingFailureCode::kWrongMixedVertexKind,
          "new-node marker decoded as an old level");
  require(std::get<MixedMappingFailure>(mixedVertexForVirtualLevel(
              VirtualOldLevel{3}, domain)).code
              == MixedMappingFailureCode::kVirtualLevelOutOfRange,
          "out-of-range virtual level was accepted");

  const MixedIdDomain inconsistent{
      std::numeric_limits<std::uint64_t>::max(),
      std::numeric_limits<std::uint64_t>::max(), 2, 0, 2};
  require(std::get<MixedMappingFailure>(newNodeForMixedVertex(
              MixedVertexId{1}, inconsistent)).code
              == MixedMappingFailureCode::kOverflow,
          "mixed-to-actual addition overflow was not typed");
}

}  // namespace
}  // namespace ista::timing_incremental_levelization::test

int main()
{
  try {
    ista::timing_incremental_levelization::test::mappingBoundariesAndRoundTrips();
    ista::timing_incremental_levelization::test::typedBoundaryFailures();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
