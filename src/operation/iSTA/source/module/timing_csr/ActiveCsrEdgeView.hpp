// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

#include "CsrReferenceNativeModels.hpp"

namespace ista::csr {

struct ArcSemanticPayload
{
  ArcKey key;
  NodeId source;
  NodeId sink;
  ArcKind kind{ArcKind::kNet};
  bool is_clock_arc{false};
  bool is_disabled{false};
  bool is_loop_disabled{false};
  ArcEligibility eligibility;
  CellTransferModel transfer;
  std::optional<ArcOutputLoadMatrix> output_load;

  friend bool operator==(const ArcSemanticPayload&, const ArcSemanticPayload&) = default;
};

enum class EdgeStorageKind : std::uint8_t
{
  kBaseOut,
  kAuxOut,
  kBaseIn
};

enum class CsrViewError : std::uint8_t
{
  kNodeOutOfRange,
  kArcOutOfRange,
  kInactiveArc,
  kMalformedAdjacency,
  kCallbackError
};

template <typename T, typename E>
class ExpectedRef final
{
 public:
  [[nodiscard]] static ExpectedRef success(T& value) noexcept { return ExpectedRef(&value, std::nullopt); }
  [[nodiscard]] static ExpectedRef failure(E error) noexcept { return ExpectedRef(nullptr, error); }

  [[nodiscard]] bool hasValue() const noexcept { return _value != nullptr; }
  [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
  [[nodiscard]] T& value() const
  {
    if (_value == nullptr)
      throw std::logic_error("ExpectedRef has no value");
    return *_value;
  }
  [[nodiscard]] E error() const
  {
    if (!_error)
      throw std::logic_error("ExpectedRef has no error");
    return *_error;
  }

 private:
  ExpectedRef(T* value, std::optional<E> error) noexcept : _value(value), _error(std::move(error)) {}

  T* _value{nullptr};
  std::optional<E> _error;
};

struct EdgeEntry
{
  ArcId arc_id;
  EdgeStorageKind storage_kind{EdgeStorageKind::kBaseOut};

  friend bool operator==(const EdgeEntry&, const EdgeEntry&) = default;
};

enum class EdgeVisitControl : std::uint8_t
{
  kContinue,
  kStop,
  kError
};

struct EdgeVisitResult
{
  EdgeVisitControl control{EdgeVisitControl::kContinue};
  std::optional<CsrViewError> error;

  [[nodiscard]] static EdgeVisitResult continueVisit() noexcept { return {}; }
  [[nodiscard]] static EdgeVisitResult stopVisit() noexcept { return {EdgeVisitControl::kStop, std::nullopt}; }
  [[nodiscard]] static EdgeVisitResult fail(CsrViewError value) noexcept { return {EdgeVisitControl::kError, value}; }
};

using EdgeVisitor = std::function<EdgeVisitResult(const EdgeEntry&)>;

class ActiveCsrEdgeView
{
 public:
  virtual ~ActiveCsrEdgeView() = default;

  [[nodiscard]] virtual std::size_t nodeCount() const noexcept = 0;
  [[nodiscard]] virtual std::size_t arcSlotCount() const noexcept = 0;
  [[nodiscard]] virtual std::size_t activeArcCount() const noexcept = 0;
  [[nodiscard]] virtual bool isActive(ArcId arc_id) const noexcept = 0;
  [[nodiscard]] virtual ExpectedRef<const ArcSemanticPayload, CsrViewError> arc(ArcId arc_id) const noexcept = 0;
  [[nodiscard]] virtual EdgeVisitResult forEachActiveArc(const EdgeVisitor& visitor) const = 0;
  [[nodiscard]] virtual EdgeVisitResult forEachOutgoing(NodeId node, const EdgeVisitor& visitor) const = 0;
  [[nodiscard]] virtual EdgeVisitResult forEachIncoming(NodeId node, const EdgeVisitor& visitor) const = 0;
};

struct CsrStageStorageVisits
{
  std::uint64_t base_out{0};
  std::uint64_t aux_out{0};
  std::uint64_t base_in{0};

  friend bool operator==(const CsrStageStorageVisits&, const CsrStageStorageVisits&) = default;
};

struct CsrTraversalTelemetry
{
  CsrStageStorageVisits levelization;
  CsrStageStorageVisits arc_initialization;
  CsrStageStorageVisits forward_propagation;
  CsrStageStorageVisits required_propagation;

  friend bool operator==(const CsrTraversalTelemetry&, const CsrTraversalTelemetry&) = default;
};

inline void recordStorageVisit(CsrStageStorageVisits& visits, EdgeStorageKind kind) noexcept
{
  if (kind == EdgeStorageKind::kBaseOut)
    ++visits.base_out;
  else if (kind == EdgeStorageKind::kAuxOut)
    ++visits.aux_out;
  else
    ++visits.base_in;
}

}  // namespace ista::csr
