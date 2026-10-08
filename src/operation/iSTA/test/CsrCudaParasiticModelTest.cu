// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#include <cuda_runtime.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "CsrArnoldiSpectrum.hpp"
#include "CsrArnoldiWaveform.hpp"
#include "CsrArnoldiDriver.hpp"
#include "CsrDmpLoad.hpp"
#include "CsrRcDriverCache.hpp"
#include "CsrParasiticThreshold.hpp"

namespace {
using namespace ista::csr;
constexpr int kCases = 1025;
struct Sample
{
  CsrArnoldiSpectrum spectrum;
  double residues[5]{};
  CsrArnoldiCrossings crossings;
  CsrRcEvaluationStatus spectrum_status;
  CsrRcEvaluationStatus residue_status;
  CsrRcEvaluationStatus crossing_status;
  CsrArnoldiDriverResult driver;
  CsrRcEvaluationStatus driver_status;
  CsrRcEvaluationStatus wide_status;
  CsrArnoldiCrossings wide_crossings;
  CsrDmpDriverResult dmp;
  CsrRcEvaluationStatus dmp_status;
  CsrRcEvaluationStatus dmp_load_status;
  double boundary_ramp{0.0};
  bool boundary_overflow_rejected{false};
  bool cache_ok{false};
  bool fallback_ok{false};
  CsrRcEvaluationStatus port_status, threshold_status;
  double port_wire{0.0}, port_slew{0.0}, adjusted_wire{0.0}, adjusted_slew{0.0};
  double dmp_wire{0.0};
  double dmp_slew{0.0};
};
void checked(cudaError_t code)
{
  if (code != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(code));
}

__host__ __device__ int matrix(int sample, double* diagonal, double* off)
{
  if (sample % 2 == 0) {
    diagonal[0] = 0.045; diagonal[1] = 0.015;
    off[0] = std::sqrt(0.000675);
    return 2;
  }
  for (int i = 0; i < 5; ++i) {
    diagonal[i] = 0.02 + 0.01 * i;
    if (i < 4) off[i] = 0.004 * (i + 1);
  }
  return 5;
}

struct LinearTable
{
  double slew_offset;
  __host__ __device__ double delay(double capacitance) const { return 0.03 + 2.0 * capacitance; }
  __host__ __device__ double slew(double capacitance) const { return slew_offset + 3.0 * capacitance; }
};

struct UnavailableTables
{
  bool nonfinite;
  __host__ __device__ double delay(double) const { return 1.0; }
  __host__ __device__ double slew(double) const { return nonfinite ? nan("") : -0.5; }
};

__device__ bool fallbackScenario()
{
  CsrDmpDriverResult result;
  const auto thresholds = csrDmpThresholds(50.0, 10.0, 90.0, 0.8);
  if (evaluateCsrDmpDriver(0.15, 0.0, 0.0, thresholds, UnavailableTables{false}, result)
          != CsrRcEvaluationStatus::kUnavailable
      || evaluateCsrDmpDriver(0.15, 0.0, 0.0, thresholds, UnavailableTables{true}, result)
          != CsrRcEvaluationStatus::kNumericFailure)
    return false;
  auto finite_invalid = thresholds;
  finite_invalid.threshold = 0.0;
  return evaluateCsrDmpDriver(0.15, 0.0, 0.0, finite_invalid, LinearTable{0.1}, result)
      == CsrRcEvaluationStatus::kUnavailable;
}

__device__ bool cacheScenario(double slew)
{
  CsrRcTimingRecord records[4]{};
  CsrRcTimingRow rows[2]{{0, 2, 0}, {2, 2, 0}};
  CsrRcDriverLane driver{0, 2};
  CsrRcTimingRecord pending;
  pending.input_slew = 0.1;
  pending.family = CsrRcDriverFamily::arnoldi;
  pending.arnoldi.driver_slew = slew;
  std::uint64_t first, second, dmp;
  if (!appendCsrRcTimingRecord(records, rows[0], driver, pending, first)
      || !appendCsrRcTimingRecord(records, rows[1], driver, pending, second)
      || findCsrRcTimingRecord(records, rows[0], 0.1) != first)
    return false;
  selectCsrRcDriverRecords(records, rows, driver, slew);
  if (driver.selected_arnoldi != second || driver.miss_sequence != 2)
    return false;
  pending.family = CsrRcDriverFamily::dmp;
  pending.input_slew = 0.2;
  pending.dmp.driver_slew = slew;
  if (!appendCsrRcTimingRecord(records, rows[0], driver, pending, dmp))
    return false;
  selectCsrRcDriverRecords(records, rows, driver, slew);
  if (driver.selected_arnoldi != second || driver.selected_dmp != dmp)
    return false;
  pending.input_slew = 0.3;
  if (appendCsrRcTimingRecord(records, rows[0], driver, pending, dmp)
      || driver.miss_sequence != 3)
    return false;
  selectCsrRcDriverRecords(records, rows, driver, slew + 1.0);
  return driver.selected_arnoldi == kCsrRcAbsent && driver.selected_dmp == kCsrRcAbsent;
}

__global__ void evaluate(Sample* output, int count)
{
  for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < count; i += blockDim.x * gridDim.x) {
    double diagonal[5]{}, off[5]{};
    const int order = matrix(i, diagonal, off);
    const double resistance = 0.1 + static_cast<double>(i) / 100.0;
    Sample result{};
    const auto boundary = csrArnoldiThresholds(30.0, 70.0, 0.5);
    const double cap = 0.013552116249999998;
    const double slew = i % 2 ? 0.22741790352985375 : 0.22741790352985364;
    const double drive = boundary.slew_derate * slew / (boundary.voltage_log * cap);
    result.boundary_ramp = csrArnoldiDriverRamp(slew, drive, cap, boundary);
    auto overflow = boundary; overflow.slew_derate = 2.0;
    result.boundary_overflow_rejected = !isfinite(csrArnoldiDriverRamp(DBL_MAX, 1.0, cap, overflow));
    result.cache_ok = cacheScenario(resistance);
    result.fallback_ok = fallbackScenario();
    result.spectrum_status = evaluateCsrArnoldiSpectrum(order, diagonal, off, 0.04, resistance, result.spectrum);
    const double projection[5]{5.0, 1.0, 2.0, 3.0, 4.0};
    result.residue_status = evaluateCsrArnoldiResidues(result.spectrum, 0.2, projection, result.residues);
    const double pole[1]{1.0 / resistance}, residue[1]{1.0};
    result.crossing_status = evaluateCsrArnoldiCrossings(resistance, 1, pole, residue,
        csrArnoldiThresholds(10.0, 90.0, 0.8), result.crossings);
    const double lumped_diagonal[1]{0.0}, source_projection[1]{5.0};
    const CsrArnoldiModelView lumped{1, lumped_diagonal, nullptr, source_projection, 0.04, 0.2};
    result.driver_status = evaluateCsrArnoldiDriver(lumped, csrArnoldiThresholds(10.0, 90.0, 0.8),
        LinearTable{i % 2 ? 0.02 : 0.2}, result.driver);
    const auto dmp_thresholds = csrDmpThresholds(50.0, 10.0, 90.0, 0.8);
    result.dmp_status = evaluateCsrDmpDriver(i % 3 == 0 ? 0.0 : 0.01, i % 3 == 1 ? 0.0 : resistance,
        0.03, dmp_thresholds, LinearTable{i % 2 ? 0.02 : 0.2}, result.dmp);
    const double load_poles[2]{2.0, 5.0}, load_residues[2]{10.0 / 3.0, -10.0 / 3.0};
    result.dmp_load_status = evaluateCsrDmpLoad(2, load_poles, load_residues, result.dmp.driver_slew,
        dmp_thresholds, result.dmp_wire, result.dmp_slew);
    const CsrArnoldiModelView port_model{order, diagonal, off, projection, 0.04, 0.2};
    result.port_status = evaluateCsrArnoldiInputPort(port_model, projection,
        csrArnoldiThresholds(35.0, 85.0, 0.6), i % 2 ? -resistance : resistance,
        result.port_wire, result.port_slew);
    result.adjusted_wire = result.port_wire;
    result.adjusted_slew = std::abs(result.port_slew);
    result.threshold_status = adjustCsrParasiticLoadThreshold(
        {0.2, 0.8, 35.0 * 0.01, 0.8}, {0.15, 0.85, 0.45, i % 3 ? 0.6 : 0.0},
        i % 2, result.adjusted_wire, result.adjusted_slew);
    result.wide_status = CsrRcEvaluationStatus::kSuccess;
    if (i == 0) {
      const double wide_poles[2]{1e-8, 10.0}, wide_residues[2]{0.4, 0.6};
      result.wide_status = evaluateCsrArnoldiCrossings(1.0, 2, wide_poles, wide_residues,
          csrArnoldiThresholds(10.0, 90.0, 0.8), result.wide_crossings);
    }
    output[i] = result;
  }
}

