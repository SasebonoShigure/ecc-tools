// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You can use this software according to the terms and conditions of the Mulan PSL v2.
// You may obtain a copy of Mulan PSL v2 at:
// http://license.coscl.org.cn/MulanPSL2
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
//
// See the Mulan PSL v2 for more details.
// ***************************************************************************************
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CsrReferenceValidator.hpp"
#include "TimingSemanticKey.hpp"

namespace ista::csr {

enum class CsrTimingResultViolationClass : std::uint8_t
{
  kSchema,
  kSession,
  kGeneration,
  kSourceIdentity,
  kShape,
  kNumeric,
  kIdMapping,
  kPredecessor
};

struct CsrTimingResultContractViolation
{
  CsrTimingResultViolationClass classification;
  std::string field;
  std::string message;
};

namespace detail {

using Violations = std::vector<CsrContractViolation>;
using TimingResultViolations = std::vector<CsrTimingResultContractViolation>;

inline void reject(Violations& errors, std::string field, std::string message)
{
  errors.push_back({std::move(field), std::move(message)});
}

inline void reject(TimingResultViolations& errors, CsrTimingResultViolationClass classification, std::string field, std::string message)
{
  errors.push_back({classification, std::move(field), std::move(message)});
}

template <typename Enum>
bool knownEnum(Enum value)
{
  try {
    static_cast<void>(token(value));
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

inline bool isFinite(double value)
{
  return std::isfinite(value);
}

inline bool validArrival(double value)
{
  return !std::isnan(value) && value != std::numeric_limits<double>::infinity();
}

inline bool validRequired(double value)
{
  return !std::isnan(value) && value != -std::numeric_limits<double>::infinity();
}

inline bool byteStringLess(const std::string& lhs, const std::string& rhs)
{
  return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                                      [](char left, char right) { return static_cast<unsigned char>(left) < static_cast<unsigned char>(right); });
}

inline bool checkedProduct(std::size_t lhs, std::size_t rhs, std::size_t& product, Violations& errors, const std::string& field)
{
  if (rhs != 0 && lhs > std::numeric_limits<std::size_t>::max() / rhs) {
    reject(errors, field, "lane count overflows size_t");
    product = 0;
    return false;
  }
  product = lhs * rhs;
  return true;
}

inline bool checkedProduct(std::size_t lhs, std::size_t rhs, std::size_t& product, TimingResultViolations& errors, CsrTimingResultViolationClass classification,
                           const std::string& field)
{
  if (rhs != 0 && lhs > std::numeric_limits<std::size_t>::max() / rhs) {
    reject(errors, classification, field, "lane count overflows size_t");
    product = 0;
    return false;
  }
  product = lhs * rhs;
  return true;
}

inline bool validateOffsets(const std::vector<std::uint64_t>& offsets, std::size_t rows, std::size_t entries, Violations& errors, const std::string& field)
{
  if (rows == std::numeric_limits<std::size_t>::max() || offsets.size() != rows + 1) {
    reject(errors, field, "offset count must equal row count plus one");
    return false;
  }
  if (offsets.front() != 0)
    reject(errors, field, "offsets must start at zero");
  if (!std::ranges::is_sorted(offsets))
    reject(errors, field, "offsets must be monotonic");
  if (offsets.back() != entries)
    reject(errors, field, "final offset must equal entry count");
  return offsets.front() == 0 && std::ranges::is_sorted(offsets) && offsets.back() == entries;
}

inline bool validateOffsets(const std::vector<std::uint64_t>& offsets, std::size_t rows, std::size_t entries, TimingResultViolations& errors,
                            CsrTimingResultViolationClass classification, const std::string& field)
{
  if (rows == std::numeric_limits<std::size_t>::max() || offsets.size() != rows + 1) {
    reject(errors, classification, field, "offset count must equal row count plus one");
    return false;
  }
  if (offsets.front() != 0)
    reject(errors, classification, field, "offsets must start at zero");
  if (!std::ranges::is_sorted(offsets))
    reject(errors, classification, field, "offsets must be monotonic");
  if (offsets.back() != entries)
    reject(errors, classification, field, "final offset must equal entry count");
  return offsets.front() == 0 && std::ranges::is_sorted(offsets) && offsets.back() == entries;
}

template <typename T>
bool canonicalAbsent(const T& value)
{
  return value == T{};
}

inline bool canonicalAbsent(double value)
{
  return std::bit_cast<std::uint64_t>(value) == 0;
}

inline bool canonicalAbsent(Transition value)
{
  return value == Transition::kNone;
}

template <typename T, typename PresentValidator>
bool validateOptionalSoa(const OptionalSoa<T>& values, std::size_t expected, bool required, TimingResultViolations& errors,
                         CsrTimingResultViolationClass classification, const std::string& field, PresentValidator validate_present)
{
  if (values.presence.size() != expected || values.values.size() != expected) {
    reject(errors, classification, field, "presence and value arrays must match the exact lane count");
    return false;
  }
  bool valid = true;
  for (std::size_t index = 0; index < expected; ++index) {
    const std::uint8_t presence = values.presence[index];
    if (presence > 1) {
      reject(errors, classification, field + ".presence", "presence must be zero or one");
      valid = false;
    } else if (presence == 0) {
      if (required) {
        reject(errors, classification, field + ".presence", "every lane requires presence");
        valid = false;
      }
      if (!canonicalAbsent(values.values[index])) {
        reject(errors, classification, field + ".values", "absent lane payload is not the canonical default");
        valid = false;
      }
    } else {
      validate_present(values.values[index], index);
    }
  }
  return valid;
}

inline void validateExactNodeSet(const std::vector<NodeId>& values, const std::vector<std::uint8_t>& expected, Violations& errors, const std::string& field)
{
  std::vector<std::uint8_t> actual(expected.size(), 0);
  for (NodeId id : values) {
    if (id.value() >= expected.size()) {
      reject(errors, field, "NodeId is out of range");
    } else if (actual[id.value()] != 0) {
      reject(errors, field, "NodeId is duplicated");
    } else {
      actual[id.value()] = 1;
    }
  }
  if (actual != expected)
    reject(errors, field, "NodeId set does not match the frozen role");
}

inline bool validateAdjacency(const std::vector<std::uint64_t>& offsets, const std::vector<ArcId>& ids, const std::vector<ArcRecord>& arcs,
                              std::size_t node_count, bool outgoing, Violations& errors, const std::string& field)
{
  bool shape_valid = ids.size() == arcs.size();
  if (!shape_valid)
    reject(errors, field, "adjacency must contain exactly E entries");
  shape_valid = validateOffsets(offsets, node_count, arcs.size(), errors, field + ".row_offsets") && shape_valid;
  if (!shape_valid)
    return false;
  std::vector<std::uint8_t> seen(arcs.size(), 0);
  for (std::size_t row = 0; row < node_count; ++row) {
    for (std::uint64_t offset = offsets[row]; offset < offsets[row + 1]; ++offset) {
      const std::uint64_t arc_id = ids[offset].value();
      if (arc_id >= arcs.size()) {
        reject(errors, field, "ArcId is out of range");
        continue;
      }
      if (seen[arc_id]++ != 0)
        reject(errors, field, "ArcId is duplicated");
      const auto& arc = arcs[arc_id];
      if ((outgoing ? arc.source : arc.sink).value() != row)
        reject(errors, field, "ArcId is stored under the wrong endpoint row");
      // The row defines candidate order. Native physical slots can remain
      // stable across edits and need not increase in that order.
    }
  }
  if (std::ranges::any_of(seen, [](std::uint8_t count) { return count != 1; }))
    reject(errors, field, "adjacency must contain every ArcId exactly once");
  return true;
}

inline void appendViolations(Violations& destination, Violations source)
{
  destination.insert(destination.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
}

}  // namespace detail
}  // namespace ista::csr
