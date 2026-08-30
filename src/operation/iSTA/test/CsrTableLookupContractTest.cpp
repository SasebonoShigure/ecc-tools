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
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CsrTableLookup.hpp"

namespace {

using namespace ista::csr;

void require(bool condition, const std::string& message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void requireNull(const std::optional<double>& actual, const std::string& name)
{
  require(!actual.has_value(), name + " unexpectedly returned a value");
}

void requireBits(const std::optional<double>& actual, std::uint64_t expected, const std::string& name)
{
  require(actual.has_value(), name + " unexpectedly returned nullopt");
  require(std::bit_cast<std::uint64_t>(*actual) == expected, name + " returned the wrong binary64 value");
}

void requireValue(const std::optional<double>& actual, double expected, const std::string& name)
{
  requireBits(actual, std::bit_cast<std::uint64_t>(expected), name);
}

TimingTableModel emptyTable()
{
  return TimingTableModel{true, TableVariable::kNone, TableVariable::kNone, {}, {}};
}

TimingTableModel oneDimensional(TableVariable variable, std::vector<double> axis, std::vector<double> values)
{
  return TimingTableModel{true, variable, TableVariable::kNone, {std::move(axis)}, std::move(values)};
}

void testPresenceGuardsAndUnits()
{
  const double infinity = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const TimingTableModel absent;
  requireNull(lookupCsrTimingTable(absent, nan, nan, nan, nan), "absent early return");

  const TimingTableModel empty = emptyTable();
  requireBits(lookupCsrTimingTable(empty, 0.0, 0.0, 1.0, 1.0), 0, "present-empty positive zero");
  requireNull(lookupCsrTimingTable(empty, -1.0, 0.0, 1.0, 1.0), "negative input slew");
  requireNull(lookupCsrTimingTable(empty, infinity, 0.0, 1.0, 1.0), "infinite input slew");
  requireNull(lookupCsrTimingTable(empty, nan, 0.0, 1.0, 1.0), "NaN input slew");
  requireNull(lookupCsrTimingTable(empty, 0.0, -1.0, 1.0, 1.0), "negative output load");
  requireNull(lookupCsrTimingTable(empty, 0.0, infinity, 1.0, 1.0), "infinite output load");
  requireNull(lookupCsrTimingTable(empty, 0.0, 0.0, 0.0, 1.0), "zero time scale");
  requireNull(lookupCsrTimingTable(empty, 0.0, 0.0, infinity, 1.0), "infinite time scale");
  requireNull(lookupCsrTimingTable(empty, 0.0, 0.0, 1.0, 0.0), "zero capacitance scale");
  requireNull(lookupCsrTimingTable(empty, 0.0, 0.0, 1.0, nan), "NaN capacitance scale");
  requireNull(lookupCsrTimingTable(empty, std::numeric_limits<double>::max(), 0.0, 2.0, 1.0), "scaled slew overflow");

  const TimingTableModel load_table = oneDimensional(TableVariable::kOutputCapacitance, {0.0, 2.0}, {0.0, 2.0});
  const double rounded_below = 1.0 + 1e-6;
  const double rounded_above = std::nextafter(rounded_below, infinity);
  requireValue(lookupCsrTimingTable(load_table, 0.0, 0.5, 1.0, rounded_below), 0.5, "near-one direct load");
  requireValue(lookupCsrTimingTable(load_table, 0.0, 0.5, 1.0, rounded_above), 1.0, "near-one converted load");
  requireValue(lookupCsrTimingTable(load_table, 0.0, 0.5, 1.0, 2.0), 1.0, "integer ceil conversion");
  requireValue(lookupCsrTimingTable(load_table, 0.0, 0.6, 1.0, 2.0), 2.0, "noninteger ceil conversion");

  const double int_max = static_cast<double>(std::numeric_limits<int>::max());
  const TimingTableModel int_table = oneDimensional(TableVariable::kOutputCapacitance, {0.0, int_max}, {0.0, int_max});
  requireValue(lookupCsrTimingTable(int_table, 0.0, int_max / 2.0, 1.0, 2.0), int_max, "INT_MAX conversion");
  requireNull(lookupCsrTimingTable(int_table, 0.0, std::nextafter(int_max / 2.0, infinity), 1.0, 2.0), "above INT_MAX conversion");
  requireNull(lookupCsrTimingTable(empty, 0.0, int_max, 1.0, 2.0), "present-empty unsafe capacitance conversion");

  const TimingTableModel slew_table = oneDimensional(TableVariable::kInputTransition, {0.0, 4.0}, {0.0, 8.0});
  requireValue(lookupCsrTimingTable(slew_table, 1.0, 0.0, 2.0, 1.0), 2.0, "time scale round trip");
  const TimingTableModel constant{true, TableVariable::kNone, TableVariable::kNone, {}, {0x1.8p+2}};
  requireValue(lookupCsrTimingTable(constant, 0.0, 0.0, 2.0, 1.0), 0x1.8p+1, "constant time scaling");
}

void testOneDimensionalLookup()
{
  const std::array variables{TableVariable::kInputTransition, TableVariable::kOutputCapacitance, TableVariable::kConstrainedTransition};
  for (TableVariable variable : variables) {
    const TimingTableModel table = oneDimensional(variable, {1.0, 2.0}, {10.0, 20.0});
    const double slew = variable == TableVariable::kInputTransition ? 1.5 : 0.0;
    const double load = variable == TableVariable::kInputTransition ? 0.0 : 1.5;
    requireValue(lookupCsrTimingTable(table, slew, load, 1.0, 1.0), 15.0, "valid 1D variable");
  }

  const TimingTableModel regions = oneDimensional(TableVariable::kInputTransition, {1.0, 2.0, 4.0}, {10.0, 20.0, 40.0});
  requireValue(lookupCsrTimingTable(regions, 0.0, 0.0, 1.0, 1.0), 0.0, "below-axis extrapolation");
  requireValue(lookupCsrTimingTable(regions, 1.0, 0.0, 1.0, 1.0), 10.0, "first knot");
  requireValue(lookupCsrTimingTable(regions, 2.0, 0.0, 1.0, 1.0), 20.0, "interior knot uses right interval");
  requireValue(lookupCsrTimingTable(regions, 4.0, 0.0, 1.0, 1.0), 40.0, "last knot");
  requireValue(lookupCsrTimingTable(regions, 5.0, 0.0, 1.0, 1.0), 50.0, "above-axis extrapolation");
  const double maximum = std::numeric_limits<double>::max();
  const TimingTableModel right_interval = oneDimensional(TableVariable::kInputTransition, {1.0, 2.0, 4.0}, {-maximum, maximum, maximum});
  requireValue(lookupCsrTimingTable(right_interval, 2.0, 0.0, 1.0, 1.0), maximum, "interior knot avoids overflowing left interval");

  const TimingTableModel singleton = oneDimensional(TableVariable::kInputTransition, {3.0}, {7.0});
  requireValue(lookupCsrTimingTable(singleton, 99.0, 0.0, 1.0, 1.0), 7.0, "singleton axis");
  const TimingTableModel out_of_bounds = oneDimensional(TableVariable::kInputTransition, {0.0, 1.0}, {2.0});
  requireValue(lookupCsrTimingTable(out_of_bounds, 1.0, 0.0, 1.0, 1.0), 0.0, "native OOB value fallback");

  const double below = std::nextafter(1e-6, 0.0);
  const double exact = 1e-6;
  const double above = std::nextafter(1e-6, std::numeric_limits<double>::infinity());
  requireValue(lookupCsrTimingTable(oneDimensional(TableVariable::kInputTransition, {0.0, below}, {3.0, 9.0}), below, 0.0, 1.0, 1.0), 3.0,
               "delta below epsilon");
  requireValue(lookupCsrTimingTable(oneDimensional(TableVariable::kInputTransition, {0.0, exact}, {3.0, 9.0}), exact, 0.0, 1.0, 1.0), 9.0,
               "delta equal epsilon");
  requireValue(lookupCsrTimingTable(oneDimensional(TableVariable::kInputTransition, {0.0, above}, {3.0, 9.0}), above, 0.0, 1.0, 1.0), 9.0,
               "delta above epsilon");
}

void testTwoDimensionalLookup()
{
  const std::array variables{TableVariable::kInputTransition, TableVariable::kOutputCapacitance, TableVariable::kConstrainedTransition};
  for (TableVariable variable1 : variables) {
    for (TableVariable variable2 : variables) {
      TimingTableModel table;
      table.present = true;
      table.variable1 = variable1;
      table.variable2 = variable2;
      if (variable1 == TableVariable::kInputTransition) {
        table.axes = {{0.0, 4.0}, {0.0, 8.0}};
        table.values = {2.0, 13.0, 7.0, 29.0};
      } else {
        table.axes = {{0.0, 8.0}, {0.0, 4.0}};
        table.values = {2.0, 7.0, 13.0, 29.0};
      }
      requireValue(lookupCsrTimingTable(table, 1.0, 6.0, 1.0, 1.0), 0x1.b2p+3, "2D orientation, axis0-major layout, and variable2 dispatch");
    }
  }

  const TimingTableModel extrapolation{
      true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{1.0, 3.0}, {2.0, 6.0}}, {3.0, 11.0, 7.0, 15.0}};
  requireValue(lookupCsrTimingTable(extrapolation, 0.0, 4.0, 1.0, 1.0), 5.0, "2D axis0 lower extrapolation");
  requireValue(lookupCsrTimingTable(extrapolation, 4.0, 4.0, 1.0, 1.0), 13.0, "2D axis0 upper extrapolation");
  requireValue(lookupCsrTimingTable(extrapolation, 2.0, 0.0, 1.0, 1.0), 1.0, "2D axis1 lower extrapolation");
  requireValue(lookupCsrTimingTable(extrapolation, 2.0, 8.0, 1.0, 1.0), 17.0, "2D axis1 upper extrapolation");
  requireValue(lookupCsrTimingTable(extrapolation, 0.0, 0.0, 1.0, 1.0), -3.0, "2D both axes lower extrapolation");
  requireValue(lookupCsrTimingTable(extrapolation, 4.0, 8.0, 1.0, 1.0), 21.0, "2D both axes upper extrapolation");

  const TimingTableModel x_singleton{true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{1.0}, {0.0, 2.0}}, {2.0, 6.0}};
  requireValue(lookupCsrTimingTable(x_singleton, 1.0, 1.0, 1.0, 1.0), 4.0, "axis0 singleton");
  const TimingTableModel y_singleton{true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.0, 2.0}, {1.0}}, {2.0, 6.0}};
  requireValue(lookupCsrTimingTable(y_singleton, 1.0, 1.0, 1.0, 1.0), 4.0, "axis1 singleton");
  const TimingTableModel both_singleton{true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{1.0}, {1.0}}, {9.0}};
  requireValue(lookupCsrTimingTable(both_singleton, 1.0, 1.0, 1.0, 1.0), 9.0, "both axes singleton");

  const double below = std::nextafter(1e-6, 0.0);
  const double exact = 1e-6;
  const double above = std::nextafter(1e-6, std::numeric_limits<double>::infinity());
  const TimingTableModel both_degenerate{
      true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.0, below}, {0.0, below}}, {1.0, 5.0, 3.0, 7.0}};
  requireValue(lookupCsrTimingTable(both_degenerate, below / 2.0, below / 2.0, 1.0, 1.0), 1.0, "x-degenerate priority");
  const TimingTableModel x_degenerate{
      true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.0, below}, {0.0, 2.0}}, {1.0, 5.0, 3.0, 7.0}};
  requireValue(lookupCsrTimingTable(x_degenerate, below / 2.0, 1.0, 1.0, 1.0), 3.0, "x-degenerate y interpolation");
  const TimingTableModel y_degenerate{
      true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.0, exact}, {0.0, below}}, {1.0, 5.0, 3.0, 7.0}};
  requireValue(lookupCsrTimingTable(y_degenerate, exact / 2.0, below / 2.0, 1.0, 1.0), 2.0, "equal-epsilon x before y branch");
  const TimingTableModel neither_degenerate{
      true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.0, exact}, {0.0, exact}}, {1.0, 5.0, 3.0, 7.0}};
  requireValue(lookupCsrTimingTable(neither_degenerate, exact / 2.0, exact / 2.0, 1.0, 1.0), 4.0, "equal-epsilon bilinear");

  const TimingTableModel x_above{true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.0, above}, {0.0, 2.0}}, {1.0, 4.0, 2.0, 8.0}};
  requireValue(lookupCsrTimingTable(x_above, above, 0.0, 1.0, 1.0), 2.0, "axis0 delta above epsilon");
  const TimingTableModel y_above{true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.0, 2.0}, {0.0, above}}, {1.0, 4.0, 2.0, 8.0}};
  requireValue(lookupCsrTimingTable(y_above, 0.0, above, 1.0, 1.0), 4.0, "axis1 delta above epsilon");
  const TimingTableModel both_above{
      true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.0, above}, {0.0, above}}, {1.0, 4.0, 2.0, 8.0}};
  requireValue(lookupCsrTimingTable(both_above, above, above, 1.0, 1.0), 8.0, "both axis deltas above epsilon");

  const TimingTableModel bit_canary{true,
                                    TableVariable::kInputTransition,
                                    TableVariable::kOutputCapacitance,
                                    {{0x1.2p-1, 0x1.9p+0}, {0x1.0p-2, 0x1.6p+0}},
                                    {0x1.3p+0, -0x1.1p-1, 0x1.7p+1, 0x1.05p+2}};
  requireBits(lookupCsrTimingTable(bit_canary, 0x1.1p+0, 0x1.2p-1, 1.0, 1.0), 0x3fff5aaaaaaaaaabULL, "bilinear expression canary");
}

