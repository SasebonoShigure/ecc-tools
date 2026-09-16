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
#include "CsrTableLookup.hpp"

#include <cmath>
#include <cstddef>
#include <limits>

namespace ista::csr {
namespace {

constexpr double kStaError = 1e-6;

struct AxisRegion
{
  double lower{0.0};
  double upper{0.0};
  std::size_t index{0};
};

std::optional<AxisRegion> findAxisRegion(const std::vector<double>& axis, double query)
{
  const std::size_t size = axis.size();
  if (size < 2)
    return AxisRegion{};

  double upper = 0.0;
  std::size_t index = 0;
  for (; index < size; ++index) {
    upper = axis[index];
    if (!std::isfinite(upper))
      return std::nullopt;
    if (upper > query)
      break;
  }

  if (index == size) {
    index = size - 2;
  } else if (index > 0) {
    --index;
  } else {
    upper = axis[1];
    if (!std::isfinite(upper))
      return std::nullopt;
  }
  const double lower = axis[index];
  if (!std::isfinite(lower))
    return std::nullopt;
  return AxisRegion{lower, upper, index};
}

double tableValue(const TimingTableModel& table, std::size_t index)
{
  return index < table.values.size() ? table.values[index] : 0.0;
}

std::optional<std::size_t> tableIndex(std::size_t row, std::size_t column, std::size_t width)
{
  constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
  if (width != 0 && row > (kMax - column) / width)
    return std::nullopt;
  return row * width + column;
}

std::optional<double> linearInterpolate(double x1, double x2, double y1, double y2, double x)
{
  const double x2_x1 = x2 - x1;
  if (!std::isfinite(x2_x1))
    return std::nullopt;
  const double absolute_delta = std::abs(x2_x1);
  if (!std::isfinite(absolute_delta))
    return std::nullopt;
  if (absolute_delta < kStaError)
    return std::isfinite(y1) ? std::optional<double>{y1} : std::nullopt;

  const double x_x1 = x - x1;
  if (!std::isfinite(x_x1))
    return std::nullopt;
  const double y2_y1 = y2 - y1;
  if (!std::isfinite(y2_y1))
    return std::nullopt;
  const double product = x_x1 * y2_y1;
  if (!std::isfinite(product))
    return std::nullopt;
  const double quotient = product / x2_x1;
  if (!std::isfinite(quotient))
    return std::nullopt;
  const double result = y1 + quotient;
  return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
}

std::optional<double> bilinearInterpolate(double q11, double q12, double q21, double q22, double x1, double x2, double y1, double y2, double x, double y)
{
  const double x_delta = x2 - x1;
  if (!std::isfinite(x_delta))
    return std::nullopt;
  if (std::abs(x_delta) < kStaError)
    return linearInterpolate(y1, y2, q11, q12, y);

  const double y_delta = y2 - y1;
  if (!std::isfinite(y_delta))
    return std::nullopt;
  if (std::abs(y_delta) < kStaError)
    return linearInterpolate(x1, x2, q11, q21, x);

  const double x2_x1 = x2 - x1;
  const double y2_y1 = y2 - y1;
  const double x2_x = x2 - x;
  const double y2_y = y2 - y;
  const double x_x1 = x - x1;
  const double y_y1 = y - y1;
  if (!std::isfinite(x2_x1) || !std::isfinite(y2_y1) || !std::isfinite(x2_x) || !std::isfinite(y2_y) || !std::isfinite(x_x1) || !std::isfinite(y_y1))
    return std::nullopt;

  double numerator = q11 * x2_x;
  if (!std::isfinite(numerator))
    return std::nullopt;
  numerator *= y2_y;
  if (!std::isfinite(numerator))
    return std::nullopt;
  double term = q21 * x_x1;
  if (!std::isfinite(term))
    return std::nullopt;
  term *= y2_y;
  if (!std::isfinite(term))
    return std::nullopt;
  numerator += term;
  if (!std::isfinite(numerator))
    return std::nullopt;
  term = q12 * x2_x;
  if (!std::isfinite(term))
    return std::nullopt;
  term *= y_y1;
  if (!std::isfinite(term))
    return std::nullopt;
  numerator += term;
  if (!std::isfinite(numerator))
    return std::nullopt;
  term = q22 * x_x1;
  if (!std::isfinite(term))
    return std::nullopt;
  term *= y_y1;
  if (!std::isfinite(term))
    return std::nullopt;
  numerator += term;
  if (!std::isfinite(numerator))
    return std::nullopt;
  const double denominator = x2_x1 * y2_y1;
  if (!std::isfinite(denominator))
    return std::nullopt;
  const double result = numerator / denominator;
  return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
}

std::optional<double> evaluateTable(const TimingTableModel& table, double slew, double secondary)
{
  if (table.values.empty())
    return 0.0;
  if (table.axes.empty())
    return std::isfinite(table.values.front()) ? std::optional<double>{table.values.front()} : std::nullopt;

  double value1 = slew;
  double value2 = secondary;
  if (table.variable1 == TableVariable::kOutputCapacitance || table.variable1 == TableVariable::kConstrainedTransition) {
    value1 = secondary;
    value2 = slew;
  }

  const auto region1 = findAxisRegion(table.axes[0], value1);
  if (!region1)
    return std::nullopt;
  if (table.axes.size() == 1)
    return linearInterpolate(region1->lower, region1->upper, tableValue(table, region1->index), tableValue(table, region1->index + 1), value1);

  const auto region2 = findAxisRegion(table.axes[1], value2);
  if (!region2)
    return std::nullopt;
  const std::size_t width = table.axes[1].size();
  const auto q11_index = tableIndex(region1->index, region2->index, width);
  const auto q21_index = tableIndex(region1->index + 1, region2->index, width);
  const auto q12_index = tableIndex(region1->index, region2->index + 1, width);
  const auto q22_index = tableIndex(region1->index + 1, region2->index + 1, width);
  if (!q11_index || !q21_index || !q12_index || !q22_index)
    return std::nullopt;

  const double q11 = tableValue(table, *q11_index);
  const double q12 = tableValue(table, *q12_index);
  const double q21 = tableValue(table, *q21_index);
  const double q22 = tableValue(table, *q22_index);
  return bilinearInterpolate(q11, q12, q21, q22, region1->lower, region1->upper, region2->lower, region2->upper, value1, value2);
}

}  // namespace

std::optional<double> lookupCsrTimingTable(const TimingTableModel& table, double input_slew, double raw_output_load, double time_unit_scale,
                                           double capacitance_unit_scale)
{
  if (!table.present)
    return std::nullopt;
  if (!std::isfinite(input_slew) || input_slew < 0.0 || !std::isfinite(raw_output_load) || raw_output_load < 0.0 || !std::isfinite(time_unit_scale)
      || time_unit_scale <= 0.0 || !std::isfinite(capacitance_unit_scale) || capacitance_unit_scale <= 0.0)
    return std::nullopt;

  const double capacitance_delta = capacitance_unit_scale - 1.0;
  if (!std::isfinite(capacitance_delta))
    return std::nullopt;
  const double absolute_delta = std::abs(capacitance_delta);
  if (!std::isfinite(absolute_delta))
    return std::nullopt;

  double converted_output_load = raw_output_load;
  if (!(absolute_delta < kStaError)) {
    const double product = raw_output_load * capacitance_unit_scale;
    if (!std::isfinite(product) || product < 0.0 || product > static_cast<double>(std::numeric_limits<int>::max()))
      return std::nullopt;
    const double rounded = std::ceil(product);
    if (!std::isfinite(rounded) || rounded < 0.0 || rounded > static_cast<double>(std::numeric_limits<int>::max()))
      return std::nullopt;
    converted_output_load = static_cast<int>(rounded);
  }

  const double converted_input_slew = input_slew * time_unit_scale;
  if (!std::isfinite(converted_input_slew) || converted_input_slew < 0.0)
    return std::nullopt;
  const auto table_value = evaluateTable(table, converted_input_slew, converted_output_load);
  if (!table_value)
    return std::nullopt;
  const double result = *table_value / time_unit_scale;
  return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
}

std::optional<double> lookupCsrTimingCheckTable(const TimingTableModel& table, double clock_slew, double data_slew, double time_unit_scale)
{
  if (!table.present || !std::isfinite(clock_slew) || clock_slew < 0.0 || !std::isfinite(data_slew) || data_slew < 0.0
      || !std::isfinite(time_unit_scale) || time_unit_scale <= 0.0)
    return std::nullopt;
  const double clock = clock_slew * time_unit_scale;
  const double data = data_slew * time_unit_scale;
  if (!std::isfinite(clock) || !std::isfinite(data))
    return std::nullopt;
  const auto value = evaluateTable(table, clock, data);
  if (!value)
    return std::nullopt;
  const double result = *value / time_unit_scale;
  return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
}

}  // namespace ista::csr
