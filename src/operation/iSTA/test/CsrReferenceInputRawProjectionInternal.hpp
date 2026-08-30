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

#include "CsrReferenceInputRawProjection.hpp"

namespace ista::csr::test::raw_wire {

class Writer
{
 public:
  void u8(std::uint8_t value) { _bytes.push_back(value); }
  void boolean(bool value) { u8(value ? 1 : 0); }
  void u64(std::uint64_t value)
  {
    for (int shift = 56; shift >= 0; shift -= 8)
      u8(static_cast<std::uint8_t>(value >> shift));
  }
  void i32(std::int32_t value)
  {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    for (int shift = 24; shift >= 0; shift -= 8)
      u8(static_cast<std::uint8_t>(bits >> shift));
  }
  void string(std::string_view value)
  {
    u64(static_cast<std::uint64_t>(value.size()));
    _bytes.insert(_bytes.end(), value.begin(), value.end());
  }
  void finite(double value)
  {
    if (!std::isfinite(value))
      throw std::invalid_argument("raw reference-input wire requires finite doubles");
    u8(0);
    u64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
  }
  template <typename Enum>
  void enumeration(Enum value)
  {
    static_assert(std::is_enum_v<Enum>);
    u8(static_cast<std::uint8_t>(value));
  }
  template <typename T, typename Function>
  void optional(const std::optional<T>& value, Function append)
  {
    boolean(value.has_value());
    if (value)
      append(*this, *value);
  }
  template <typename T, typename Function>
  void vector(const std::vector<T>& values, Function append)
  {
    u64(static_cast<std::uint64_t>(values.size()));
    for (const T& value : values)
      append(*this, value);
  }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return _bytes; }

 private:
  std::vector<std::uint8_t> _bytes;
};

void appendNode(Writer& writer, const RawNode& node);
void appendInstance(Writer& writer, const RawInstance& instance);
void appendArc(Writer& writer, const RawArc& arc);
void appendNet(Writer& writer, const RawNet& net);
void appendClock(Writer& writer, const RawClockConstraint& clock);
void appendPort(Writer& writer, const RawPortConstraint& port);
void appendCase(Writer& writer, const RawCaseConstraint& constraint);
void appendCheck(Writer& writer, const RawTimingCheck& check);
void appendLibraryCell(Writer& writer, const RawReferencedLibraryCell& cell);
void appendParasitics(Writer& writer, const RawParasitics& parasitics);

inline void optionalDouble(Writer& writer, const std::optional<double>& value)
{
  writer.optional(value, [](Writer& local, double present) { local.finite(present); });
}

}  // namespace ista::csr::test::raw_wire
