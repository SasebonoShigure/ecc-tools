// ***************************************************************************************
// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
// Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
//
// iEDA is licensed under Mulan PSL v2.
// You may use this software according to the terms and conditions of the Mulan PSL v2.
// ***************************************************************************************
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "CsrCpuReferenceSlowOracleInternal.hpp"

namespace ista::csr::test::detail {
namespace {

struct Region
{
  std::size_t first{0};
  std::size_t second{0};
};

bool finite(double value)
{
  return std::isfinite(value);
}

SlowOracleTableValue numericFailure()
{
  return {true, SlowOracleStatus::kNumeric, 0.0};
}

Region regionFor(const std::vector<double>& axis, double query)
{
  if (axis.size() < 2)
    return {};
  std::size_t upper = 0;
  while (upper < axis.size() && !(axis[upper] > query))
    ++upper;
  if (upper == 0)
    return {0, 1};
  if (upper == axis.size())
    return {axis.size() - 2, axis.size() - 1};
  return {upper - 1, upper};
}

double tableValue(const TimingTableModel& table, std::size_t first, std::size_t second)
{
  if (table.axes.size() < 2 || second >= table.axes[1].size())
    return 0.0;
  const std::size_t index = first * table.axes[1].size() + second;
  return index < table.values.size() ? table.values[index] : 0.0;
}

SlowOracleTableValue linear(double x1, double x2, double y1, double y2, double x)
{
  const double delta = x2 - x1;
  if (!finite(delta))
    return numericFailure();
  if (std::fabs(delta) < 1e-6)
    return {true, SlowOracleStatus::kSuccess, y1};
  const double x_offset = x - x1;
  if (!finite(x_offset))
    return numericFailure();
  const double y_delta = y2 - y1;
  if (!finite(y_delta))
    return numericFailure();
  const double product = x_offset * y_delta;
  if (!finite(product))
    return numericFailure();
  const double quotient = product / delta;
  if (!finite(quotient))
    return numericFailure();
  const double value = y1 + quotient;
  return finite(value) ? SlowOracleTableValue{true, SlowOracleStatus::kSuccess, value} : numericFailure();
}

SlowOracleTableValue findValue(const TimingTableModel& table, double scaled_slew, double converted_load)
{
  if (table.values.empty())
    return {true, SlowOracleStatus::kSuccess, 0.0};
  if (table.axes.empty())
    return {true, SlowOracleStatus::kSuccess, table.values.front()};

  const bool load_first = table.variable1 == TableVariable::kOutputCapacitance || table.variable1 == TableVariable::kConstrainedTransition;
  const double x = load_first ? converted_load : scaled_slew;
  const double y = load_first ? scaled_slew : converted_load;
  const Region x_region = regionFor(table.axes[0], x);
  if (table.axes.size() == 1) {
    const double y1 = x_region.first < table.values.size() ? table.values[x_region.first] : 0.0;
    const double y2 = x_region.second < table.values.size() ? table.values[x_region.second] : 0.0;
    return linear(table.axes[0][x_region.first], table.axes[0][x_region.second], y1, y2, x);
  }

  const Region y_region = regionFor(table.axes[1], y);
  const double x1 = table.axes[0][x_region.first];
  const double x2 = table.axes[0][x_region.second];
  const double y1 = table.axes[1][y_region.first];
  const double y2 = table.axes[1][y_region.second];
  const double q11 = tableValue(table, x_region.first, y_region.first);
  const double q12 = tableValue(table, x_region.first, y_region.second);
  const double q21 = tableValue(table, x_region.second, y_region.first);
  const double q22 = tableValue(table, x_region.second, y_region.second);

  const double x2_x1 = x2 - x1;
  const double y2_y1 = y2 - y1;
  if (!finite(x2_x1) || !finite(y2_y1))
    return numericFailure();
  if (std::fabs(x2_x1) < 1e-6)
    return linear(y1, y2, q11, q12, y);
  if (std::fabs(y2_y1) < 1e-6)
    return linear(x1, x2, q11, q21, x);

  const double x2_x = x2 - x;
  const double y2_y = y2 - y;
  const double x_x1 = x - x1;
  const double y_y1 = y - y1;
  if (!finite(x2_x) || !finite(y2_y) || !finite(x_x1) || !finite(y_y1))
    return numericFailure();

  const double term11 = q11 * x2_x * y2_y;
  const double term21 = q21 * x_x1 * y2_y;
  const double term12 = q12 * x2_x * y_y1;
  const double term22 = q22 * x_x1 * y_y1;
  if (!finite(term11) || !finite(term21) || !finite(term12) || !finite(term22))
    return numericFailure();
  const double numerator12 = term11 + term21;
  const double numerator123 = numerator12 + term12;
  const double numerator = numerator123 + term22;
  const double denominator = x2_x1 * y2_y1;
  if (!finite(numerator12) || !finite(numerator123) || !finite(numerator) || !finite(denominator) || denominator == 0.0)
    return numericFailure();
  const double value = numerator / denominator;
  return finite(value) ? SlowOracleTableValue{true, SlowOracleStatus::kSuccess, value} : numericFailure();
}

}  // namespace

SlowOracleTableValue slowOracleLookupTable(const TimingTableModel& table, double input_slew, double raw_output_load, double time_unit_scale,
                                           double capacitance_unit_scale)
{
  if (!table.present)
    return {};
  if (!finite(input_slew) || input_slew < 0.0 || !finite(raw_output_load) || raw_output_load < 0.0 || !finite(time_unit_scale) || time_unit_scale <= 0.0
      || !finite(capacitance_unit_scale) || capacitance_unit_scale <= 0.0)
    return numericFailure();

  const double cap_delta = capacitance_unit_scale - 1.0;
  if (!finite(cap_delta))
    return numericFailure();
  double converted_load = raw_output_load;
  if (!(std::fabs(cap_delta) < 1e-6)) {
    const double product = raw_output_load * capacitance_unit_scale;
    if (!finite(product) || product < 0.0 || product > static_cast<double>(std::numeric_limits<int>::max()))
      return numericFailure();
    const double rounded = std::ceil(product);
    if (!finite(rounded) || rounded < 0.0 || rounded > static_cast<double>(std::numeric_limits<int>::max()))
      return numericFailure();
    converted_load = static_cast<double>(static_cast<int>(rounded));
  }

  const double scaled_slew = input_slew * time_unit_scale;
  if (!finite(scaled_slew))
    return numericFailure();
  SlowOracleTableValue value = findValue(table, scaled_slew, converted_load);
  if (value.status != SlowOracleStatus::kSuccess)
    return value;
  const double unscaled = value.value / time_unit_scale;
  if (!finite(unscaled))
    return numericFailure();
  value.value = unscaled;
  return value;
}

}  // namespace ista::csr::test::detail
