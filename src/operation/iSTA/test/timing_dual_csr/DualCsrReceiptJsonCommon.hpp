// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
// Licensed under Mulan PSL v2.
// ***************************************************************************************
#pragma once

#include <optional>
#include <vector>

#include "../timing_edit/TimingEditSemanticJson.hpp"
#include "DualCsrFunctionalProjection.hpp"
#include "json.hpp"

namespace ista::timing_dual_csr::test::json_detail {

using Json = nlohmann::json;

using timing_edit::test::semantic_json::arcKey;
using timing_edit::test::semantic_json::arrayOf;
using timing_edit::test::semantic_json::librarySignature;
using timing_edit::test::semantic_json::nodeKey;
using timing_edit::test::semantic_json::tableSignature;

template <typename Range>
Json scalarArray(const Range& values)
{
  return arrayOf(values, [](const auto& value) { return Json(value); });
}

template <typename T, typename Convert>
Json optionalValue(const std::optional<T>& value, Convert convert)
{
  return value ? convert(*value) : Json(nullptr);
}

template <typename T, typename Convert>
Json optionalSoa(const csr::OptionalSoa<T>& value, Convert convert)
{
  return {{"presence", scalarArray(value.presence)},
          {"values", arrayOf(value.values, convert)}};
}

[[nodiscard]] Json number(double value);
[[nodiscard]] Json laneMatrix(const csr::OptionalLaneMatrix& value);
[[nodiscard]] Json loadMatrix(const csr::ArcOutputLoadMatrix& value);
[[nodiscard]] Json transfer(const csr::CellTransferModel& value);
[[nodiscard]] Json libraryCell(
    const csr::ReferencedLibraryCellModel& value);
[[nodiscard]] Json parasiticNet(const csr::ParasiticNetModel& value);
[[nodiscard]] Json parasitics(const csr::ParasiticModel& value);

inline Json optionalNumber(const std::optional<double>& value)
{
  return optionalValue(value, [](double present) { return number(present); });
}

inline Json optionalNodeKey(const std::optional<csr::NodeKey>& value)
{
  return optionalValue(value,
                       [](const csr::NodeKey& present) {
                         return nodeKey(present);
                       });
}

inline Json optionalText(const std::optional<std::string>& value)
{
  return optionalValue(value,
                       [](const std::string& present) {
                         return Json(present);
                       });
}

}  // namespace ista::timing_dual_csr::test::json_detail