void near(double actual, double expected, double tolerance)
{
  if (!std::isfinite(actual) || !std::isfinite(expected) || std::abs(actual - expected) > tolerance)
    throw std::runtime_error("device RC numeric mismatch");
}

double crossing(double target, double tau)
{
  double lower = 0.0, upper = 100.0 * tau;
  for (int i = 0; i < 100; ++i) {
    const double mid = (lower + upper) / 2.0;
    const double voltage = mid < tau ? 1.0 - mid / tau - std::expm1(-mid / tau)
                                    : -std::expm1(-1.0) * std::exp(1.0 - mid / tau);
    if (voltage > target) lower = mid;
    else upper = mid;
  }
  return (lower + upper) / 2.0;
}

void verify(const std::vector<Sample>& samples)
{
  for (int i = 0; i < kCases; ++i) {
    const auto& result = samples[i];
    near(result.boundary_ramp, 0.10218581909453474, 1e-12);
    if (!result.boundary_overflow_rejected) throw std::runtime_error("ramp roundoff handling hid overflow");
    if (!result.cache_ok || !result.fallback_ok) throw std::runtime_error("device RC cache/fallback semantics");
    double diagonal[5]{}, off[5]{};
    const int order = matrix(i, diagonal, off);
    const double resistance = 0.1 + static_cast<double>(i) / 100.0;
    const double moment = diagonal[0] + off[0] / 5.0;
    diagonal[0] += 0.04 * resistance;
    if (result.spectrum_status != CsrRcEvaluationStatus::kSuccess || result.spectrum.order != order
        || result.residue_status != CsrRcEvaluationStatus::kSuccess
        || result.crossing_status != CsrRcEvaluationStatus::kSuccess)
      throw std::runtime_error("device RC solve failed");
    if (result.driver_status != CsrRcEvaluationStatus::kSuccess
        || result.wide_status != CsrRcEvaluationStatus::kSuccess)
      throw std::runtime_error("device driver or search bound failed");
    CsrDmpDriverResult host_dmp;
    const auto dmp_thresholds = csrDmpThresholds(50.0, 10.0, 90.0, 0.8);
    const auto host_status = evaluateCsrDmpDriver(i % 3 == 0 ? 0.0 : 0.01, i % 3 == 1 ? 0.0 : resistance,
        0.03, dmp_thresholds, LinearTable{i % 2 ? 0.02 : 0.2}, host_dmp);
    if (host_status != CsrRcEvaluationStatus::kSuccess || result.dmp_status != host_status)
      throw std::runtime_error("device DMP driver failed");
    near(result.dmp.gate_delay, host_dmp.gate_delay, 1e-6);
    near(result.dmp.driver_slew, host_dmp.driver_slew, 1e-6);
    near(result.dmp.effective_capacitance, host_dmp.effective_capacitance, 1e-6);
    const double load_poles[2]{2.0, 5.0}, load_residues[2]{10.0 / 3.0, -10.0 / 3.0};
    double host_wire = 0.0, host_slew = 0.0;
    const auto load_status = evaluateCsrDmpLoad(2, load_poles, load_residues, host_dmp.driver_slew,
        dmp_thresholds, host_wire, host_slew);
    if (load_status != CsrRcEvaluationStatus::kSuccess || result.dmp_load_status != load_status)
      throw std::runtime_error("device DMP load failed");
    near(result.dmp_wire, host_wire, 1e-6);
    near(result.dmp_slew, host_slew, 1e-6);
    if (i == 0) {
      const double expected = 1.0 - std::log(0.1 / (0.4 * (1.0 - std::exp(-1e-8)) / 1e-8)) / 1e-8;
      near(result.wide_crossings.lower_time, expected, 1e-6);
    }
    if (result.port_status != CsrRcEvaluationStatus::kSuccess || result.threshold_status != CsrRcEvaluationStatus::kSuccess)
      throw std::runtime_error("device RC threshold status failed");
    const double port_slew = resistance + std::log((85.0 * 0.01) / (35.0 * 0.01)) / 0.6 * moment;
    const double wire = std::log(2.0) * moment;
    near(result.port_wire, wire, 1e-12);
    near(result.port_slew, i % 2 ? -port_slew : port_slew, 1e-12);
    const double correction = i % 3 ? port_slew * ((0.45 - 35.0 * 0.01) / (0.8 - 0.2)) : 0.0;
    near(result.adjusted_wire, i % 2 ? wire - correction : wire + correction, 1e-12);
    near(result.adjusted_slew, i % 3 ? port_slew * (((0.85 - 0.15) / 0.6) / ((0.8 - 0.2) / 0.8)) : port_slew, 1e-12);
    near(result.driver.effective_capacitance, 0.04, 1e-12);
    near(result.driver.gate_delay, 0.11, 1e-6);
    double trace = 0.0, diagonal_sum = 0.0, residue_sum = 0.0;
    for (int j = 0; j < order; ++j) {
      trace += result.spectrum.eigenvalues[j];
      diagonal_sum += diagonal[j];
      residue_sum += result.residues[j];
      near(result.spectrum.poles[j], 1.0 / result.spectrum.eigenvalues[j], 1e-8);
      if (j && result.spectrum.eigenvalues[j] > result.spectrum.eigenvalues[j - 1])
        throw std::runtime_error("unordered eigenvalues");
      const auto* v = result.spectrum.eigenvectors[j];
      for (int k = 0; k < order; ++k) {
        double av = diagonal[k] * v[k];
        if (k) av += off[k - 1] * v[k - 1];
        if (k + 1 < order) av += off[k] * v[k + 1];
        // Native QL stops when the off-diagonal falls below 1e-9.
        near(av, result.spectrum.eigenvalues[j] * v[k], 2e-9);
        double dot = 0.0;
        for (int m = 0; m < order; ++m)
          dot += v[m] * result.spectrum.eigenvectors[k][m];
        near(dot, j == k ? 1.0 : 0.0, 1e-10);
      }
    }
    near(trace, diagonal_sum, 1e-10);
    near(residue_sum, 1.0, 1e-10);
    if (order == 2) {
      const double gap = std::sqrt((diagonal[0] - diagonal[1]) * (diagonal[0] - diagonal[1]) + 4.0 * off[0] * off[0]);
      near(result.spectrum.eigenvalues[0], (diagonal_sum + gap) / 2.0, 1e-10);
      near(result.spectrum.eigenvalues[1], (diagonal_sum - gap) / 2.0, 1e-10);
    }
    near(result.crossings.upper_time, crossing(0.9, resistance), 1e-6);
    near(result.crossings.middle_time, crossing(0.5, resistance), 1e-6);
    near(result.crossings.lower_time, crossing(0.1, resistance), 1e-6);
  }
}

