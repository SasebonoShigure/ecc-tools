#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "CsrCudaTableLookup.cuh"
#include "CsrTableLookup.hpp"

namespace {
using namespace ista::csr::cuda_detail;

struct Case
{
  Table table;
  Child child;
  double slew;
  double load;
};

struct Inputs
{
  std::vector<Case> cases;
  std::vector<double> data;
  std::vector<std::optional<double>> expected;

  void add(const ista::csr::TimingTableModel& table, double slew, double load, double time_scale = 1.0, double capacitance_scale = 1.0)
  {
    Case value{};
    value.table.present = table.present;
    value.table.variable1 = static_cast<std::uint8_t>(table.variable1);
    value.table.dimensions = static_cast<std::uint8_t>(table.axes.size());
    for (std::size_t axis = 0; axis < table.axes.size(); ++axis) {
      value.table.axis_offset[axis] = data.size();
      value.table.axis_size[axis] = table.axes[axis].size();
      data.insert(data.end(), table.axes[axis].begin(), table.axes[axis].end());
    }
    value.table.value_offset = data.size();
    value.table.value_size = table.values.size();
    data.insert(data.end(), table.values.begin(), table.values.end());
    value.child.time_scale = time_scale;
    value.child.capacitance_scale = capacitance_scale;
    value.slew = slew;
    value.load = load;
    expected.push_back(ista::csr::lookupCsrTimingTable(table, slew, load, time_scale, capacitance_scale));
    cases.push_back(value);
  }
};

void addGeneratedCases(Inputs& inputs)
{
  std::mt19937 random(718);
  std::uniform_real_distribution<double> distribution(0.0, 5.0);
  for (int index = 0; index < 2048; ++index) {
    ista::csr::TimingTableModel table;
    table.present = index % 17 != 0;
    table.variable1 = static_cast<ista::csr::TableVariable>(index % 4);
    const int dimensions = index % 3;
    for (int dimension = 0; dimension < dimensions; ++dimension) {
      std::vector<double> axis;
      const int size = (index / (dimension + 1)) % 5;
      for (int sample = 0; sample < size; ++sample)
        axis.push_back(index % 11 == 0 ? 1.0 : sample * 0.5);
      table.axes.push_back(std::move(axis));
    }
    for (int sample = 0; sample < index % 16; ++sample)
      table.values.push_back(distribution(random));
    const double time_scale = index % 47 == 0 ? 1e308 : (index % 5 == 0 ? 1000.0 : 1.0);
    const double capacitance_scale = index % 7 == 0 ? 0.13 : 1.0;
    const double slew = distribution(random);
    double load = distribution(random);
    if (index % 41 == 0)
      load = 1e308;
    inputs.add(table, slew, load, time_scale, capacitance_scale);
  }
}

void addNumericFailureCases(Inputs& inputs)
{
  // These failures are deliberate, independent of the generated distribution.
  const std::size_t begin = inputs.cases.size();
  ista::csr::TimingTableModel table;
  table.present = true;
  table.values = {1.0};
  inputs.add(table, -1.0, 0.0);
  inputs.add(table, 1.0, 1e308, 1.0, 2.0);  // Capacitance product overflow.
  inputs.add(table, 1e308, 0.0, 2.0);       // Slew product overflow.
  inputs.add(table, 0.0, 0.0, 0.0);         // Invalid time scale.
  inputs.add(table, 0.0, 0.0, 1.0, 0.0);    // Invalid capacitance scale.
  table.values = {1e308};
  inputs.add(table, 0.0, 0.0, 1e-308);  // Result unit conversion overflow.
  table.values = {std::numeric_limits<double>::infinity()};
  inputs.add(table, 0.0, 0.0);
  table.axes = {{-1e308, 1e308}};
  table.values = {0.0, 1.0};
  inputs.add(table, 0.0, 0.0);  // Axis delta overflow.
  table.axes = {{0.0, 1.0}};
  table.values = {-1e308, 1e308};
  inputs.add(table, 0.5, 0.0);  // Interpolation value delta overflow.
  for (std::size_t index = begin; index < inputs.expected.size(); ++index)
    if (inputs.expected[index])
      throw std::runtime_error("explicit numeric failure did not fail in CPU oracle");
}

void check(cudaError_t error)
{
  if (error != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(error));
}

struct DeviceDelete
{
  void operator()(void* pointer) const { cudaFree(pointer); }
};

template <typename T>
std::unique_ptr<T, DeviceDelete> allocate(std::size_t count)
{
  T* pointer = nullptr;
  check(cudaMallocManaged(&pointer, count * sizeof(T)));
  return std::unique_ptr<T, DeviceDelete>(pointer);
}

__global__ void evaluate(const Case* cases, const double* data, double* results, Status* statuses, std::size_t count)
{
  const std::size_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= count)
    return;
  DeviceView view;
  view.table_data = data;
  view.status = statuses + index;
  const Case& value = cases[index];
  results[index] = lookupTable(view, value.table, value.child, value.slew, value.load);
}

void compare(const Inputs& inputs, const double* results, const Status* statuses)
{
  double max_absolute_error = 0.0;
  double max_scaled_error = 0.0;
  for (std::size_t index = 0; index < inputs.cases.size(); ++index) {
    const bool present = statuses[index] == Status::success;
    if (present != inputs.expected[index].has_value() || (!present && statuses[index] != Status::numeric))
      throw std::runtime_error("CUDA/CPU table result presence or failure category differs at case " + std::to_string(index));
    if (!present)
      continue;
    const double expected = *inputs.expected[index];
    const double error = std::abs(results[index] - expected);
    const double scaled = error / std::max(1.0, std::abs(expected));
    max_absolute_error = std::max(max_absolute_error, error);
    max_scaled_error = std::max(max_scaled_error, scaled);
    // Tight double-precision tolerance allows CPU/GPU arithmetic rounding,
    // including values near zero; it does not relax result-presence checks.
    if (!std::isfinite(results[index]) || scaled > 1e-12)
      throw std::runtime_error("CUDA/CPU table value differs at case " + std::to_string(index));
  }
  std::printf("CUDA/CPU NLDM seed=718 cases=%zu max_absolute_error=%.17g max_scaled_error=%.17g tolerance=1e-12\n", inputs.cases.size(), max_absolute_error,
              max_scaled_error);
}
}  // namespace

int main()
{
  try {
    Inputs inputs;
    addGeneratedCases(inputs);
    addNumericFailureCases(inputs);
    auto cases = allocate<Case>(inputs.cases.size());
    auto data = allocate<double>(inputs.data.size());
    auto results = allocate<double>(inputs.cases.size());
    auto statuses = allocate<Status>(inputs.cases.size());
    std::copy(inputs.cases.begin(), inputs.cases.end(), cases.get());
    std::copy(inputs.data.begin(), inputs.data.end(), data.get());
    std::fill_n(statuses.get(), inputs.cases.size(), Status::success);
    evaluate<<<(inputs.cases.size() + 127) / 128, 128>>>(cases.get(), data.get(), results.get(), statuses.get(), inputs.cases.size());
    check(cudaGetLastError());
    check(cudaDeviceSynchronize());
    compare(inputs, results.get(), statuses.get());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
