// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <type_traits>
#include <variant>

#include "TimingEditTypes.hpp"

namespace ista::timing_dual_csr::detail {

struct StableArcEndpointRefs
{
  timing_edit::StableNodeRef source;
  timing_edit::StableNodeRef sink;
};

[[nodiscard]] inline StableArcEndpointRefs stableArcEndpointRefs(
    const timing_edit::StableArcRef& arc)
{
  return std::visit(
      [](const auto& ref) -> StableArcEndpointRefs {
        using Ref = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Ref,
                                     timing_edit::ExistingArcRef>) {
          return {
              timing_edit::ExistingNodeRef{
                  ref.baseline_key.base.source},
              timing_edit::ExistingNodeRef{
                  ref.baseline_key.base.sink}};
        } else if constexpr (std::is_same_v<
                                 Ref,
                                 timing_edit::CreatedCellArcRef>) {
          return {
              timing_edit::CreatedNodeRef{
                  ref.creation_token,
                  timing_edit::CreatedNodeRole::kInput},
              timing_edit::CreatedNodeRef{
                  ref.creation_token,
                  timing_edit::CreatedNodeRole::kOutput}};
        } else if constexpr (std::is_same_v<
                                 Ref,
                                 timing_edit::CreatedOldNetArcRef>) {
          return {ref.driver, ref.sink};
        } else {
          static_assert(std::is_same_v<
                        Ref, timing_edit::CreatedNewNetArcRef>);
          return {ref.driver, ref.sink};
        }
      },
      arc);
}

}  // namespace ista::timing_dual_csr::detail
