// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include <exception>
#include <iostream>
#include <vector>

#include "IncrementalLevelizationTestSupport.hpp"
#include "MixedLevelReconstructor.hpp"

namespace ista::timing_incremental_levelization::test {
namespace {

MixedKahnLevelization interleavedMixedLevels()
{
  MixedKahnLevelization mixed;
  mixed.level_offsets = {0, 2, 4};
  mixed.level_vertices = {
      MixedVertexId{0}, MixedVertexId{2},
      MixedVertexId{1}, MixedVertexId{3}};
  mixed.vertex_to_level = {1, 2, 1, 2};
  mixed.processed_count = 4;
  mixed.max_frontier = 2;
  return mixed;
}

void reconstructsPermutationAndInverse()
{
  const MixedIdDomain domain =
      std::get<MixedIdDomain>(makeMixedIdDomain(3, 5, 2));
  const auto previous = makeLevels(3, {{0, 1}, {2}});
  const auto attempt = reconstructMixedLevelization(
      domain, previous, interleavedMixedLevels());
  const auto* levels = std::get_if<csr::CsrFifoLevelization>(&attempt);
  require(levels != nullptr, "valid mixed levels did not reconstruct");
  require(levels->complete && !levels->view_error
              && levels->level_offsets
                     == std::vector<std::uint64_t>{0, 3, 5}
              && levels->level_nodes
                     == std::vector<csr::NodeId>{
                         csr::NodeId{0}, csr::NodeId{1}, csr::NodeId{3},
                         csr::NodeId{2}, csr::NodeId{4}}
              && levels->node_to_level
                     == std::vector<std::uint64_t>{1, 1, 2, 1, 2},
          "actual reconstruction lost deterministic expansion semantics");
}

void reconstructionCanaries()
{
  const MixedIdDomain domain =
      std::get<MixedIdDomain>(makeMixedIdDomain(3, 5, 2));
  const auto previous = makeLevels(3, {{0, 1}, {2}});
  auto two_virtual = interleavedMixedLevels();
  two_virtual.level_vertices = {
      MixedVertexId{0}, MixedVertexId{1},
      MixedVertexId{2}, MixedVertexId{3}};
  two_virtual.vertex_to_level = {1, 1, 2, 2};
  const auto rejected = reconstructMixedLevelization(
      domain, previous, two_virtual);
  require(std::get<MixedReconstructionFailure>(rejected).code
              == MixedReconstructionFailureCode::kInvariantFailure,
          "multiple virtual markers in one mixed level were accepted");

  auto duplicate = interleavedMixedLevels();
  duplicate.level_vertices[3] = MixedVertexId{2};
  const auto duplicate_attempt = reconstructMixedLevelization(
      domain, previous, duplicate);
  require(std::get<MixedReconstructionFailure>(duplicate_attempt).code
              == MixedReconstructionFailureCode::kInvariantFailure,
          "duplicate mixed vertex was accepted");

  auto malformed_previous = previous;
  malformed_previous.view_error = csr::CsrViewError::kCallbackError;
  const auto malformed_attempt = reconstructMixedLevelization(
      domain, malformed_previous, interleavedMixedLevels());
  require(std::get<MixedReconstructionFailure>(malformed_attempt).code
              == MixedReconstructionFailureCode::kMalformedPreviousLevels,
          "previous view_error was ignored by reconstruction");
}

}  // namespace
}  // namespace ista::timing_incremental_levelization::test

int main()
{
  try {
    ista::timing_incremental_levelization::test::reconstructsPermutationAndInverse();
    ista::timing_incremental_levelization::test::reconstructionCanaries();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
