#pragma once

#include <climits>
#include <cmath>

#include "CsrCudaDeviceTypes.hpp"

namespace ista::csr::cuda_detail {

__device__ inline double numeric(DeviceView view)
{
  *view.status = Status::numeric;
  return 0.0;
}

__device__ inline double finiteValue(DeviceView view, double value)
{
  return isfinite(value) ? value : numeric(view);
}

struct AxisRegion
{
  double lower{0.0};
  double upper{0.0};
  Index index{0};
};

__device__ inline AxisRegion axisRegion(DeviceView view, const Table& table, unsigned axis, double query)
{
  AxisRegion region;
  const Index size = table.axis_size[axis];
  if (size < 2)
    return region;
  const double* values = view.table_data + table.axis_offset[axis];
  Index index = 0;
  for (; index < size; ++index) {
    region.upper = finiteValue(view, values[index]);
    if (region.upper > query)
      break;
  }
  if (index == size)
    index = size - 2;
  else if (index > 0)
    --index;
  else
    region.upper = finiteValue(view, values[1]);
  region.index = index;
  region.lower = finiteValue(view, values[index]);
  return region;
}

__device__ inline double sample(DeviceView view, const Table& table, Index index)
{
  return index < table.value_size ? view.table_data[table.value_offset + index] : 0.0;
}

__device__ inline double linear(DeviceView view, double x1, double x2, double y1, double y2, double x)
{
  const double delta = finiteValue(view, x2 - x1);
  if (fabs(delta) < 1e-6)
    return finiteValue(view, y1);
  const double offset = finiteValue(view, x - x1);
  const double rise = finiteValue(view, y2 - y1);
  const double product = finiteValue(view, offset * rise);
  const double quotient = finiteValue(view, product / delta);
  return finiteValue(view, y1 + quotient);
}

__device__ inline double bilinear(DeviceView view, double q11, double q12, double q21, double q22, const AxisRegion& a, const AxisRegion& b, double x, double y)
{
  const double dx = finiteValue(view, a.upper - a.lower);
  if (fabs(dx) < 1e-6)
    return linear(view, b.lower, b.upper, q11, q12, y);
  const double dy = finiteValue(view, b.upper - b.lower);
  if (fabs(dy) < 1e-6)
    return linear(view, a.lower, a.upper, q11, q21, x);
  const double right = finiteValue(view, a.upper - x);
  const double top = finiteValue(view, b.upper - y);
  const double left = finiteValue(view, x - a.lower);
  const double bottom = finiteValue(view, y - b.lower);
  double numerator = finiteValue(view, q11 * right);
  numerator = finiteValue(view, numerator * top);
  double term = finiteValue(view, q21 * left);
  term = finiteValue(view, term * top);
  numerator = finiteValue(view, numerator + term);
  term = finiteValue(view, q12 * right);
  term = finiteValue(view, term * bottom);
  numerator = finiteValue(view, numerator + term);
  term = finiteValue(view, q22 * left);
  term = finiteValue(view, term * bottom);
  numerator = finiteValue(view, numerator + term);
  return finiteValue(view, numerator / finiteValue(view, dx * dy));
}

__device__ inline double evaluateTable(DeviceView view, const Table& table, double slew, double load)
{
  if (table.value_size == 0)
    return 0.0;
  if (table.dimensions == 0)
    return finiteValue(view, sample(view, table, 0));
  const bool swapped = table.variable1 == 2 || table.variable1 == 3;
  const double x = swapped ? load : slew;
  const double y = swapped ? slew : load;
  const AxisRegion a = axisRegion(view, table, 0, x);
  if (table.dimensions == 1)
    return linear(view, a.lower, a.upper, sample(view, table, a.index), sample(view, table, a.index + 1), x);
  const AxisRegion b = axisRegion(view, table, 1, y);
  const Index width = table.axis_size[1];
  if (width != 0 && a.index + 1 > (absent - (b.index + 1)) / width)
    return numeric(view);
  return bilinear(view, sample(view, table, a.index * width + b.index), sample(view, table, a.index * width + b.index + 1),
                  sample(view, table, (a.index + 1) * width + b.index), sample(view, table, (a.index + 1) * width + b.index + 1), a, b, x, y);
}

__device__ inline double lookupTable(DeviceView view, const Table& table, const Child& child, double slew, double load)
{
  if (!table.present || !isfinite(slew) || slew < 0.0 || !isfinite(load) || load < 0.0 || !isfinite(child.time_scale) || child.time_scale <= 0.0
      || !isfinite(child.capacitance_scale) || child.capacitance_scale <= 0.0)
    return numeric(view);
  const double delta = finiteValue(view, child.capacitance_scale - 1.0);
  if (!(fabs(delta) < 1e-6)) {
    const double product = load * child.capacitance_scale;
    if (!isfinite(product) || product < 0.0 || product > INT_MAX)
      return numeric(view);
    const double rounded = ceil(product);
    if (!isfinite(rounded) || rounded < 0.0 || rounded > INT_MAX)
      return numeric(view);
    load = static_cast<int>(rounded);
  }
  slew = finiteValue(view, slew * child.time_scale);
  return finiteValue(view, evaluateTable(view, table, slew, load) / child.time_scale);
}

__device__ inline double lookupCheckTable(DeviceView view, const Table& table, const Child& child, double clock_slew, double data_slew)
{
  if (!table.present || !isfinite(clock_slew) || clock_slew < 0.0 || !isfinite(data_slew) || data_slew < 0.0
      || !isfinite(child.time_scale) || child.time_scale <= 0.0)
    return numeric(view);
  const double clock = finiteValue(view, clock_slew * child.time_scale);
  const double data = finiteValue(view, data_slew * child.time_scale);
  return finiteValue(view, evaluateTable(view, table, clock, data) / child.time_scale);
}

}  // namespace ista::csr::cuda_detail