void testNumericFailures()
{
  const double maximum = std::numeric_limits<double>::max();
  const double infinity = std::numeric_limits<double>::infinity();
  const TimingTableModel infinite_constant{true, TableVariable::kNone, TableVariable::kNone, {}, {infinity}};
  requireNull(lookupCsrTimingTable(infinite_constant, 0.0, 0.0, 1.0, 1.0), "nonfinite constant");
  const TimingTableModel huge_constant{true, TableVariable::kNone, TableVariable::kNone, {}, {maximum}};
  requireNull(lookupCsrTimingTable(huge_constant, 0.0, 0.0, std::numeric_limits<double>::denorm_min(), 1.0), "final division overflow");
  const TimingTableModel axis_overflow = oneDimensional(TableVariable::kInputTransition, {-maximum, maximum}, {0.0, 1.0});
  requireNull(lookupCsrTimingTable(axis_overflow, 0.0, 0.0, 1.0, 1.0), "axis delta overflow");
  const TimingTableModel value_overflow = oneDimensional(TableVariable::kInputTransition, {0.0, 1.0}, {-maximum, maximum});
  requireNull(lookupCsrTimingTable(value_overflow, 0.5, 0.0, 1.0, 1.0), "value delta overflow");
  const TimingTableModel product_overflow = oneDimensional(TableVariable::kInputTransition, {0.0, 1.0}, {0.0, maximum});
  requireNull(lookupCsrTimingTable(product_overflow, 2.0, 0.0, 1.0, 1.0), "linear product overflow");
  const TimingTableModel bilinear_overflow{
      true, TableVariable::kInputTransition, TableVariable::kOutputCapacitance, {{0.0, 2.0}, {0.0, 2.0}}, {maximum, maximum, maximum, maximum}};
  requireNull(lookupCsrTimingTable(bilinear_overflow, 1.0, 1.0, 1.0, 1.0), "bilinear intermediate overflow");
}

}  // namespace

int main()
{
  testPresenceGuardsAndUnits();
  testOneDimensionalLookup();
  testTwoDimensionalLookup();
  testNumericFailures();
  return 0;
}
