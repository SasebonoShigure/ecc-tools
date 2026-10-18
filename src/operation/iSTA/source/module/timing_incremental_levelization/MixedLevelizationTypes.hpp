// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "CsrFifoLevelization.hpp"

namespace ista::timing_incremental_levelization {

class VirtualOldLevel final
{
 public:
  explicit constexpr VirtualOldLevel(std::uint64_t value) : _value(value) {}
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return _value; }
  friend constexpr bool operator==(VirtualOldLevel, VirtualOldLevel) = default;
  friend constexpr bool operator<(VirtualOldLevel lhs, VirtualOldLevel rhs)
  {
    return lhs._value < rhs._value;
  }

 private:
  std::uint64_t _value{0};
};

class NewActualNode final
{
 public:
  explicit constexpr NewActualNode(csr::NodeId value) : _value(value) {}
  [[nodiscard]] constexpr csr::NodeId value() const noexcept { return _value; }
  friend constexpr bool operator==(NewActualNode, NewActualNode) = default;
  friend constexpr bool operator<(NewActualNode lhs, NewActualNode rhs)
  {
    return lhs._value < rhs._value;
  }

 private:
  csr::NodeId _value;
};

class MixedVertexId final
{
 public:
  explicit constexpr MixedVertexId(std::uint64_t value) : _value(value) {}
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return _value; }
  friend constexpr bool operator==(MixedVertexId, MixedVertexId) = default;
  friend constexpr bool operator<(MixedVertexId lhs, MixedVertexId rhs)
  {
    return lhs._value < rhs._value;
  }

 private:
  std::uint64_t _value{0};
};

struct MixedIdDomain
{
  std::uint64_t n_last_nodes{0};
  std::uint64_t n_current_nodes{0};
  std::uint64_t n_new_nodes{0};
  std::uint64_t n_old_levels{0};
  std::uint64_t mixed_vertex_count{0};

  friend bool operator==(const MixedIdDomain&, const MixedIdDomain&) = default;
};

enum class MixedMappingFailureCode : std::uint8_t
{
  kMalformedDomain,
  kOldNodeOutOfRange,
  kNewNodeOutOfRange,
  kVirtualLevelOutOfRange,
  kMixedVertexOutOfRange,
  kWrongMixedVertexKind,
  kMalformedPreviousLevels,
  kOverflow
};

struct MixedMappingFailure
{
  MixedMappingFailureCode code{MixedMappingFailureCode::kMalformedDomain};
  std::string detail;

  friend bool operator==(const MixedMappingFailure&,
                         const MixedMappingFailure&) = default;
};

using MixedIdDomainAttempt = std::variant<MixedIdDomain, MixedMappingFailure>;
using MixedVertexMappingAttempt =
    std::variant<MixedVertexId, MixedMappingFailure>;
using ActualNodeMappingAttempt =
    std::variant<NewActualNode, MixedMappingFailure>;
using VirtualLevelMappingAttempt =
    std::variant<VirtualOldLevel, MixedMappingFailure>;

[[nodiscard]] MixedIdDomainAttempt makeMixedIdDomain(
    std::uint64_t n_last_nodes, std::uint64_t n_current_nodes,
    std::uint64_t n_old_levels);
[[nodiscard]] bool isVirtualOldLevel(MixedVertexId vertex,
                                     const MixedIdDomain& domain) noexcept;
[[nodiscard]] bool isNewActualNode(MixedVertexId vertex,
                                   const MixedIdDomain& domain) noexcept;
[[nodiscard]] MixedVertexMappingAttempt mixedVertexForVirtualLevel(
    VirtualOldLevel level, const MixedIdDomain& domain);
[[nodiscard]] MixedVertexMappingAttempt mixedVertexForNewNode(
    NewActualNode node, const MixedIdDomain& domain);
[[nodiscard]] ActualNodeMappingAttempt newNodeForMixedVertex(
    MixedVertexId vertex, const MixedIdDomain& domain);
[[nodiscard]] VirtualLevelMappingAttempt virtualLevelForMixedVertex(
    MixedVertexId vertex, const MixedIdDomain& domain);
[[nodiscard]] VirtualLevelMappingAttempt virtualLevelForOldNode(
    csr::NodeId node, const csr::CsrFifoLevelization& previous,
    const MixedIdDomain& domain);

enum class MixedStageFailureCode : std::uint8_t
{
  kActualCycle,
  kOldLevelOrderConflict,
  kViewError,
  kMalformedPreviousLevels,
  kMappingOverflow,
  kInvariantFailure
};

struct MixedLevelizationRequest
{
  std::uint64_t n_last_nodes{0};
  std::uint64_t generation_before{0};
  std::uint64_t generation_after{0};

  friend bool operator==(const MixedLevelizationRequest&,
                         const MixedLevelizationRequest&) = default;
};

}  // namespace ista::timing_incremental_levelization