void equal(const Sample& a, const Sample& b)
{
  near(a.boundary_ramp, b.boundary_ramp, 0.0);
  if (a.spectrum_status != b.spectrum_status || a.residue_status != b.residue_status
      || a.crossing_status != b.crossing_status || a.spectrum.order != b.spectrum.order)
    throw std::runtime_error("launch shape changed RC status");
  for (int j = 0; j < 5; ++j) {
    near(a.spectrum.eigenvalues[j], b.spectrum.eigenvalues[j], 0.0);
    near(a.spectrum.poles[j], b.spectrum.poles[j], 0.0);
    near(a.residues[j], b.residues[j], 0.0);
    for (int k = 0; k < 5; ++k)
      near(a.spectrum.eigenvectors[j][k], b.spectrum.eigenvectors[j][k], 0.0);
  }
  if (a.driver_status != b.driver_status || a.wide_status != b.wide_status)
    throw std::runtime_error("launch shape changed driver status");
  if (a.dmp_status != b.dmp_status || a.dmp_load_status != b.dmp_load_status)
    throw std::runtime_error("launch shape changed DMP status");
  near(a.dmp.gate_delay, b.dmp.gate_delay, 0.0);
  near(a.dmp.driver_slew, b.dmp.driver_slew, 0.0);
  near(a.dmp.effective_capacitance, b.dmp.effective_capacitance, 0.0);
  if (a.port_status != b.port_status || a.threshold_status != b.threshold_status)
    throw std::runtime_error("RC threshold status changed with launch shape");
  near(a.port_wire, b.port_wire, 0.0); near(a.port_slew, b.port_slew, 0.0);
  near(a.adjusted_wire, b.adjusted_wire, 0.0); near(a.adjusted_slew, b.adjusted_slew, 0.0);
  near(a.dmp_wire, b.dmp_wire, 0.0);
  near(a.dmp_slew, b.dmp_slew, 0.0);
  near(a.driver.drive_resistance, b.driver.drive_resistance, 0.0);
  near(a.driver.driver_ramp, b.driver.driver_ramp, 0.0);
  near(a.driver.effective_capacitance, b.driver.effective_capacitance, 0.0);
  near(a.driver.table_delay, b.driver.table_delay, 0.0);
  near(a.driver.reference_time, b.driver.reference_time, 0.0);
  near(a.driver.gate_delay, b.driver.gate_delay, 0.0);
  near(a.driver.driver_slew, b.driver.driver_slew, 0.0);
  for (int j = 0; j < 5; ++j) {
    near(a.driver.spectrum.eigenvalues[j], b.driver.spectrum.eigenvalues[j], 0.0);
    near(a.driver.spectrum.poles[j], b.driver.spectrum.poles[j], 0.0);
    for (int k = 0; k < 5; ++k)
      near(a.driver.spectrum.eigenvectors[j][k], b.driver.spectrum.eigenvectors[j][k], 0.0);
  }
  near(a.wide_crossings.upper_time, b.wide_crossings.upper_time, 0.0);
  near(a.wide_crossings.middle_time, b.wide_crossings.middle_time, 0.0);
  near(a.wide_crossings.lower_time, b.wide_crossings.lower_time, 0.0);
  near(a.crossings.upper_time, b.crossings.upper_time, 0.0);
  near(a.crossings.middle_time, b.crossings.middle_time, 0.0);
  near(a.crossings.lower_time, b.crossings.lower_time, 0.0);
}
}  // namespace

