// Copyright (c) 2023-2025 Peng Cheng Laboratory
// Licensed under Mulan PSL v2.
#pragma once

#include <cmath>

#if defined(__CUDACC__)
#define ISTA_RC_HD __host__ __device__
#else
#define ISTA_RC_HD
#endif

namespace ista::csr {

enum class CsrRcEvaluationStatus { kSuccess, kUnavailable, kNumericFailure };

// Fixed native order-five eigensystem; no dynamic allocation in a node worker.
struct CsrArnoldiSpectrum
{
  int order{0};
  double eigenvalues[5]{};
  double eigenvectors[5][5]{};
  double poles[5]{};
};

ISTA_RC_HD inline CsrRcEvaluationStatus evaluateCsrArnoldiSpectrum(
    int order, const double* diagonal, const double* off_diagonal,
    double total_capacitance, double drive_resistance, CsrArnoldiSpectrum& out)
{
  out = {};
  if (order <= 0 || order > 5 || !diagonal || (order > 1 && !off_diagonal))
    return CsrRcEvaluationStatus::kUnavailable;
  if (!std::isfinite(total_capacitance) || !std::isfinite(drive_resistance)
      || total_capacitance <= 0.0 || drive_resistance <= 0.0)
    return CsrRcEvaluationStatus::kNumericFailure;
  double off[5]{};
  for (int i = 0; i < order; ++i) {
    out.eigenvalues[i] = diagonal[i];
    out.eigenvectors[i][i] = 1.0;
    if (!std::isfinite(diagonal[i]))
      return CsrRcEvaluationStatus::kNumericFailure;
    if (i + 1 < order) {
      off[i + 1] = off_diagonal[i];
      if (!std::isfinite(off[i + 1]))
        return CsrRcEvaluationStatus::kNumericFailure;
    }
  }
  // This term depends on the current driver's slew through drive resistance.
  out.eigenvalues[0] += drive_resistance * total_capacitance;
  if (!std::isfinite(out.eigenvalues[0]))
    return CsrRcEvaluationStatus::kNumericFailure;

  for (int high = order - 1; high >= 1; --high) {
    int iterations = 0;
    while (std::abs(off[high]) > 1e-9) {
      if (iterations++ == 20)
        return CsrRcEvaluationStatus::kUnavailable;
      double g = (out.eigenvalues[high - 1] - out.eigenvalues[high]) / (2.0 * off[high]);
      double r = std::sqrt(1.0 + g * g);
      g = out.eigenvalues[0] - out.eigenvalues[high] + off[high] / (g + (g < 0.0 ? -r : r));
      double sine = 1.0, cosine = 1.0, shift = 0.0;
      int i = 1;
      for (; i <= high; ++i) {
        double f = sine * off[i];
        const double b = cosine * off[i];
        off[i - 1] = r = std::sqrt(f * f + g * g);
        if (r == 0.0) {
          out.eigenvalues[i - 1] -= shift;
          off[0] = 0.0;
          break;
        }
        sine = f / r;
        cosine = g / r;
        g = out.eigenvalues[i - 1] - shift;
        r = (out.eigenvalues[i] - g) * sine + 2.0 * cosine * b;
        shift = sine * r;
        out.eigenvalues[i - 1] = g + shift;
        g = cosine * r - b;
        for (int j = 0; j < order; ++j) {
          f = out.eigenvectors[i - 1][j];
          out.eigenvectors[i - 1][j] = sine * out.eigenvectors[i][j] + cosine * f;
          out.eigenvectors[i][j] = cosine * out.eigenvectors[i][j] - sine * f;
        }
      }
      if (r == 0.0 && i <= high)
        continue;
      out.eigenvalues[high] -= shift;
      off[high] = g;
      off[0] = 0.0;
      if (!std::isfinite(g) || !std::isfinite(out.eigenvalues[high]))
        return CsrRcEvaluationStatus::kNumericFailure;
    }
  }
  for (int i = 0; i + 1 < order; ++i) {
    int maximum = i;
    for (int j = i + 1; j < order; ++j)
      if (out.eigenvalues[j] > out.eigenvalues[maximum])
        maximum = j;
    if (maximum == i)
      continue;
    const double value = out.eigenvalues[maximum];
    out.eigenvalues[maximum] = out.eigenvalues[i];
    out.eigenvalues[i] = value;
    for (int j = 0; j < order; ++j) {
      const double component = out.eigenvectors[maximum][j];
      out.eigenvectors[maximum][j] = out.eigenvectors[i][j];
      out.eigenvectors[i][j] = component;
    }
  }
  for (int i = 0; i < order; ++i) {
    if (!std::isfinite(out.eigenvalues[i]))
      return CsrRcEvaluationStatus::kNumericFailure;
    out.poles[i] = 1.0 / (out.eigenvalues[i] > 1e-5 ? out.eigenvalues[i] : 1e-5);
    for (int j = 0; j < order; ++j)
      if (!std::isfinite(out.eigenvectors[i][j]))
        return CsrRcEvaluationStatus::kNumericFailure;
  }
  out.order = order;
  return CsrRcEvaluationStatus::kSuccess;
}

// The terminal projection is immutable structure data. Each caller owns
// its residue array, so parallel sinks never update a shared driver cache.
ISTA_RC_HD inline CsrRcEvaluationStatus evaluateCsrArnoldiResidues(
    const CsrArnoldiSpectrum& spectrum, double sqrt_total_capacitance,
    const double* terminal_projection, double* residues)
{
  if (spectrum.order <= 0 || spectrum.order > 5 || !terminal_projection || !residues)
    return CsrRcEvaluationStatus::kUnavailable;
  for (int i = 0; i < spectrum.order; ++i) {
    const double source = sqrt_total_capacitance * spectrum.eigenvectors[i][0];
    double terminal = 0.0;
    for (int j = 0; j < spectrum.order; ++j)
      terminal += spectrum.eigenvectors[i][j] * terminal_projection[j];
    residues[i] = source * terminal;
    if (!std::isfinite(residues[i]))
      return CsrRcEvaluationStatus::kNumericFailure;
  }
  return CsrRcEvaluationStatus::kSuccess;
}

}  // namespace ista::csr
#undef ISTA_RC_HD
