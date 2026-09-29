// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#include "NativeDualCsrArcBijectionInternal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>

#include "TimingSemanticKey.hpp"
#include <vector>

namespace ista::timing_dual_csr::detail {
namespace {

constexpr std::size_t kUnassigned = std::numeric_limits<std::size_t>::max();

}  // namespace

std::variant<NativeArcBijection, NarrowCaptureError>
matchNativeAffectedArcs(
    std::span<const AffectedArcCapture> requests,
    std::span<const csr::ArcKey> candidate_keys,
    std::span<const csr::ArcId> request_ids,
    const NativeArcCompatibility& compatible,
    const NativeArcSemanticEquivalence& semantically_equivalent)
{
  if (requests.size() != request_ids.size())
    return NarrowCaptureError::kInvalidHighWater;

  NativeArcBijection result;
  result.candidate_by_request.resize(requests.size());
  std::vector<bool> globally_assigned(candidate_keys.size(), false);
  struct Group
  {
    std::vector<std::size_t> rows;
    std::vector<std::size_t> columns;
  };
  std::vector<Group> groups;
  std::map<csr::ArcKeyBase, std::size_t, decltype(&csr::arcKeyBaseLess)>
      by_base(csr::arcKeyBaseLess);
  // Preserve first-request group order and native candidate order within a group.
  for (std::size_t row = 0; row < requests.size(); ++row) {
    const auto& request = requests[row];
    if (csr::validateArcKeyBase(request.expected_post_base))
      return NarrowCaptureError::kArcOrdinalMismatch;
    auto [group, inserted] = by_base.try_emplace(request.expected_post_base, groups.size());
    if (inserted)
      groups.emplace_back();
    if (request.expected_post_multiplicity != 0)
      groups[group->second].rows.push_back(row);
  }
  for (std::size_t column = 0; column < candidate_keys.size(); ++column) {
    const auto& base = candidate_keys[column].base;
    if (csr::validateArcKeyBase(base))
      return NarrowCaptureError::kArcOrdinalMismatch;
    const auto group = by_base.find(base);
    if (group != by_base.end())
      groups[group->second].columns.push_back(column);
  }

  for (const auto& group : groups) {
    const auto& rows = group.rows;
    const auto& columns = group.columns;
    if (rows.size() != columns.size())
      return NarrowCaptureError::kArcMultiplicityMismatch;
    if (rows.empty())
      continue;
    for (std::size_t request : rows)
      if (requests[request].expected_post_multiplicity != rows.size())
        return NarrowCaptureError::kArcMultiplicityMismatch;

    std::vector<std::size_t> row_match(rows.size(), kUnassigned);
    std::vector<std::size_t> column_match(columns.size(), kUnassigned);
    const auto assign = [&](std::size_t row, std::size_t column) {
      if (row_match[row] != kUnassigned
          || column_match[column] != kUnassigned
          || globally_assigned[columns[column]])
        return false;
      row_match[row] = column;
      column_match[column] = row;
      globally_assigned[columns[column]] = true;
      return true;
    };

    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t row = 0; row < rows.size(); ++row) {
        if (row_match[row] != kUnassigned)
          continue;
        std::size_t only = kUnassigned;
        std::size_t count = 0;
        for (std::size_t column = 0; column < columns.size(); ++column) {
          if (column_match[column] == kUnassigned
              && compatible(rows[row], columns[column])) {
            only = column;
            ++count;
          }
        }
        if (count == 0)
          return NarrowCaptureError::kUnplannedDependency;
        if (count == 1) {
          if (!assign(row, only))
            return NarrowCaptureError::kUnplannedDependency;
          changed = true;
        }
      }
      for (std::size_t column = 0; column < columns.size(); ++column) {
        if (column_match[column] != kUnassigned)
          continue;
        std::size_t only = kUnassigned;
        std::size_t count = 0;
        for (std::size_t row = 0; row < rows.size(); ++row) {
          if (row_match[row] == kUnassigned
              && compatible(rows[row], columns[column])) {
            only = row;
            ++count;
          }
        }
        if (count == 0)
          return NarrowCaptureError::kUnplannedDependency;
        if (count == 1) {
          if (!assign(only, column))
            return NarrowCaptureError::kUnplannedDependency;
          changed = true;
        }
      }
    }

    std::vector<std::size_t> residual_rows;
    std::vector<std::size_t> residual_columns;
    for (std::size_t row = 0; row < rows.size(); ++row)
      if (row_match[row] == kUnassigned)
        residual_rows.push_back(row);
    for (std::size_t column = 0; column < columns.size(); ++column)
      if (column_match[column] == kUnassigned)
        residual_columns.push_back(column);
    if (residual_rows.size() != residual_columns.size())
      return NarrowCaptureError::kUnplannedDependency;

    if (!residual_rows.empty()) {
      for (std::size_t row : residual_rows)
        for (std::size_t column : residual_columns)
          if (!compatible(rows[row], columns[column]))
            return NarrowCaptureError::kUnplannedDependency;
      for (std::size_t index = 1; index < residual_columns.size(); ++index)
        if (!semantically_equivalent(
                columns[residual_columns.front()],
                columns[residual_columns[index]]))
          return NarrowCaptureError::kMalformedNativeRecord;

      std::ranges::sort(residual_rows, [&](std::size_t lhs,
                                           std::size_t rhs) {
        const auto lhs_id = request_ids[rows[lhs]].value();
        const auto rhs_id = request_ids[rows[rhs]].value();
        if (lhs_id != rhs_id)
          return lhs_id < rhs_id;
        return requests[rows[lhs]].ref < requests[rows[rhs]].ref;
      });
      for (std::size_t index = 0; index < residual_rows.size(); ++index)
        if (!assign(residual_rows[index], residual_columns[index]))
          return NarrowCaptureError::kUnplannedDependency;
      result.identity_tie_breaks += residual_rows.size();
    }

    for (std::size_t row = 0; row < rows.size(); ++row) {
      if (row_match[row] == kUnassigned)
        return NarrowCaptureError::kUnplannedDependency;
      result.candidate_by_request[rows[row]]
          = columns[row_match[row]];
    }
  }

  for (std::size_t request = 0; request < requests.size(); ++request)
    if ((requests[request].expected_post_multiplicity != 0)
        != result.candidate_by_request[request].has_value())
      return NarrowCaptureError::kArcMultiplicityMismatch;
  return result;
}

}  // namespace ista::timing_dual_csr::detail
