// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.

#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

#include "CsrReferenceTypes.hpp"

namespace ista::csr::test::wire {

class Writer
{
 public:
  void appendU8(std::uint8_t value) { _bytes.push_back(value); }

  void appendBool(bool value) { appendU8(value ? 1 : 0); }

  void appendU32(std::uint32_t value)
  {
    for (int shift = 24; shift >= 0; shift -= 8)
      appendU8(static_cast<std::uint8_t>(value >> shift));
  }

  void appendI32(std::int32_t value) { appendU32(std::bit_cast<std::uint32_t>(value)); }

  void appendU64(std::uint64_t value)
  {
    for (int shift = 56; shift >= 0; shift -= 8)
      appendU8(static_cast<std::uint8_t>(value >> shift));
  }

  void appendString(std::string_view value)
  {
    appendU64(static_cast<std::uint64_t>(value.size()));
    _bytes.insert(_bytes.end(), value.begin(), value.end());
  }

  void appendFiniteDouble(double value)
  {
    if (!std::isfinite(value))
      throw std::invalid_argument("independent reference-input wire requires a finite double");
    appendU8(0);
    appendU64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
  }

  template <typename Enum>
  void appendEnum(Enum value)
  {
    static_assert(std::is_enum_v<Enum>);
    appendU8(static_cast<std::uint8_t>(value));
  }

  template <typename T, typename Function>
  void appendOptional(const std::optional<T>& value, Function append)
  {
    appendBool(value.has_value());
    if (value)
      append(*this, *value);
  }

  template <typename T, typename Function>
  void appendVector(const std::vector<T>& values, Function append)
  {
    appendU64(static_cast<std::uint64_t>(values.size()));
    for (const T& value : values)
      append(*this, value);
  }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return _bytes; }

 private:
  std::vector<std::uint8_t> _bytes;
};

void appendSnapshotIdentity(Writer& writer, const CsrSnapshotIdentity& identity);
void appendNodeKey(Writer& writer, const NodeKey& key);
void appendLibraryArcSignature(Writer& writer, const LibraryArcSignature& signature);
void appendArcKey(Writer& writer, const ArcKey& key);
void appendTransfer(Writer& writer, const CellTransferModel& transfer);
void appendNode(Writer& writer, const NodeRecord& node);
void appendNodeLookup(Writer& writer, const NodeIdLookupEntry& entry);
void appendInstance(Writer& writer, const InstanceRecord& instance);
void appendArc(Writer& writer, const ArcRecord& arc);
void appendArcLookup(Writer& writer, const ArcIdLookupEntry& entry);

void appendClock(Writer& writer, const ClockConstraintModel& clock);
void appendPortConstraint(Writer& writer, const PortConstraintModel& port);
void appendCaseConstraint(Writer& writer, const CaseAnalysisConstraintModel& constraint);
void appendTimingCheckEndpoint(Writer& writer, const TimingCheckEndpointModel& endpoint);
void appendLibraryCell(Writer& writer, const ReferencedLibraryCellModel& cell);
void appendParasitics(Writer& writer, const ParasiticModel& parasitics);
void appendStartSeed(Writer& writer, const StartSeedModel& seed);
void appendEndpointRequired(Writer& writer, const EndpointRequiredModel& endpoint);
void appendPinCapacitance(Writer& writer, const CellPinCapacitanceModel& capacitance);

inline void appendNodeId(Writer& writer, NodeId id)
{
  writer.appendU64(id.value());
}

inline void appendArcId(Writer& writer, ArcId id)
{
  writer.appendU64(id.value());
}

inline void appendOptionalDouble(Writer& writer, const std::optional<double>& value)
{
  writer.appendOptional(value, [](Writer& local, double present) { local.appendFiniteDouble(present); });
}

}  // namespace ista::csr::test::wire