void testCudaParasiticPacking();

int main()
{
  Sample* device = nullptr;
  try {
    checked(cudaMalloc(&device, sizeof(Sample) * kCases));
    std::vector<Sample> first(kCases), second(kCases);
    // Initialize transport padding as well. All result fields are checked;
    // the NaN/-1 sentinel catches a missing write rather than supplying zeros.
    checked(cudaMemset(device, 0xff, sizeof(Sample) * kCases));
    evaluate<<<5, 256>>>(device, kCases);
    checked(cudaGetLastError());
    checked(cudaMemcpy(first.data(), device, sizeof(Sample) * kCases, cudaMemcpyDeviceToHost));
    verify(first);
    checked(cudaMemset(device, 0xff, sizeof(Sample) * kCases));
    evaluate<<<19, 64>>>(device, kCases);
    checked(cudaGetLastError());
    checked(cudaMemcpy(second.data(), device, sizeof(Sample) * kCases, cudaMemcpyDeviceToHost));
    verify(second);
    for (int i = 0; i < kCases; ++i) equal(first[i], second[i]);
    checked(cudaFree(device));
    device = nullptr;
    testCudaParasiticPacking();
    std::cout << "CUDA RC spectrum/residue/waveform passed\n";
    return 0;
  } catch (const std::exception& error) {
    if (device) (void) cudaFree(device);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
